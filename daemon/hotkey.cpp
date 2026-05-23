// Global hotkey manager.
//
// TODO(phase-1):
//   - Hidden message-only window (HWND_MESSAGE parent).
//   - RegisterHotKey for each binding; route WM_HOTKEY to OnFire.
//   - Register Raw Input (RIDEV_INPUTSINK + RIDEV_NOLEGACY) for kbd to handle
//     exclusive-fullscreen games where WM_HOTKEY gets eaten.
//   - Dedicated message-pump thread.

#include "hotkey.h"
#include "shared/log.h"

namespace gpur::daemon {

namespace {
class StubHotkeyManager final : public HotkeyManager {
public:
    Result<void> set_bindings(std::vector<Binding>, OnFire) override {
        return err(Error::not_implemented("HotkeyManager (phase 1)"));
    }
    Result<void> start() override { return ok(); }
    Result<void> stop() override  { return ok(); }
};
} // namespace

std::unique_ptr<HotkeyManager> HotkeyManager::create() {
    return std::make_unique<StubHotkeyManager>();
}

} // namespace gpur::daemon
