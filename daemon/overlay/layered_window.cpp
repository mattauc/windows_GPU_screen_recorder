// Tier 1 overlay — topmost layered Win32 window with Direct2D.
//
// TODO(phase-2):
//   - CreateWindowExW with WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT
//                          | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
//   - SetLayeredWindowAttributes(LWA_COLORKEY) for true-transparent areas
//   - D2D1CreateFactory + ID2D1HwndRenderTarget for compositing
//   - DirectWrite for SF-style text rendering
//   - Animation queue with fade-in/out
//   - Renders on its own UI thread so the recording thread isn't blocked.

#include "layered_window.h"
#include "shared/log.h"
#include "shared/wstr.h"

namespace gpur::daemon::overlay {

namespace {
class StubLayeredOverlay final : public LayeredWindowOverlay {
public:
    Result<void> start() override {
        return err(Error::not_implemented("LayeredWindowOverlay (phase 2)"));
    }
    Result<void> stop() override { return ok(); }
    void show_toast(ToastKind, std::wstring_view title, std::wstring_view body,
                    std::chrono::milliseconds) override {
        GPUR_DEBUG("[toast stub] {} — {}", gpur::wstring_to_utf8(title), gpur::wstring_to_utf8(body));
    }
    void set_recording_indicator(bool visible) override {
        GPUR_DEBUG("[toast stub] recording indicator: {}", visible);
    }
};
} // namespace

std::unique_ptr<LayeredWindowOverlay> LayeredWindowOverlay::create() {
    return std::make_unique<StubLayeredOverlay>();
}

} // namespace gpur::daemon::overlay
