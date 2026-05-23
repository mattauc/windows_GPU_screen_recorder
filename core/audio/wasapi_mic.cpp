// Microphone capture via WASAPI.
//
// TODO(phase-1): implement.
//   - IMMDeviceEnumerator → default eCapture or specific device_id
//   - IAudioClient::Initialize (no loopback flag)
//   - Same capture loop as loopback

#include "wasapi_mic.h"
#include "shared/log.h"

namespace gpur::core::audio {

namespace {
class StubMic final : public WasapiMic {
public:
    explicit StubMic(std::wstring device_id) : device_id_(std::move(device_id)) {}
    Result<void> start(OnBlock) override {
        return err(Error::not_implemented("WASAPI mic (phase 1)"));
    }
    Result<void> stop() override { return ok(); }
    const Format& format() const noexcept override { return fmt_; }
private:
    std::wstring device_id_;
    Format       fmt_{};
};
} // namespace

std::unique_ptr<WasapiMic> WasapiMic::create(std::wstring_view device_id) {
    return std::make_unique<StubMic>(std::wstring(device_id));
}

} // namespace gpur::core::audio
