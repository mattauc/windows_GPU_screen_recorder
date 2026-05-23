// Shared Dear ImGui overlay renderer.
//
// TODO(phase-2 / phase-4.5):
//   - Add `imgui` to vcpkg.json with the d3d11 backend feature when this lands.
//   - On initialise_d3d11: ImGui_ImplDX11_Init.
//   - render_frame: NewFrame, draw toast queue, EndFrame, RenderDrawData.

#include "imgui_overlay.h"
#include "shared/log.h"

namespace gpur::daemon::overlay {

namespace {
class StubImguiOverlay final : public ImguiOverlay {
public:
    Result<void> initialise_d3d11(void*, void*) override {
        return err(Error::not_implemented("ImguiOverlay (phase 2)"));
    }
    void render_frame() override {}
    void shutdown()     override {}
};
} // namespace

std::unique_ptr<ImguiOverlay> ImguiOverlay::create() {
    return std::make_unique<StubImguiOverlay>();
}

} // namespace gpur::daemon::overlay
