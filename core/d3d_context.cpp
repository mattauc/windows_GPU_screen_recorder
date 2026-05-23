#include "d3d_context.h"
#include "shared/log.h"

#include <comdef.h>
#include <vector>

namespace gpur::core {

namespace {

Result<ComPtr<IDXGIAdapter1>> pick_adapter(LUID requested) {
    ComPtr<IDXGIFactory6> factory;
    if (HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return err(Error::from_hresult(hr, "CreateDXGIFactory1 failed"));
    }

    auto wants_specific = (requested.LowPart != 0 || requested.HighPart != 0);

    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        a->GetDesc1(&desc);

        // Skip the software adapter.
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (wants_specific) {
            if (desc.AdapterLuid.LowPart == requested.LowPart &&
                desc.AdapterLuid.HighPart == requested.HighPart) {
                chosen = a;
                break;
            }
        } else {
            chosen = a;
            break;
        }
    }

    if (!chosen) {
        return err(Error::make(Error::Code::DeviceNotFound, "no suitable DXGI adapter found"));
    }
    return chosen;
}

} // namespace

Result<std::shared_ptr<D3dContext>> D3dContext::create(const CreateParams& params) {
    auto self = std::shared_ptr<D3dContext>(new D3dContext());

    auto adapter = pick_adapter(params.adapter_luid);
    if (!adapter) return err(std::move(adapter.error()));
    self->adapter_ = *adapter;

    DXGI_ADAPTER_DESC1 desc{};
    self->adapter_->GetDesc1(&desc);
    self->adapter_luid_ = desc.AdapterLuid;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (params.debug_layer) flags |= D3D11_CREATE_DEVICE_DEBUG;

    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        self->adapter_.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        kFeatureLevels,
        static_cast<UINT>(std::size(kFeatureLevels)),
        D3D11_SDK_VERSION,
        &self->device_,
        &got,
        &self->context_);

    if (FAILED(hr) && params.debug_layer) {
        GPUR_WARN("D3D11CreateDevice with debug layer failed (0x{:x}); retrying without it",
                  static_cast<uint32_t>(hr));
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            self->adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            kFeatureLevels,
            static_cast<UINT>(std::size(kFeatureLevels)),
            D3D11_SDK_VERSION,
            &self->device_,
            &got,
            &self->context_);
    }
    if (FAILED(hr)) {
        return err(Error::from_hresult(hr, "D3D11CreateDevice failed"));
    }

    // NVENC and WGC both share textures across threads — set the multithread
    // protection bit on the device.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(self->device_.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    GPUR_INFO(L"D3D11 device created on adapter \"{}\" (LUID {:08x}:{:08x}), feature level 0x{:x}",
              std::wstring(desc.Description),
              static_cast<uint32_t>(self->adapter_luid_.HighPart),
              static_cast<uint32_t>(self->adapter_luid_.LowPart),
              static_cast<uint32_t>(got));

    return self;
}

D3dContext::~D3dContext() = default;

std::wstring D3dContext::adapter_description() const {
    if (!adapter_) return L"<no adapter>";
    DXGI_ADAPTER_DESC1 desc{};
    adapter_->GetDesc1(&desc);
    return std::wstring(desc.Description);
}

} // namespace gpur::core
