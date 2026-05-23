#pragma once

#include "core/d3d_context.h"
#include "shared/result.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gpur::core::capture {

using Microsoft::WRL::ComPtr;

enum class Source {
    PrimaryMonitor,
    MonitorIndex,    // payload = monitor_index
    HWND,            // payload = window handle (uintptr_t)
};

struct Target {
    Source    source{Source::PrimaryMonitor};
    int       monitor_index{0};
    uintptr_t hwnd{0};
};

// One captured frame. Texture is owned by the capturer's frame pool; it must
// be consumed (or copied) before the next frame is acquired.
struct Frame {
    ComPtr<ID3D11Texture2D>     texture;          // BGRA8 (or whatever capture format is)
    DXGI_FORMAT                 format{DXGI_FORMAT_B8G8R8A8_UNORM};
    uint32_t                    width{};
    uint32_t                    height{};
    std::chrono::nanoseconds    timestamp{};      // monotonic, from QPC
    bool                        content_changed{true};  // false = duplicate (no redraw since last)
};

// Capture backend abstraction. Two implementations:
//   - WgcCapture: Windows.Graphics.Capture (modern, default)
//   - DdaCapture: Desktop Duplication API (fallback for Win10 < 1903)
class ICapture {
public:
    virtual ~ICapture() = default;

    struct StartParams {
        Target                  target;
        uint32_t                fps_hint{60};   // best-effort; WGC is event-driven
        bool                    include_cursor{true};
        bool                    capture_audio{false};   // only WGC; usually we use WASAPI separately
    };

    virtual Result<void> start(const StartParams& params) = 0;
    virtual Result<void> stop()                            = 0;

    // Block until a frame is ready or the deadline elapses. Returns
    // Error::Code::Cancelled if stop() was called concurrently.
    virtual Result<Frame> next_frame(std::chrono::milliseconds timeout) = 0;

    // Captured texture dimensions. Valid only after start().
    virtual uint32_t width()  const noexcept = 0;
    virtual uint32_t height() const noexcept = 0;
    virtual DXGI_FORMAT format() const noexcept = 0;
};

// Factory: returns the preferred backend for the current OS / target.
Result<std::unique_ptr<ICapture>> make_capture(
    std::shared_ptr<D3dContext> ctx,
    bool force_dda = false);

} // namespace gpur::core::capture
