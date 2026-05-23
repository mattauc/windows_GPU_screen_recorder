#pragma once

#include "shared/result.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace gpur::daemon::overlay {

// Tier 1 overlay: topmost transparent layered Win32 window.
// Renders toast notifications via Direct2D. Works in borderless-fullscreen
// games (the vast majority of modern titles). No DLL injection, no anti-cheat
// risk.
//
// STATUS: stub. Phase 2.
class LayeredWindowOverlay {
public:
    enum class ToastKind {
        RecordingStarted,
        RecordingStopped,
        ReplaySaved,
        Error,
    };

    static std::unique_ptr<LayeredWindowOverlay> create();
    virtual ~LayeredWindowOverlay() = default;

    virtual Result<void> start() = 0;
    virtual Result<void> stop()  = 0;

    virtual void show_toast(ToastKind kind,
                            std::wstring_view title,
                            std::wstring_view body,
                            std::chrono::milliseconds duration = std::chrono::milliseconds{3000}) = 0;

    // On-air red dot in the corner while recording.
    virtual void set_recording_indicator(bool visible) = 0;
};

} // namespace gpur::daemon::overlay
