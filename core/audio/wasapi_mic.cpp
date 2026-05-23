// Microphone capture via WASAPI.
//
// Almost identical to wasapi_loopback.cpp — the only differences are:
//   - eCapture endpoint (the mic) instead of eRender
//   - No AUDCLNT_STREAMFLAGS_LOOPBACK
//   - device_id parameter lets the UI pick a non-default capture device
//
// Kept as a separate file (rather than a templated common base) because the
// WASAPI plumbing is small and the differences are clearer side-by-side.

#include "wasapi_mic.h"
#include "shared/log.h"
#include "shared/wstr.h"

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

// Mirrors parse_format in wasapi_loopback.cpp. We duplicate rather than share
// because forward-declaring WAVEFORMATEX in a shared header drags <ksmedia.h>
// everywhere and the function is 30 lines.
std::optional<Format> parse_format(const WAVEFORMATEX* wf, uint32_t& block_align_out) {
    if (!wf) return std::nullopt;

    bool is_float = false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* wfx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (wfx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)      is_float = true;
        else if (wfx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)        is_float = false;
        else return std::nullopt;
    } else if (wf->wFormatTag != WAVE_FORMAT_PCM) {
        return std::nullopt;
    }
    if (is_float && wf->wBitsPerSample != 32) return std::nullopt;
    if (!is_float && wf->wBitsPerSample != 16) return std::nullopt;

    Format f{};
    f.sample_rate    = wf->nSamplesPerSec;
    f.channels       = wf->nChannels;
    f.sample_fmt     = is_float ? SampleFormat::F32 : SampleFormat::S16;
    block_align_out  = wf->nBlockAlign;
    return f;
}

class WasapiMicImpl final : public WasapiMic {
public:
    explicit WasapiMicImpl(std::wstring device_id) : device_id_(std::move(device_id)) {}
    ~WasapiMicImpl() override { (void)stop(); }

    Result<void> start(OnBlock cb) override {
        if (running_.load()) {
            return err(Error::make(Error::Code::AlreadyInitialised, "mic already started"));
        }
        on_block_ = std::move(cb);

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
        if (device_id_.empty()) {
            hr = enumr->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
            if (FAILED(hr)) {
                return cleanup_with(Error::from_hresult(hr,
                                    "GetDefaultAudioEndpoint(eCapture) failed"));
            }
        } else {
            hr = enumr->GetDevice(device_id_.c_str(), &device);
            if (FAILED(hr)) {
                return cleanup_with(Error::from_hresult(hr,
                                    "GetDevice(specific mic) failed for " +
                                    wstring_to_utf8(device_id_)));
            }
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
        auto parsed = parse_format(mix_format, block_align_);
        if (!parsed) {
            CoTaskMemFree(mix_format);
            return cleanup_with(Error::make(Error::Code::AudioFailed, "unsupported mic format"));
        }
        format_ = *parsed;

        hr = client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            kBufferDuration100ns,
            0,
            mix_format,
            nullptr);
        CoTaskMemFree(mix_format);
        if (FAILED(hr)) {
            return cleanup_with(Error::from_hresult(hr, "IAudioClient::Initialize(mic) failed"));
        }

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            return cleanup_with(Error::from_hresult(HRESULT_FROM_WIN32(GetLastError()),
                                                    "CreateEvent failed"));
        }
        hr = client_->SetEventHandle(event_);
        if (FAILED(hr)) return cleanup_with(Error::from_hresult(hr, "SetEventHandle failed"));

        hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) return cleanup_with(Error::from_hresult(hr,
                                            "GetService(IAudioCaptureClient) failed"));

        hr = client_->Start();
        if (FAILED(hr)) return cleanup_with(Error::from_hresult(hr,
                                            "IAudioClient::Start(mic) failed"));

        running_.store(true);
        thread_ = std::thread([this] { worker(); });

        GPUR_INFO("WASAPI mic started: {} Hz, {} ch, {}",
                  format_.sample_rate, format_.channels,
                  format_.sample_fmt == SampleFormat::F32 ? "F32" : "S16");
        return ok();
    }

    Result<void> stop() override {
        if (!running_.exchange(false)) return cleanup_with({});
        if (event_) SetEvent(event_);
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
        return cleanup_with({});
    }

    const Format& format() const noexcept override { return format_; }

private:
    Result<void> cleanup_with(Error e) {
        capture_.Reset();
        if (client_) { client_->Stop(); client_.Reset(); }
        if (event_)  { CloseHandle(event_); event_ = nullptr; }
        if (com_owned_) { CoUninitialize(); com_owned_ = false; }
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
            if (w != WAIT_OBJECT_0) continue;
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
            if (FAILED(hr)) return;

            const size_t bytes = static_cast<size_t>(num_frames) * block_align_;
            Block block;
            block.format   = format_;
            block.duration = std::chrono::nanoseconds(
                (static_cast<int64_t>(num_frames) * 1'000'000'000LL) / format_.sample_rate);
            block.pts      = std::chrono::nanoseconds(static_cast<int64_t>(qpc_pos) * 100LL);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)      block.data.assign(bytes, 0);
            else if (data)                                block.data.assign(data, data + bytes);
            else                                          block.data.assign(bytes, 0);

            capture_->ReleaseBuffer(num_frames);

            if (on_block_) on_block_(std::move(block));
        }
    }

    std::wstring                device_id_;
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

std::unique_ptr<WasapiMic> WasapiMic::create(std::wstring_view device_id) {
    return std::make_unique<WasapiMicImpl>(std::wstring(device_id));
}

} // namespace gpur::core::audio
