#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/msvc_sink.h>

#include <memory>
#include <string_view>

namespace gpur {

// Initialise the global logger. Safe to call multiple times; the first call wins.
// Writes to stdout (colored) + the VS debug output window when running under a debugger.
void init_logging(spdlog::level::level_enum level = spdlog::level::info);

// Convenience macros so call sites are concise.
#define GPUR_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define GPUR_DEBUG(...) ::spdlog::debug(__VA_ARGS__)
#define GPUR_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define GPUR_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define GPUR_ERROR(...) ::spdlog::error(__VA_ARGS__)

} // namespace gpur
