#pragma once

#include "shared/result.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>

namespace gpur::core {

using Microsoft::WRL::ComPtr;

// One D3D11 device + immediate context, shared across capture / convert /
// encode. This is the single most important invariant in the project: every
// stage of the pipeline must operate on the SAME ID3D11Device so that texture
// handles can be passed around without copies.
//
// Created with BGRA + multi-threaded support so WGC and NVENC are both happy.
class D3dContext {
public:
    // Adapter LUID lets us pin to a specific GPU (useful for laptops with both
    // an iGPU and dGPU). Pass {0,0} to use the default adapter.
    struct CreateParams {
        LUID adapter_luid{0, 0};
        bool debug_layer{false};
    };

    static Result<std::shared_ptr<D3dContext>> create(const CreateParams& params = {});

    ~D3dContext();

    D3dContext(const D3dContext&)            = delete;
    D3dContext& operator=(const D3dContext&) = delete;

    ID3D11Device*        device() const noexcept        { return device_.Get(); }
    ID3D11DeviceContext* immediate() const noexcept     { return context_.Get(); }
    IDXGIAdapter1*       adapter() const noexcept       { return adapter_.Get(); }
    LUID                 adapter_luid() const noexcept  { return adapter_luid_; }

    // The immediate context is not thread-safe. Any caller wanting to issue
    // commands from a non-render thread must hold this lock for the duration
    // of the call.
    std::mutex& immediate_mutex() noexcept              { return immediate_mutex_; }

    // Convenience: describe the adapter for logging.
    std::wstring adapter_description() const;

private:
    D3dContext() = default;

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter1>       adapter_;
    LUID                        adapter_luid_{0, 0};
    std::mutex                  immediate_mutex_;
};

} // namespace gpur::core
