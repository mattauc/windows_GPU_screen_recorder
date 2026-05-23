// gpur-daemon.exe — recording daemon.
//
// Phase 0: simple CLI. Records the primary monitor for a given duration and
// writes raw H.264 NALUs to a file. No IPC server, no hotkeys, no overlay yet
// (those exist as stubs alongside main.cpp; they'll be wired up in later
// phases).

#include "core/d3d_context.h"
#include "core/pipeline.h"
#include "shared/log.h"

#include <atomic>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CliArgs {
    std::filesystem::path output{"recording.mkv"};
    uint32_t              duration_sec{0};
    uint32_t              fps{60};
    uint32_t              bitrate_bps{50'000'000};
    uint32_t              audio_bitrate_bps{192'000};
    int                   monitor{0};
    std::string           codec{"h264"};
    bool                  capture_audio{true};
    bool                  show_help{false};
};

void print_usage() {
    std::cout <<
        "gpur-daemon — GPU Screen Recorder (Phase 0 CLI)\n"
        "\n"
        "Usage: gpur-daemon [options]\n"
        "\n"
        "Options:\n"
        "  --output PATH          Output file (default: recording.mkv).\n"
        "                         .mkv = Matroska container (recommended).\n"
        "                         .h264/.h265 = raw Annex-B bitstream (for debugging).\n"
        "  --duration SEC         Recording length in seconds (0 = until Ctrl+C)\n"
        "  --fps N                Capture/encode framerate (default: 60)\n"
        "  --bitrate BPS          Encoder bitrate, bits/sec (default: 50_000_000)\n"
        "  --monitor N            Monitor index, 0 = primary (default: 0)\n"
        "  --codec h264|hevc      Encoder codec (default: h264)\n"
        "  --audio-bitrate BPS    AAC audio bitrate (default: 192000)\n"
        "  --no-audio             Disable system-audio capture\n"
        "  --help                 Show this help\n";
}

bool parse_uint(std::string_view s, uint32_t& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool parse_int(std::string_view s, int& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool parse_args(int argc, char** argv, CliArgs& out) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto needs_value = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return {};
            }
            return argv[++i];
        };

        if (a == "--help" || a == "-h") {
            out.show_help = true;
            return true;
        } else if (a == "--output") {
            auto v = needs_value(a); if (v.empty()) return false;
            out.output = std::filesystem::path(v);
        } else if (a == "--duration") {
            auto v = needs_value(a); if (v.empty()) return false;
            if (!parse_uint(v, out.duration_sec)) { std::cerr << "bad --duration\n"; return false; }
        } else if (a == "--fps") {
            auto v = needs_value(a); if (v.empty()) return false;
            if (!parse_uint(v, out.fps)) { std::cerr << "bad --fps\n"; return false; }
        } else if (a == "--bitrate") {
            auto v = needs_value(a); if (v.empty()) return false;
            if (!parse_uint(v, out.bitrate_bps)) { std::cerr << "bad --bitrate\n"; return false; }
        } else if (a == "--monitor") {
            auto v = needs_value(a); if (v.empty()) return false;
            if (!parse_int(v, out.monitor)) { std::cerr << "bad --monitor\n"; return false; }
        } else if (a == "--codec") {
            auto v = needs_value(a); if (v.empty()) return false;
            out.codec = std::string(v);
            if (out.codec != "h264" && out.codec != "hevc") {
                std::cerr << "--codec must be h264 or hevc\n";
                return false;
            }
        } else if (a == "--audio-bitrate") {
            auto v = needs_value(a); if (v.empty()) return false;
            if (!parse_uint(v, out.audio_bitrate_bps)) {
                std::cerr << "bad --audio-bitrate\n"; return false;
            }
        } else if (a == "--no-audio") {
            out.capture_audio = false;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
    }
    return true;
}

std::atomic<gpur::core::Pipeline*> g_active_pipeline{nullptr};

BOOL WINAPI console_ctrl_handler(DWORD ctrl) {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT ||
        ctrl == CTRL_CLOSE_EVENT) {
        if (auto* p = g_active_pipeline.load()) {
            p->stop();
        }
        return TRUE;
    }
    return FALSE;
}

} // namespace

int main(int argc, char** argv) {
    gpur::init_logging(spdlog::level::info);

    CliArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage();
        return 2;
    }
    if (args.show_help) {
        print_usage();
        return 0;
    }

    auto ctx = gpur::core::D3dContext::create({});
    if (!ctx) {
        GPUR_ERROR("D3dContext::create failed: {}", ctx.error().message);
        return 1;
    }

    auto pipeline = gpur::core::Pipeline::create(*ctx);
    if (!pipeline) {
        GPUR_ERROR("Pipeline::create failed: {}", pipeline.error().message);
        return 1;
    }

    g_active_pipeline.store(pipeline->get());
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    gpur::core::Pipeline::Config cfg{};
    cfg.output_path                       = args.output;
    cfg.target.source                     = args.monitor == 0
        ? gpur::core::capture::Source::PrimaryMonitor
        : gpur::core::capture::Source::MonitorIndex;
    cfg.target.monitor_index              = args.monitor;
    cfg.encoder.fps                       = args.fps;
    cfg.encoder.bitrate_bps               = args.bitrate_bps;
    cfg.encoder.codec                     = (args.codec == "hevc")
        ? gpur::core::encode::Codec::HEVC
        : gpur::core::encode::Codec::H264;
    cfg.encoder.keyframe_interval_frames  = args.fps * 2;   // 2s
    cfg.duration                          = std::chrono::seconds(args.duration_sec);
    cfg.capture_audio                     = args.capture_audio;
    cfg.audio_bitrate_bps                 = args.audio_bitrate_bps;

    GPUR_INFO("Starting recording: output={}, monitor={}, {} fps, {} kbps, codec={}, audio={}",
              cfg.output_path.string(), args.monitor, args.fps,
              args.bitrate_bps / 1000, args.codec,
              args.capture_audio ? "on" : "off");

    auto r = (*pipeline)->run(cfg);
    g_active_pipeline.store(nullptr);

    if (!r) {
        GPUR_ERROR("Pipeline failed: {} (hr=0x{:x})",
                   r.error().message, static_cast<uint32_t>(r.error().hresult));
        return 1;
    }

    auto s = (*pipeline)->stats();
    GPUR_INFO("Done. {} frames, {:.1f} MB written, {:.2f}s wall",
              s.frames_captured,
              s.bytes_written / 1'000'000.0,
              std::chrono::duration<double>(s.wall_time).count());
    return 0;
}
