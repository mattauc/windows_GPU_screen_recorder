// Windows.Graphics.Capture implementation.
//
// Pipeline:
//   1. Create a Direct3D11CaptureFramePool sized to our monitor.
//   2. Create a GraphicsCaptureSession for the target monitor (or window).
//   3. On each FrameArrived event, copy the captured texture into a slot in
//      our own ring of textures (CopySubresourceRegion, GPU→GPU, no CPU touch)
//      and signal next_frame().
//
// Why we copy instead of handing the frame texture directly:
//   - WGC requires us to release the frame quickly so its internal pool can
//     recycle the surface. If we held it for the duration of NVENC submission
//     we'd cause the capture session to stall.
//   - The copy is a single GPU CopySubresourceRegion — fully zero-CPU.

#include "wgc_capture.h"
#include "shared/log.h"

// WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace winrt {
using namespace Windows::Foundation;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace gpur::core::capture {

namespace {

constexpr uint32_t kRingSize = 4;

// Convert a D3D11 device into a WinRT IDirect3DDevice.
winrt::IDirect3DDevice make_winrt_device(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgi;
    winrt::check_hresult(dev->QueryInterface(IID_PPV_ARGS(&dxgi)));
    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),
                                                                insp.put()));
    return insp.as<winrt::IDirect3DDevice>();
}

// Extract the underlying ID3D11Texture2D from a WinRT IDirect3DSurface.
ComPtr<ID3D11Texture2D> texture_from_surface(const winrt::IDirect3DSurface& surface) {
    auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&tex)));
    return tex;
}

class WgcCapture final : public ICapture {
public:
    explicit WgcCapture(std::shared_ptr<D3dContext> ctx) : ctx_(std::move(ctx)) {}

    ~WgcCapture() override {
        (void)stop();
    }

    Result<void> start(const StartParams& params) override {
        if (started_) return err(Error::make(Error::Code::AlreadyInitialised, "WGC already started"));

        // Ensure WinRT is initialised on this thread (the daemon's main thread).
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (winrt::hresult_error const& e) {
            // RPC_E_CHANGED_MODE = 0x80010106: already initialised in a different mode, fine.
            if (e.code() != 0x80010106) {
                return err(Error::from_hresult(e.code(), "winrt::init_apartment failed"));
            }
        }

        if (!wgc_is_supported()) {
            return err(Error::make(Error::Code::CaptureFailed,
                                    "Windows Graphics Capture is not supported on this OS"));
        }

        params_ = params;

        // Build a GraphicsCaptureItem from the target.
        winrt::GraphicsCaptureItem item{nullptr};
        try {
            auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem,
                                                         IGraphicsCaptureItemInterop>();
            if (params.target.source == Source::HWND) {
                winrt::check_hresult(interop->CreateForWindow(
                    reinterpret_cast<HWND>(params.target.hwnd),
                    winrt::guid_of<winrt::GraphicsCaptureItem>(),
                    winrt::put_abi(item)));
            } else {
                HMONITOR mon = pick_monitor(params.target);
                if (!mon) {
                    return err(Error::make(Error::Code::CaptureFailed,
                                           "could not resolve monitor for capture"));
                }
                winrt::check_hresult(interop->CreateForMonitor(
                    mon,
                    winrt::guid_of<winrt::GraphicsCaptureItem>(),
                    winrt::put_abi(item)));
            }
        } catch (winrt::hresult_error const& e) {
            return err(Error::from_hresult(e.code(), "CreateForWindow/Monitor failed"));
        }

        auto size = item.Size();
        width_  = static_cast<uint32_t>(size.Width);
        height_ = static_cast<uint32_t>(size.Height);

        // Create the WinRT-bound D3D device.
        winrt_device_ = make_winrt_device(ctx_->device());

        // Frame pool. Use B8G8R8A8UIntNormalized (WGC's standard SDR format).
        try {
            frame_pool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrt_device_,
                winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                static_cast<int32_t>(kRingSize),
                size);
        } catch (winrt::hresult_error const& e) {
            return err(Error::from_hresult(e.code(), "CreateFreeThreaded frame pool failed"));
        }

        // Allocate our internal ring of textures.
        ring_.clear();
        ring_.resize(kRingSize);
        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width            = width_;
        tdesc.Height           = height_;
        tdesc.MipLevels        = 1;
        tdesc.ArraySize        = 1;
        tdesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage            = D3D11_USAGE_DEFAULT;
        tdesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        tdesc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
        for (auto& slot : ring_) {
            if (HRESULT hr = ctx_->device()->CreateTexture2D(&tdesc, nullptr, &slot.texture); FAILED(hr)) {
                return err(Error::from_hresult(hr, "CreateTexture2D for capture ring failed"));
            }
        }

        // Subscribe to FrameArrived BEFORE starting the session.
        frame_arrived_token_ = frame_pool_.FrameArrived(
            {this, &WgcCapture::on_frame_arrived});

        session_ = frame_pool_.CreateCaptureSession(item);
        // Try to suppress the capture border (yellow rectangle) on Win11 22H2+.
        try {
            session_.IsBorderRequired(false);
        } catch (...) { /* older OS, ignore */ }
        try {
            session_.IsCursorCaptureEnabled(params.include_cursor);
        } catch (...) {}

        started_ = true;
        cancelled_.store(false);
        session_.StartCapture();

        GPUR_INFO("WGC capture started: {}x{}", width_, height_);
        return ok();
    }

    Result<void> stop() override {
        if (!started_) return ok();
        cancelled_.store(true);
        cv_.notify_all();

        if (frame_pool_) {
            frame_pool_.FrameArrived(frame_arrived_token_);
        }
        if (session_) {
            session_.Close();
            session_ = nullptr;
        }
        if (frame_pool_) {
            frame_pool_.Close();
            frame_pool_ = nullptr;
        }

        started_ = false;
        GPUR_INFO("WGC capture stopped");
        return ok();
    }

    Result<Frame> next_frame(std::chrono::milliseconds timeout) override {
        std::unique_lock lk(mu_);
        if (!cv_.wait_for(lk, timeout, [&] { return !pending_.empty() || cancelled_.load(); })) {
            return err(Error::make(Error::Code::CaptureFailed, "next_frame timed out"));
        }
        if (cancelled_.load() && pending_.empty()) {
            return err(Error::make(Error::Code::Cancelled, "capture stopped"));
        }
        Frame f = std::move(pending_.front());
        pending_.pop();
        return f;
    }

    uint32_t    width()  const noexcept override { return width_; }
    uint32_t    height() const noexcept override { return height_; }
    DXGI_FORMAT format() const noexcept override { return DXGI_FORMAT_B8G8R8A8_UNORM; }

private:
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
    };

    static HMONITOR pick_monitor(const Target& t) {
        struct Ctx { int wanted; int seen; HMONITOR out; };
        Ctx c{t.monitor_index, 0, nullptr};

        if (t.source == Source::PrimaryMonitor) {
            POINT origin{0, 0};
            return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        }

        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR mon, HDC, LPRECT, LPARAM lparam) -> BOOL {
                auto* cc = reinterpret_cast<Ctx*>(lparam);
                if (cc->seen == cc->wanted) {
                    cc->out = mon;
                    return FALSE;
                }
                ++cc->seen;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&c));
        return c.out;
    }

    void on_frame_arrived(const winrt::Direct3D11CaptureFramePool& sender,
                          const winrt::IInspectable&) {
        if (cancelled_.load()) return;

        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        auto surface = frame.Surface();
        auto src     = texture_from_surface(surface);

        // Pick the next ring slot and CopySubresourceRegion into it under the
        // immediate-context lock.
        uint32_t slot_idx = (write_idx_++) % kRingSize;
        auto& slot = ring_[slot_idx];

        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
        auto ts_ns = std::chrono::nanoseconds(
            static_cast<int64_t>((qpc.QuadPart * 1'000'000'000LL) / qpf.QuadPart));

        {
            std::scoped_lock g(ctx_->immediate_mutex());
            ctx_->immediate()->CopyResource(slot.texture.Get(), src.Get());
        }

        Frame out;
        out.texture        = slot.texture;
        out.format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        out.width          = width_;
        out.height         = height_;
        out.timestamp      = ts_ns;
        out.content_changed = true; // WGC doesn't expose dirty rects through this path

        {
            std::scoped_lock lk(mu_);
            pending_.push(std::move(out));
        }
        cv_.notify_one();
    }

    std::shared_ptr<D3dContext>             ctx_;
    StartParams                             params_;
    uint32_t                                width_{};
    uint32_t                                height_{};

    winrt::IDirect3DDevice                  winrt_device_{nullptr};
    winrt::Direct3D11CaptureFramePool       frame_pool_{nullptr};
    winrt::GraphicsCaptureSession           session_{nullptr};
    winrt::event_token                      frame_arrived_token_{};

    std::vector<Slot>                       ring_;
    std::atomic<uint32_t>                   write_idx_{0};

    std::mutex                              mu_;
    std::condition_variable                 cv_;
    std::queue<Frame>                       pending_;
    std::atomic<bool>                       cancelled_{false};
    bool                                    started_{false};
};

} // namespace

bool wgc_is_supported() {
    try {
        return winrt::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

std::unique_ptr<ICapture> make_wgc_capture(std::shared_ptr<D3dContext> ctx) {
    return std::make_unique<WgcCapture>(std::move(ctx));
}

// Default factory: prefer WGC, fall back to DDA.
Result<std::unique_ptr<ICapture>> make_capture(std::shared_ptr<D3dContext> ctx, bool force_dda) {
    if (!force_dda && wgc_is_supported()) {
        return make_wgc_capture(std::move(ctx));
    }
    // DDA implementation is a stub for now — see dda_capture.cpp.
    extern std::unique_ptr<ICapture> make_dda_capture(std::shared_ptr<D3dContext>);
    return make_dda_capture(std::move(ctx));
}

} // namespace gpur::core::capture
