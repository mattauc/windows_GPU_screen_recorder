#pragma once

#include "icapture.h"

namespace gpur::core::capture {

// Windows.Graphics.Capture backend. Primary capture path on Win10 1903+.
// Captures into ID3D11Texture2D handles in our shared D3D11 device — no copy
// out of GPU memory.
std::unique_ptr<ICapture> make_wgc_capture(std::shared_ptr<D3dContext> ctx);

// Probe: is WGC available on this machine? (OS version + GraphicsCaptureSession::IsSupported)
bool wgc_is_supported();

} // namespace gpur::core::capture
