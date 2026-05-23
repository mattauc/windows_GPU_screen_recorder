#pragma once

#include "shared/result.h"

#include <windows.h>

#include <chrono>
#include <functional>
#include <string>

namespace gpur::core::capture {

// Tracks a target HWND as it moves between monitors / changes size, so a
// recording can follow a specific game window across topology changes.
//
// STATUS: stub. Phase 4 (per-game profiles).
class WindowTracker {
public:
    struct State {
        HWND        hwnd{};
        RECT        bounds{};        // screen coords
        HMONITOR    monitor{};
        bool        minimised{false};
        bool        fullscreen{false};
    };

    using OnChange = std::function<void(const State&)>;

    Result<void> attach(HWND hwnd, OnChange cb);
    Result<void> detach();

    State current() const;

private:
    HWND     hwnd_{};
    OnChange cb_;
    State    last_;
};

// Resolve a window by process name + title fragment (best-effort).
Result<HWND> find_window_by_process(std::wstring_view exe_name,
                                    std::wstring_view title_fragment = {});

} // namespace gpur::core::capture
