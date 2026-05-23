#include "log.h"

#include <atomic>

namespace gpur {

namespace {
std::atomic<bool> g_initialised{false};
}

void init_logging(spdlog::level::level_enum level) {
    bool expected = false;
    if (!g_initialised.compare_exchange_strong(expected, true)) {
        spdlog::set_level(level);
        return;
    }

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto msvc    = std::make_shared<spdlog::sinks::msvc_sink_mt>();

    std::vector<spdlog::sink_ptr> sinks{console, msvc};
    auto logger = std::make_shared<spdlog::logger>("gpur", sinks.begin(), sinks.end());

    logger->set_level(level);
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");

    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::warn);
}

} // namespace gpur
