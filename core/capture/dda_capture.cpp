// Desktop Duplication API capture — fallback for WGC.
//
// TODO(phase-1.5): implement IDXGIOutputDuplication-based capture.
//   - IDXGIOutput1::DuplicateOutput on the chosen monitor
//   - AcquireNextFrame loop, copy into our ring (same pattern as WGC)
//   - Handle DXGI_ERROR_ACCESS_LOST on monitor topology changes (recreate)
//   - Cursor compositing (DDA gives cursor separately)

#include "dda_capture.h"
#include "shared/log.h"

namespace gpur::core::capture {

namespace {

class DdaCapture final : public ICapture {
public:
    explicit DdaCapture(std::shared_ptr<D3dContext> ctx) : ctx_(std::move(ctx)) {}

    Result<void> start(const StartParams&) override {
        return err(Error::not_implemented("Desktop Duplication capture (phase 1.5)"));
    }
    Result<void> stop() override { return ok(); }

    Result<Frame> next_frame(std::chrono::milliseconds) override {
        return err(Error::not_implemented("DdaCapture::next_frame"));
    }

    uint32_t    width()  const noexcept override { return 0; }
    uint32_t    height() const noexcept override { return 0; }
    DXGI_FORMAT format() const noexcept override { return DXGI_FORMAT_B8G8R8A8_UNORM; }

private:
    std::shared_ptr<D3dContext> ctx_;
};

} // namespace

std::unique_ptr<ICapture> make_dda_capture(std::shared_ptr<D3dContext> ctx) {
    GPUR_WARN("DDA capture requested but not implemented yet — phase 1.5");
    return std::make_unique<DdaCapture>(std::move(ctx));
}

} // namespace gpur::core::capture
