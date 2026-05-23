#pragma once

#include "core/audio/audio_types.h"
#include "core/encode/iencoder.h"
#include "shared/result.h"

#include <filesystem>
#include <memory>
#include <string>

namespace gpur::core::mux {

// Sink for encoded video (+ audio later). Two implementations:
//   - AnnexBFileWriter: writes raw H.264/HEVC Annex-B NALUs to a file.
//     Phase 0 default. No audio, no container, no recovery on crash.
//   - FfmpegMuxer: real MP4/MKV with audio. Phase 1.
class IMuxer {
public:
    virtual ~IMuxer() = default;

    struct OpenParams {
        std::filesystem::path   output_path;
        encode::Codec           video_codec{encode::Codec::H264};
        uint32_t                video_fps{60};
        uint32_t                video_width{};
        uint32_t                video_height{};

        // Audio (ignored by Phase 0 AnnexBFileWriter):
        bool                    has_audio{false};
        audio::Format           audio_format{};
    };

    virtual Result<void> open(const OpenParams& params) = 0;
    virtual Result<void> close()                        = 0;
    virtual Result<void> write_video(const encode::IEncoder::EncodedPacket& pkt) = 0;
    virtual Result<void> write_audio(const audio::Block& block)                  = 0;
};

// Phase 0 muxer — just writes Annex-B NAL units to disk.
std::unique_ptr<IMuxer> make_annexb_writer();

// Phase 1 muxer — FFmpeg libavformat. Matroska (.mkv) only for now;
// MP4 with avcC bitstream conversion lands in Phase 1.B.
std::unique_ptr<IMuxer> make_ffmpeg_muxer();

// Convenience: pick the right muxer based on the output file extension.
//   .h264 / .265 / .hevc          -> AnnexBFileWriter
//   .mkv / .mp4 / anything else   -> FfmpegMatroskaMuxer
std::unique_ptr<IMuxer> make_muxer_for_path(const std::filesystem::path& output);

} // namespace gpur::core::mux
