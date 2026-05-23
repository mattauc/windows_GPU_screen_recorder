// WASAPI loopback capture (system audio).
//
// TODO(phase-1): implement.
//   - IMMDeviceEnumerator → GetDefaultAudioEndpoint(eRender)
//   - IAudioClient::Initialize with AUDCLNT_STREAMFLAGS_LOOPBACK
//   - IAudioCaptureClient::GetBuffer in a dedicated thread (avrt MMCSS)
//   - Convert to F32 if device gives us S16

#include "wasapi_loopback.h"
#include "shared/log.h"

namespace gpur::core::audio {

namespace {
class StubLoopback final : public WasapiLoopback {
public:
    Result<void> start(OnBlock) override {
        return err(Error::not_implemented("WASAPI loopback (phase 1)"));
    }
    Result<void> stop() override { return ok(); }
    const Format& format() const noexcept override { return fmt_; }
private:
    Format fmt_{};
};
} // namespace

std::unique_ptr<WasapiLoopback> WasapiLoopback::create() {
    return std::make_unique<StubLoopback>();
}

} // namespace gpur::core::audio
