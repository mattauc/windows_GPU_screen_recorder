#pragma once

#include "iencoder.h"

namespace gpur::core::encode {

// NVENC backend. Requires NVIDIA Video Codec SDK 12.2+ at build time and an
// NVIDIA GPU with a recent driver (R535+) at runtime.
//
// When GPUR_HAVE_NVENC is 0, returns a stub encoder that fails initialise()
// with Error::NotImplemented.
std::unique_ptr<IEncoder> make_nvenc_encoder(std::shared_ptr<D3dContext> ctx);

// Probe: is NVENC usable on this machine right now? Loads nvEncodeAPI64.dll
// and attempts to open a session.
bool nvenc_is_available(std::shared_ptr<D3dContext> ctx);

// Encoder-allocated NV12 input texture. Created with bind flags suitable for
// both UAV writes (from the color converter compute shader) and NVENC input
// registration. Caller is expected to keep these alive across encode submits.
Result<ComPtr<ID3D11Texture2D>> create_nv12_input_texture(
    D3dContext& ctx, uint32_t width, uint32_t height);

} // namespace gpur::core::encode
