// Two muxer implementations live here:
//   1. AnnexBFileWriter (Phase 0): raw NALU output to a .h264/.h265 file.
//   2. FfmpegMuxer (Phase 1): libavformat-based MP4/MKV with audio.
//
// Phase 0 only requires (1). FFmpeg is intentionally NOT linked yet to keep
// the build light. Switch the vcpkg manifest feature `mux` on when Phase 1
// starts.

#include "imuxer.h"
#include "shared/log.h"

#include <fstream>

namespace gpur::core::mux {

namespace {

class AnnexBFileWriter final : public IMuxer {
public:
    Result<void> open(const OpenParams& params) override {
        if (file_.is_open()) {
            return err(Error::make(Error::Code::AlreadyInitialised, "AnnexB writer already open"));
        }
        file_.open(params.output_path, std::ios::binary | std::ios::trunc);
        if (!file_) {
            return err(Error::make(Error::Code::IoFailed,
                                    "failed to open " + params.output_path.string()));
        }
        GPUR_INFO("AnnexB writer opened: {} ({}x{}, codec={})",
                  params.output_path.string(), params.video_width, params.video_height,
                  params.video_codec == encode::Codec::HEVC ? "HEVC" : "H.264");
        return ok();
    }

    Result<void> close() override {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        return ok();
    }

    Result<void> write_video(const encode::IEncoder::EncodedPacket& pkt) override {
        if (!file_.is_open()) {
            return err(Error::make(Error::Code::NotInitialised, "AnnexB writer not open"));
        }
        file_.write(reinterpret_cast<const char*>(pkt.data.data()),
                    static_cast<std::streamsize>(pkt.data.size()));
        if (!file_) {
            return err(Error::make(Error::Code::IoFailed, "write failed"));
        }
        bytes_written_ += pkt.data.size();
        return ok();
    }

    Result<void> write_audio(const audio::Block&) override {
        // Phase 0 doesn't write audio. Silently drop.
        return ok();
    }

private:
    std::ofstream file_;
    uint64_t      bytes_written_{0};
};

class FfmpegMuxerStub final : public IMuxer {
public:
    Result<void> open(const OpenParams&) override {
        return err(Error::not_implemented("FFmpeg muxer (phase 1)"));
    }
    Result<void> close() override { return ok(); }
    Result<void> write_video(const encode::IEncoder::EncodedPacket&) override {
        return err(Error::not_implemented("FfmpegMuxer::write_video"));
    }
    Result<void> write_audio(const audio::Block&) override {
        return err(Error::not_implemented("FfmpegMuxer::write_audio"));
    }
};

} // namespace

std::unique_ptr<IMuxer> make_annexb_writer() {
    return std::make_unique<AnnexBFileWriter>();
}

std::unique_ptr<IMuxer> make_ffmpeg_muxer() {
    return std::make_unique<FfmpegMuxerStub>();
}

} // namespace gpur::core::mux
