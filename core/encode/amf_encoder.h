#pragma once

#include "iencoder.h"

namespace gpur::core::encode {

// AMD AMF encoder backend (Phase 4).
//
// STATUS: stub.
std::unique_ptr<IEncoder> make_amf_encoder(std::shared_ptr<D3dContext> ctx);

} // namespace gpur::core::encode
