#pragma once

#include "core/audio/aac_encoder.h"
#include "core/audio/mixer.h"
#include "core/audio/wasapi_loopback.h"
#include "core/audio/wasapi_mic.h"
#include "core/capture/icapture.h"
#include "core/convert/color_converter.h"
#include "core/d3d_context.h"
#include "core/encode/iencoder.h"
#include "core/mux/imuxer.h"
#include "shared/result.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gpur::core {

// One-shot recording pipeline: capture → convert → encode → mux.
//
// Phase 0 version: no audio, no replay, single-monitor, NVENC only.
// Phase 1+ will compose this with audio + replay buffer + named pipes.
class Pipeline {
public:
    struct Config {
        std::filesystem::path           output_path{"recording.mkv"};
        capture::Target                 target{};
        encode::EncoderParams           encoder;
        // duration == 0 means "run until stop() is called".
        std::chrono::seconds            duration{0};

        // Audio (loopback only for now; see audio/mixer.cpp for the mic TODO).
        bool                            capture_audio{true};
        uint32_t                        audio_bitrate_bps{192'000};
    };

    static Result<std::unique_ptr<Pipeline>> create(std::shared_ptr<D3dContext> ctx);
    ~Pipeline();

    Result<void> run(const Config& cfg);   // blocks until duration elapsed or stop()
    void         stop();                   // can be called from a signal handler

    // Stats since run() began.
    struct Stats {
        uint64_t                      frames_captured{0};
        uint64_t                      frames_encoded{0};
        uint64_t                      bytes_written{0};
        std::chrono::nanoseconds      wall_time{};
    };
    Stats stats() const;

private:
    Pipeline() = default;

    std::shared_ptr<D3dContext>                ctx_;
    std::unique_ptr<capture::ICapture>         capture_;
    std::unique_ptr<convert::ColorConverter>   converter_;
    std::unique_ptr<encode::IEncoder>          encoder_;
    std::unique_ptr<mux::IMuxer>               muxer_;
    ComPtr<ID3D11Texture2D>                    nv12_scratch_;   // staging NV12 written by converter

    // Audio chain (created only when Config::capture_audio is true).
    std::unique_ptr<audio::WasapiLoopback>     loopback_;
    std::unique_ptr<audio::Mixer>              mixer_;
    std::unique_ptr<audio::AacEncoder>         aac_encoder_;
    std::mutex                                 mux_mutex_;        // serialises write_audio + write_video

    std::atomic<bool>                          stop_requested_{false};
    mutable std::atomic<uint64_t>              frames_captured_{0};
    mutable std::atomic<uint64_t>              frames_encoded_{0};
    mutable std::atomic<uint64_t>              bytes_written_{0};
    std::chrono::steady_clock::time_point      started_at_{};
};

} // namespace gpur::core
