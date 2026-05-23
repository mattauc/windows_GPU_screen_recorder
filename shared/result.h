#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

namespace gpur {

// Project-wide error type. Carries an HRESULT (or 0 if not Win32-originated),
// an Error::Code categorisation, and a human-readable message.
struct Error {
    enum class Code {
        Ok,
        NotImplemented,
        InvalidArgument,
        NotInitialised,
        AlreadyInitialised,
        DeviceLost,
        DeviceNotFound,
        CaptureFailed,
        EncoderFailed,
        AudioFailed,
        MuxerFailed,
        IoFailed,
        Win32Failed,
        Cancelled,
        Unknown,
    };

    Code        code{Code::Unknown};
    long        hresult{0};       // HRESULT if Win32-originated, 0 otherwise
    std::string message;

    static Error make(Code c, std::string_view msg) {
        return Error{c, 0, std::string{msg}};
    }
    static Error from_hresult(long hr, std::string_view msg) {
        return Error{Code::Win32Failed, hr, std::string{msg}};
    }
    static Error not_implemented(std::string_view what) {
        return Error{Code::NotImplemented, 0, std::string{what} + " is not implemented yet"};
    }
};

// Result<T> = std::expected<T, Error>. Use this for any operation that can fail.
template <typename T>
using Result = std::expected<T, Error>;

inline auto ok() -> Result<void> { return {}; }

template <typename T>
inline auto ok(T&& value) -> Result<std::decay_t<T>> {
    return Result<std::decay_t<T>>{std::forward<T>(value)};
}

inline auto err(Error e) -> std::unexpected<Error> {
    return std::unexpected<Error>{std::move(e)};
}

inline const char* to_string(Error::Code c) {
    switch (c) {
        case Error::Code::Ok:                  return "Ok";
        case Error::Code::NotImplemented:      return "NotImplemented";
        case Error::Code::InvalidArgument:     return "InvalidArgument";
        case Error::Code::NotInitialised:      return "NotInitialised";
        case Error::Code::AlreadyInitialised:  return "AlreadyInitialised";
        case Error::Code::DeviceLost:          return "DeviceLost";
        case Error::Code::DeviceNotFound:      return "DeviceNotFound";
        case Error::Code::CaptureFailed:       return "CaptureFailed";
        case Error::Code::EncoderFailed:       return "EncoderFailed";
        case Error::Code::AudioFailed:         return "AudioFailed";
        case Error::Code::MuxerFailed:         return "MuxerFailed";
        case Error::Code::IoFailed:            return "IoFailed";
        case Error::Code::Win32Failed:         return "Win32Failed";
        case Error::Code::Cancelled:           return "Cancelled";
        case Error::Code::Unknown:             return "Unknown";
    }
    return "?";
}

} // namespace gpur

// Convenience: bail out of a function if a Result<> failed.
#define GPUR_TRY(expr)                                  \
    do {                                                \
        auto&& _r = (expr);                             \
        if (!_r) return ::gpur::err(std::move(_r.error())); \
    } while (0)

#define GPUR_TRY_ASSIGN(var, expr)                      \
    auto&& _tmp_##var = (expr);                         \
    if (!_tmp_##var) return ::gpur::err(std::move(_tmp_##var.error())); \
    var = std::move(*_tmp_##var)
