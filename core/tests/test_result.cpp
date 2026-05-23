#include "shared/result.h"

#include <catch2/catch_test_macros.hpp>

using namespace gpur;

namespace {
Result<int> divide(int a, int b) {
    if (b == 0) return err(Error::make(Error::Code::InvalidArgument, "divide by zero"));
    return a / b;
}
} // namespace

TEST_CASE("Result<T> happy path", "[result]") {
    auto r = divide(10, 2);
    REQUIRE(r.has_value());
    REQUIRE(*r == 5);
}

TEST_CASE("Result<T> error path carries message + code", "[result]") {
    auto r = divide(10, 0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::Code::InvalidArgument);
    REQUIRE(r.error().message == "divide by zero");
}

TEST_CASE("to_string covers every error code", "[result]") {
    using Code = Error::Code;
    for (auto c : {Code::Ok, Code::NotImplemented, Code::InvalidArgument,
                   Code::NotInitialised, Code::AlreadyInitialised, Code::DeviceLost,
                   Code::DeviceNotFound, Code::CaptureFailed, Code::EncoderFailed,
                   Code::AudioFailed, Code::MuxerFailed, Code::IoFailed,
                   Code::Win32Failed, Code::Cancelled, Code::Unknown}) {
        REQUIRE(to_string(c) != nullptr);
        REQUIRE(std::string(to_string(c)) != "?");
    }
}
