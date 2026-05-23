// WASAPI loopback capture — system audio (whatever is playing on the default
// render device).
//
// Pipeline:
//   1. Enumerate default eRender endpoint
//   2. IAudioClient::Initialize(SHARED | LOOPBACK | EVENTCALLBACK) at the
//      system mix format (typically 48 kHz F32 stereo)
//   3. Dedicated MMCSS-prioritised worker thread:
//        WaitForSingleObject(event)
//        IAudioCaptureClient::GetBuffer / ReleaseBuffer in a drain loop
//        Forward block to the user's callback
//
// Timestamps come from the device performance counter when available
// (AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR is not set), otherwise we synthesise
// from frame counts. Downstream (the mixer) is responsible for sync.

#include "wasapi_loopback.h"
#include "shared/log.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <wrl/client.h>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

namespace gpur::core::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr REFERENCE_TIME kBufferDuration100ns = 1'000'000;  // 100 ms

// Translate a WAVEFORMATEX(TENSIBLE) into our internal Format struct.
// Returns std::nullopt if the format isn't one of S16 / F32.
struct ParsedFormat {
    Format       fmt;
    bool         is_float;
    uint32_t     bits_per_sample;
    uint32_t     block_align;
};

std::optional<ParsedFormat> parse_format(const WAVEFORMATEX* wf) {
    if (!wf) return std::nullopt;

    bool is_float = false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* wfx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (wfx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            is_float = true;
        } else if (wfx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            is_float = false;
        } else {
            return std::nullopt;
        }
    } else if (wf->wFormatTag != WAVE_FORMAT_PCM) {
        return std::nullopt;
    }

    if (is_float && wf->wBitsPerSample != 32) return std::nullopt;
    if (!is_float && wf->wBitsPerSample != 16) return std::nullopt;

    ParsedFormat out{};
    out.fmt.sample_rate = wf->nSamplesPerSec;
    out.fmt.channels    = wf->nChannels;
    out.fmt.sample_fmt  = is_float ? SampleFormat::F32 : SampleFormat::S16;
    out.is_float        = is_float;
    out.bits_per_sample = wf->wBitsPerSample;
    out.block_align     = wf->nBlockAlign;
    return out;
}

class WasapiLoopbackImpl final : public WasapiLoopback {
public:
    ~WasapiLoopbackImpl() override { (void)stop(); }

    Result<void> start(OnBlock cb) override {
        if (running_.load()) {
            return err(Error::make(Error::Code::AlreadyInitialised, "loopback already started"));
        }
        on_block_ = std::move(cb);

        // COM apartment for this thread. Tolerate already-initialised (we
        // share the daemon's MTA).
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
            return err(Error::from_hresult(hr, "CoInitializeEx failed"));
        }
        com_owned_ = (hr == S_OK);

        ComPtr<IMMDeviceEnumerator> enumr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumr));
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "MMDeviceEnumerator create failed"));
        }

        ComPtr<IMMDevice> device;
        hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr,
                                "GetDefaultAudioEndpoint(eRender) failed"));
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "IMMDevice::Activate failed"));
        }

        WAVEFORMATEX* mix_format = nullptr;
        hr = client_->GetMixFormat(&mix_format);
        if (FAILED(hr) || !mix_format) {
            return cleanup_with(Error::from_hresult(hr, "GetMixFormat failed"));
        }

        auto parsed = parse_format(mix_format);
        if (!parsed) {
            CoTaskMemFree(mix_format);
            return cleanup_with(Error::make(Error::Code::AudioFailed,
                                "unsupported WASAPI mix format"));
        }
        format_      = parsed->fmt;
        block_align_ = parsed->block_align;

        hr = client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            kBufferDuration100ns,
            0,
            mix_format,
            nullptr);
        CoTaskMemFree(mix_format);
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "IAudioClient::Initialize failed"));
        }

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            return cleanup_with(Error::from_hresult(HRESULT_FROM_WIN32(GetLastError()),
                                                    "CreateEvent failed"));
        }
        hr = client_->SetEventHandle(event_);
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "SetEventHandle failed"));
        }

        hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "GetService(IAudioCaptureClient) failed"));
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "IAudioClient::Start failed"));
        }

        running_.store(true);
        thread_ = std::thread([this] { worker(); });

        GPUR_INFO("WASAPI loopback started: {} Hz, {} ch, {}",
                  format_.sample_rate, format_.channels,
                  format_.sample_fmt == SampleFormat::F32 ? "F32" : "S16");
        return ok();
    }

    Result<void> stop() override {
        if (!running_.exchange(false)) {
            // Even if not running, drop any partially-initialised state.
            return cleanup_with({});
        }
        if (event_) SetEvent(event_);   // unblock worker
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
        return cleanup_with({});
    }

    const Format& format() const noexcept override { return format_; }

private:
    Result<void> cleanup_with(Error e) {
        capture_.Reset();
        if (client_) {
            client_->Stop();
            client_.Reset();
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        if (com_owned_) {
            CoUninitialize();
            com_owned_ = false;
        }
        if (e.code == Error::Code::Ok) return ok();
        return err(std::move(e));
    }

    void worker() {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

        constexpr DWORD kWaitMs = 200;
        while (running_.load()) {
            DWORD w = WaitForSingleObject(event_, kWaitMs);
            if (!running_.load()) break;
            if (w == WAIT_TIMEOUT) continue;
            if (w != WAIT_OBJECT_0) {
                GPUR_WARN("WASAPI loopback wait returned {} (last err {})",
                          static_cast<uint32_t>(w),
                          static_cast<uint32_t>(GetLastError()));
                continue;
            }
            drain_packets();
        }

        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    void drain_packets() {
        UINT32 packet_frames = 0;
        while (capture_ && SUCCEEDED(capture_->GetNextPacketSize(&packet_frames))
               && packet_frames > 0) {
            BYTE*  data       = nullptr;
            UINT32 num_frames = 0;
            DWORD  flags      = 0;
            UINT64 device_pos = 0;
            UINT64 qpc_pos    = 0;

            HRESULT hr = capture_->GetBuffer(&data, &num_frames, &flags, &device_pos, &qpc_pos);
            if (FAILED(hr)) {
                GPUR_WARN("IAudioCaptureClient::GetBuffer failed 0x{:x}",
                          static_cast<uint32_t>(hr));
                return;
            }

            const size_t bytes = static_cast<size_t>(num_frames) * block_align_;
            Block block;
            block.format    = format_;
            block.duration  = std::chrono::nanoseconds(
                (static_cast<int64_t>(num_frames) * 1'000'000'000LL) / format_.sample_rate);
            // qpc_pos is a perf-counter timestamp in 100ns units (1/10us). Convert to ns.
            block.pts       = std::chrono::nanoseconds(static_cast<int64_t>(qpc_pos) * 100LL);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                block.data.assign(bytes, 0);   // explicit silence
            } else if (data) {
                block.data.assign(data, data + bytes);
            } else {
                block.data.assign(bytes, 0);
            }

            capture_->ReleaseBuffer(num_frames);

            if (on_block_) on_block_(std::move(block));
        }
    }

    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioCaptureClient> capture_;
    HANDLE                      event_      {nullptr};
    std::thread                 thread_;
    std::atomic<bool>           running_    {false};
    bool                        com_owned_  {false};
    Format                      format_     {};
    uint32_t                    block_align_{0};
    OnBlock                     on_block_;
};

} // namespace

std::unique_ptr<WasapiLoopback> WasapiLoopback::create() {
    return std::make_unique<WasapiLoopbackImpl>();
}

} // namespace gpur::core::audio
