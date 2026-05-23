#pragma once

#include "core/d3d_context.h"
#include "shared/result.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>

namespace gpur::core::convert {

using Microsoft::WRL::ComPtr;

// GPU color converter: BGRA8 source → NV12 destination (single ID3D11Texture2D
// in DXGI_FORMAT_NV12, with the Y plane as plane 0 and UV plane as plane 1).
//
// Uses a single compute-shader dispatch — no CPU touch, no intermediate
// staging surfaces.
class ColorConverter {
public:
    static Result<std::unique_ptr<ColorConverter>> create(std::shared_ptr<D3dContext> ctx);
    ~ColorConverter();

    // Convert src (BGRA) into the encoder-bound NV12 texture. dst must be a
    // DXGI_FORMAT_NV12 ID3D11Texture2D allocated by the encoder for NVENC input
    // (BindFlags must allow UAV writes — see nvenc_encoder.cpp).
    Result<void> convert(ID3D11Texture2D* src, ID3D11Texture2D* dst);

private:
    ColorConverter() = default;

    Result<void> initialise();

    std::shared_ptr<D3dContext>     ctx_;
    ComPtr<ID3D11ComputeShader>     shader_;
    ComPtr<ID3D11Buffer>            cbuffer_;
};

} // namespace gpur::core::convert
