#pragma once

#include "shared/result.h"

#include <memory>

namespace gpur::daemon::overlay {

// Tier 2 overlay: MinHook on IDXGISwapChain::Present, rendered with ImGui
// inside the game's swap chain.
//
// Opt-in per game in config (anti-cheat risk — EAC / BattlEye / VAC may flag
// this as cheating). Required for true exclusive-fullscreen games.
//
// STATUS: stub. Phase 4.5.
class D3d11Hook {
public:
    static std::unique_ptr<D3d11Hook> create();
    virtual ~D3d11Hook() = default;

    virtual Result<void> install_into_process(uint32_t pid) = 0;
    virtual Result<void> uninstall()                        = 0;
};

} // namespace gpur::daemon::overlay
