// Tier 2 overlay — DXGI Present hook for exclusive-fullscreen games.
//
// TODO(phase-4.5):
//   - Spawn a helper DLL that the daemon injects (CreateRemoteThread + LoadLibrary).
//   - DLL uses MinHook to detour IDXGISwapChain::Present.
//   - First call: capture the swap-chain's D3D device, set up ImGui D3D11 backend.
//   - Per call: ImGui::NewFrame → render our toast UI → ImGui::Render → present.
//   - Daemon ↔ helper-DLL IPC: same named pipe (different message subset).

#include "d3d11_hook.h"
#include "shared/log.h"

namespace gpur::daemon::overlay {

namespace {
class StubD3d11Hook final : public D3d11Hook {
public:
    Result<void> install_into_process(uint32_t) override {
        return err(Error::not_implemented("D3d11Hook (phase 4.5)"));
    }
    Result<void> uninstall() override { return ok(); }
};
} // namespace

std::unique_ptr<D3d11Hook> D3d11Hook::create() {
    return std::make_unique<StubD3d11Hook>();
}

} // namespace gpur::daemon::overlay
