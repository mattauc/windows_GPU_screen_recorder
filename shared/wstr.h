#pragma once

// Small string utility: convert std::wstring -> UTF-8 std::string.
//
// spdlog defaults to char-based fmt format strings. Anything we want to log
// that comes from a Win32 / WinRT API (adapter names, file paths, window
// titles) is wchar_t — convert through here at the log site so callers stay
// readable and spdlog stays narrow.

#include <windows.h>

#include <string>
#include <string_view>

namespace gpur {

inline std::string wstring_to_utf8(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0,
                                  ws.data(), static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0,
                          ws.data(), static_cast<int>(ws.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring utf8_to_wstring(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0,
                                  s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0,
                          s.data(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

} // namespace gpur
