#pragma once

#include "iencoder.h"

namespace gpur::core::encode {

// Intel Quick Sync (oneVPL) encoder backend (Phase 4).
//
// STATUS: stub.
std::unique_ptr<IEncoder> make_qsv_encoder(std::shared_ptr<D3dContext> ctx);

} // namespace gpur::core::encode
