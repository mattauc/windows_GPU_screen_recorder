// AAC-LC encoder using Media Foundation's AAC encoder MFT.
//
// Why MF and not libfdk-aac:
//   - Ships with Windows, no extra dep.
//   - Quality is decent for the bitrates we use (>= 96 kbps).
//   - We're already pulling in mfplat/mfuuid for other things.
//
// Flow:
//   1. MFStartup once per process.
//   2. MFTEnumEx for an audio encoder accepting MFAudioFormat_PCM input
//      and producing MFAudioFormat_AAC output.
//   3. Activate, set input/output media types, BEGIN_STREAMING.
//   4. Per push: convert F32->S16 if needed, build an IMFSample, ProcessInput,
//      drain ProcessOutput until NEED_MORE_INPUT.
//   5. drain(): END_OF_STREAM + COMMAND_DRAIN + final ProcessOutput loop.

#include "aac_encoder.h"
#include "shared/log.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace gpur::core::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr GUID kMftCategoryAudioEncoder = MFT_CATEGORY_AUDIO_ENCODER;

struct MfStartupGuard {
    MfStartupGuard()  { (void)MFStartup(MF_VERSION, MFSTARTUP_FULL); }
    ~MfStartupGuard() { (void)MFShutdown(); }
};

// Hold one MFStartup() for the lifetime of the process. The encoder may be
// constructed and destroyed many times across recordings; cheaper to keep
// MF alive than to startup/shutdown around each session.
MfStartupGuard& mf_startup() {
    static MfStartupGuard g;
    return g;
}

class AacEncoderImpl final : public AacEncoder {
public:
    ~AacEncoderImpl() override { (void)shutdown(); }

    Result<std::vector<uint8_t>> initialise(const Params& p, OnPacket cb) override {
        if (mft_) {
            return err(Error::make(Error::Code::AlreadyInitialised, "AAC encoder already initialised"));
        }
        params_   = p;
        on_packet_ = std::move(cb);
        (void)mf_startup();

        if (p.sample_rate != 44100 && p.sample_rate != 48000) {
            return err(Error::make(Error::Code::InvalidArgument,
                                    "AAC MFT only supports 44.1 or 48 kHz"));
        }
        if (p.channels != 1 && p.channels != 2) {
            return err(Error::make(Error::Code::InvalidArgument,
                                    "AAC MFT only supports mono or stereo"));
        }

        // Find the encoder MFT.
        MFT_REGISTER_TYPE_INFO in_info  {MFMediaType_Audio, MFAudioFormat_PCM};
        MFT_REGISTER_TYPE_INFO out_info {MFMediaType_Audio, MFAudioFormat_AAC};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(
            kMftCategoryAudioEncoder,
            MFT_ENUM_FLAG_SORTANDFILTER,
            &in_info, &out_info,
            &activates, &count);
        if (FAILED(hr) || count == 0 || !activates) {
            if (activates) CoTaskMemFree(activates);
            return err(Error::from_hresult(hr, "MFTEnumEx(AAC encoder) found nothing"));
        }
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&mft_));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr) || !mft_) {
            return err(Error::from_hresult(hr, "ActivateObject(AAC encoder MFT) failed"));
        }

        // Streams: AAC encoder has 1 input + 1 output, both at id 0.
        DWORD in_streams = 0, out_streams = 0;
        if (FAILED(mft_->GetStreamCount(&in_streams, &out_streams))
            || in_streams != 1 || out_streams != 1) {
            return err(Error::make(Error::Code::EncoderFailed,
                                    "AAC MFT has unexpected stream count"));
        }

        if (auto r = configure_output(p); !r) return err(std::move(r.error()));
        if (auto r = configure_input(p);  !r) return err(std::move(r.error()));

        // After both types are set, the MFT can tell us its input buffer size.
        MFT_INPUT_STREAM_INFO  isi{};
        MFT_OUTPUT_STREAM_INFO osi{};
        (void)mft_->GetInputStreamInfo(0,  &isi);
        (void)mft_->GetOutputStreamInfo(0, &osi);
        output_provides_samples_ = !(osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES));
        output_buffer_min_       = osi.cbSize;

        // Begin streaming.
        hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) {
            return err(Error::from_hresult(hr, "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING failed"));
        }

        // Extract codec-private data (AudioSpecificConfig). Stripping the
        // 12-byte HEAACWAVEINFO trailer prefix per MS docs:
        //   "MF_MT_USER_DATA: 12-byte payload type info followed by the
        //    AudioSpecificConfig (per ISO/IEC 14496-3)."
        std::vector<uint8_t> codec_config;
        ComPtr<IMFMediaType> out_type;
        if (SUCCEEDED(mft_->GetOutputCurrentType(0, &out_type)) && out_type) {
            UINT32 blob_size = 0;
            if (SUCCEEDED(out_type->GetBlobSize(MF_MT_USER_DATA, &blob_size)) && blob_size > 12) {
                std::vector<uint8_t> blob(blob_size);
                if (SUCCEEDED(out_type->GetBlob(MF_MT_USER_DATA, blob.data(), blob_size, nullptr))) {
                    codec_config.assign(blob.begin() + 12, blob.end());
                }
            }
        }

        GPUR_INFO("AAC encoder initialised: {} Hz, {} ch, {} kbps, asc={} bytes",
                  p.sample_rate, p.channels, p.bitrate_bps / 1000, codec_config.size());
        return codec_config;
    }

    Result<void> push(const Block& pcm) override {
        if (!mft_) return err(Error::make(Error::Code::NotInitialised, "AAC encoder not initialised"));
        if (pcm.data.empty()) return ok();

        // Convert input to S16 interleaved at our configured channel count.
        std::vector<uint8_t> s16;
        if (pcm.format.sample_fmt == SampleFormat::S16
            && pcm.format.channels == params_.channels) {
            s16.assign(pcm.data.begin(), pcm.data.end());
        } else if (pcm.format.sample_fmt == SampleFormat::F32
                   && pcm.format.channels == params_.channels) {
            const size_t n = pcm.data.size() / sizeof(float);
            s16.resize(n * sizeof(int16_t));
            auto* in  = reinterpret_cast<const float*>(pcm.data.data());
            auto* out = reinterpret_cast<int16_t*>(s16.data());
            for (size_t i = 0; i < n; ++i) {
                float v = in[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                out[i] = static_cast<int16_t>(v * 32767.0f);
            }
        } else {
            return err(Error::make(Error::Code::InvalidArgument,
                                    "AAC push: incoming format doesn't match configured params"));
        }

        // Wrap in IMFSample.
        ComPtr<IMFMediaBuffer> mb;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(s16.size()), &mb);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "MFCreateMemoryBuffer failed"));
        BYTE* lock = nullptr;
        DWORD cap  = 0;
        hr = mb->Lock(&lock, &cap, nullptr);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "IMFMediaBuffer::Lock failed"));
        std::memcpy(lock, s16.data(), s16.size());
        mb->Unlock();
        mb->SetCurrentLength(static_cast<DWORD>(s16.size()));

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "MFCreateSample failed"));
        sample->AddBuffer(mb.Get());
        sample->SetSampleTime(pcm.pts.count() / 100);          // 100ns units
        sample->SetSampleDuration(pcm.duration.count() / 100);

        hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // Need to drain output first, then retry.
            if (auto r = drain_output(); !r) return r;
            hr = mft_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) return err(Error::from_hresult(hr, "AAC ProcessInput failed"));

        return drain_output();
    }

    Result<void> drain() override {
        if (!mft_) return ok();
        (void)mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        (void)mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return drain_output();
    }

    Result<void> shutdown() override {
        if (!mft_) return ok();
        (void)drain();
        (void)mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_.Reset();
        return ok();
    }

    const Params& params() const noexcept override { return params_; }

private:
    Result<void> configure_input(const Params& p) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "MFCreateMediaType(in) failed"));

        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, p.channels);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, p.sample_rate);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * p.channels);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, p.sample_rate * 2 * p.channels);
        t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);

        hr = mft_->SetInputType(0, t.Get(), 0);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "SetInputType(AAC) failed"));
        return ok();
    }

    Result<void> configure_output(const Params& p) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "MFCreateMediaType(out) failed"));

        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_AAC);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, p.channels);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, p.sample_rate);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, p.bitrate_bps / 8);
        t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);           // raw AAC (no ADTS)
        t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC LC

        hr = mft_->SetOutputType(0, t.Get(), 0);
        if (FAILED(hr)) return err(Error::from_hresult(hr, "SetOutputType(AAC) failed"));
        return ok();
    }

    Result<void> drain_output() {
        while (true) {
            MFT_OUTPUT_DATA_BUFFER out{};
            out.dwStreamID = 0;

            ComPtr<IMFSample>      sample_we_own;
            ComPtr<IMFMediaBuffer> buffer_we_own;

            // If the MFT doesn't allocate samples itself, we have to supply
            // one. The AAC encoder MFT does allocate, but be defensive.
            if (output_provides_samples_) {
                HRESULT hr = MFCreateSample(&sample_we_own);
                if (FAILED(hr)) return err(Error::from_hresult(hr, "MFCreateSample(out) failed"));
                hr = MFCreateMemoryBuffer(std::max<DWORD>(output_buffer_min_, 4096),
                                           &buffer_we_own);
                if (FAILED(hr)) return err(Error::from_hresult(hr, "MFCreateMemoryBuffer(out) failed"));
                sample_we_own->AddBuffer(buffer_we_own.Get());
                out.pSample = sample_we_own.Get();
            }

            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return ok();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // Re-negotiate output type. Shouldn't happen with our config but
                // handle it so we don't infinite-loop.
                if (auto r = configure_output(params_); !r) return r;
                continue;
            }
            if (FAILED(hr)) {
                return err(Error::from_hresult(hr, "AAC ProcessOutput failed"));
            }

            if (out.pSample) {
                emit_sample(out.pSample);
                // If the MFT allocated the sample, we must release.
                if (!sample_we_own) {
                    out.pSample->Release();
                }
            }
            if (out.pEvents) {
                out.pEvents->Release();
            }
        }
    }

    void emit_sample(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(sample->ConvertToContiguousBuffer(&mb)) || !mb) return;

        BYTE* lock = nullptr;
        DWORD len = 0;
        if (FAILED(mb->Lock(&lock, nullptr, &len))) return;

        Packet pkt;
        pkt.data.assign(lock, lock + len);
        mb->Unlock();

        LONGLONG t = 0, d = 0;
        sample->GetSampleTime(&t);
        sample->GetSampleDuration(&d);
        pkt.pts      = std::chrono::nanoseconds(t * 100);
        pkt.duration = std::chrono::nanoseconds(d * 100);

        if (on_packet_) on_packet_(std::move(pkt));
    }

    ComPtr<IMFTransform>  mft_;
    Params                params_{};
    OnPacket              on_packet_;
    bool                  output_provides_samples_{false};
    DWORD                 output_buffer_min_{0};
};

} // namespace

std::unique_ptr<AacEncoder> AacEncoder::create() {
    return std::make_unique<AacEncoderImpl>();
}

} // namespace gpur::core::audio
