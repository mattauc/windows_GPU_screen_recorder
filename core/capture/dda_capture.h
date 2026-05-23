#pragma once

#include "icapture.h"

namespace gpur::core::capture {

// Desktop Duplication API fallback. Used when WGC is unavailable
// (Win10 < 1903, RDP sessions, certain enterprise configurations).
//
// STATUS: stub. Phase 1.5 work.
std::unique_ptr<ICapture> make_dda_capture(std::shared_ptr<D3dContext> ctx);

} // namespace gpur::core::capture
