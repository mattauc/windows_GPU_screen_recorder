#pragma once

#include "shared/result.h"

#include <functional>
#include <memory>
#include <string>

namespace gpur::daemon {

// Global hotkey manager. Two backends, both wired:
//   - RegisterHotKey via a hidden message-only window (works for windowed
//     and most borderless-fullscreen games).
//   - Raw Input (RIDEV_INPUTSINK) as fallback for true exclusive-fullscreen
//     where RegisterHotKey gets suppressed.
//
// STATUS: stub. Phase 1.
class HotkeyManager {
public:
    enum class Action {
        StartStopRecording,
        SaveReplay,
        ToggleReplay,
    };

    struct Binding {
        Action       action;
        uint32_t     modifiers;   // MOD_CONTROL | MOD_ALT | ...
        uint32_t     vk;          // VK_F9, etc.
    };

    using OnFire = std::function<void(Action)>;

    static std::unique_ptr<HotkeyManager> create();
    virtual ~HotkeyManager() = default;

    virtual Result<void> set_bindings(std::vector<Binding> bindings, OnFire cb) = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> stop()  = 0;
};

} // namespace gpur::daemon
