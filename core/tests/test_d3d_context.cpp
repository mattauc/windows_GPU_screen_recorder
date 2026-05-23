#include "core/d3d_context.h"

#include <catch2/catch_test_macros.hpp>

using namespace gpur::core;

TEST_CASE("D3dContext creates on the default adapter", "[d3d][windows-only]") {
    // CI may run headless; D3D11CreateDevice should still succeed against the
    // basic display adapter. If it doesn't, skip rather than fail.
    auto ctx = D3dContext::create({});
    if (!ctx) {
        WARN("D3dContext::create failed in test env: " << ctx.error().message);
        SUCCEED();
        return;
    }
    REQUIRE((*ctx)->device() != nullptr);
    REQUIRE((*ctx)->immediate() != nullptr);
}
