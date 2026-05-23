// Window tracker — follow a game window across monitor / size changes.
//
// TODO(phase-4): implement using SetWinEventHook on
//   EVENT_OBJECT_LOCATIONCHANGE / EVENT_SYSTEM_MOVESIZEEND. For now, just a
//   stub so the build links.

#include "window_tracker.h"
#include "shared/log.h"

namespace gpur::core::capture {

Result<void> WindowTracker::attach(HWND, OnChange) {
    return err(Error::not_implemented("WindowTracker::attach (phase 4)"));
}

Result<void> WindowTracker::detach() {
    return ok();
}

WindowTracker::State WindowTracker::current() const {
    return last_;
}

Result<HWND> find_window_by_process(std::wstring_view, std::wstring_view) {
    return err(Error::not_implemented("find_window_by_process (phase 4)"));
}

} // namespace gpur::core::capture
