#pragma once

#include "shared/result.h"

#include <memory>

namespace gpur::daemon::overlay {

// ImGui rendering helpers shared between the layered-window Tier-1 overlay
// (rendered into our own D2D/D3D surface) and the Tier-2 injected hook
// (rendered into the game's swap chain).
//
// STATUS: stub. Phase 2 / 4.5.
class ImguiOverlay {
public:
    static std::unique_ptr<ImguiOverlay> create();
    virtual ~ImguiOverlay() = default;

    virtual Result<void> initialise_d3d11(void* d3d11_device, void* d3d11_context) = 0;
    virtual void         render_frame() = 0;
    virtual void         shutdown()     = 0;
};

} // namespace gpur::daemon::overlay
