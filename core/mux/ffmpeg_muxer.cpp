// Two muxer implementations live here:
//   1. AnnexBFileWriter (Phase 0): raw NALU output to a .h264/.h265 file.
//   2. FfmpegMatroskaMuxer (Phase 1): libavformat-based .mkv with proper
//      container framing. Matroska accepts H.264/HEVC Annex-B input natively,
//      so we don't need an avcC bitstream conversion here. MP4 with avcC
//      conversion is Phase 1.B (separate file once written).

#include "imuxer.h"
#include "shared/log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cstring>
#include <fstream>

namespace gpur::core::mux {

namespace {

std::string av_err(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

// -----------------------------------------------------------------------------
// AnnexBFileWriter — Phase 0 raw-NALU output.
// -----------------------------------------------------------------------------

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

    Result<void> write_audio(const EncodedAudioPacket&) override {
        // Phase 0 raw-bitstream writer ignores audio. We could splice an
        // .aac file alongside, but that's not a useful workflow.
        return ok();
    }

private:
    std::ofstream file_;
    uint64_t      bytes_written_{0};
};

// -----------------------------------------------------------------------------
// FfmpegMatroskaMuxer — Phase 1 .mkv output.
//
// Why MKV first: Matroska accepts H.264/HEVC NALUs in Annex-B framing directly
// (start-code framed), with no bitstream conversion needed. NVENC emits
// Annex-B by default, so we can pass packets straight through.
//
// MP4 needs avcC framing (length-prefixed NALUs) + the SPS/PPS extracted into
// the codecpar->extradata box. That's the next chunk (Phase 1.B), in its own
// translation unit.
// -----------------------------------------------------------------------------

class FfmpegMatroskaMuxer final : public IMuxer {
public:
    ~FfmpegMatroskaMuxer() override { (void)close(); }

    Result<void> open(const OpenParams& params) override {
        if (fmt_ctx_) {
            return err(Error::make(Error::Code::AlreadyInitialised, "muxer already open"));
        }
        params_ = params;
        first_pts_ns_ = -1;

        int rc = avformat_alloc_output_context2(
            &fmt_ctx_, nullptr, "matroska", params.output_path.string().c_str());
        if (rc < 0 || !fmt_ctx_) {
            return err(Error::make(Error::Code::MuxerFailed,
                                    "avformat_alloc_output_context2 failed: " + av_err(rc)));
        }

        video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
        if (!video_stream_) {
            (void)close();
            return err(Error::make(Error::Code::MuxerFailed, "avformat_new_stream failed"));
        }

        auto* cp = video_stream_->codecpar;
        cp->codec_type = AVMEDIA_TYPE_VIDEO;
        cp->codec_id   = (params.video_codec == encode::Codec::HEVC)
            ? AV_CODEC_ID_HEVC
            : AV_CODEC_ID_H264;
        cp->codec_tag  = 0;
        cp->width      = static_cast<int>(params.video_width);
        cp->height     = static_cast<int>(params.video_height);
        cp->format     = AV_PIX_FMT_NV12;

        // Drive the muxer in nanoseconds so we don't lose precision on PTS.
        // Matroska will rescale internally to its own 1ms grid.
        video_stream_->time_base       = AVRational{1, 1'000'000'000};
        video_stream_->avg_frame_rate  = AVRational{static_cast<int>(params.video_fps), 1};
        video_stream_->r_frame_rate    = AVRational{static_cast<int>(params.video_fps), 1};

        // Optional audio stream — AAC LC with extradata supplied by the caller.
        if (params.has_audio) {
            audio_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
            if (!audio_stream_) {
                (void)close();
                return err(Error::make(Error::Code::MuxerFailed,
                                        "avformat_new_stream(audio) failed"));
            }
            auto* acp = audio_stream_->codecpar;
            acp->codec_type  = AVMEDIA_TYPE_AUDIO;
            acp->codec_id    = AV_CODEC_ID_AAC;
            acp->codec_tag   = 0;
            acp->sample_rate = static_cast<int>(params.audio_format.sample_rate);
            acp->bit_rate    = params.audio_bitrate_bps;
            acp->frame_size  = 1024;                        // AAC LC frame
            acp->format      = AV_SAMPLE_FMT_S16;
            av_channel_layout_default(&acp->ch_layout,
                                       static_cast<int>(params.audio_format.channels));

            // Codec-private data: AudioSpecificConfig. Without this, decoders
            // can't parse the raw AAC frames we're about to mux.
            if (!params.audio_codec_config.empty()) {
                acp->extradata = static_cast<uint8_t*>(
                    av_mallocz(params.audio_codec_config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (!acp->extradata) {
                    (void)close();
                    return err(Error::make(Error::Code::MuxerFailed,
                                            "av_mallocz(audio extradata) failed"));
                }
                std::memcpy(acp->extradata, params.audio_codec_config.data(),
                            params.audio_codec_config.size());
                acp->extradata_size = static_cast<int>(params.audio_codec_config.size());
            }

            audio_stream_->time_base = AVRational{1, 1'000'000'000};
        }

        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            rc = avio_open(&fmt_ctx_->pb, params.output_path.string().c_str(), AVIO_FLAG_WRITE);
            if (rc < 0) {
                (void)close();
                return err(Error::make(Error::Code::MuxerFailed,
                                        "avio_open failed: " + av_err(rc)));
            }
        }

        // Header is deferred until the FIRST video packet arrives so that a
        // future MP4 backend can populate avcC extradata from the bitstream.
        GPUR_INFO("FFmpeg muxer opened: {} ({}x{} @ {}fps, codec={}{})",
                  params.output_path.string(), params.video_width, params.video_height,
                  params.video_fps,
                  params.video_codec == encode::Codec::HEVC ? "HEVC" : "H.264",
                  params.has_audio ? ", +AAC audio" : "");
        return ok();
    }

    Result<void> close() override {
        if (!fmt_ctx_) return ok();

        if (header_written_) {
            av_write_trailer(fmt_ctx_);
        }
        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE) && fmt_ctx_->pb) {
            avio_closep(&fmt_ctx_->pb);
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_       = nullptr;
        video_stream_  = nullptr;
        audio_stream_  = nullptr;
        header_written_= false;
        return ok();
    }

    Result<void> write_video(const encode::IEncoder::EncodedPacket& pkt) override {
        if (!fmt_ctx_) {
            return err(Error::make(Error::Code::NotInitialised, "muxer not open"));
        }
        if (pkt.data.empty()) return ok();

        if (!header_written_) {
            int rc = avformat_write_header(fmt_ctx_, nullptr);
            if (rc < 0) {
                return err(Error::make(Error::Code::MuxerFailed,
                                        "avformat_write_header failed: " + av_err(rc)));
            }
            header_written_ = true;
        }

        if (first_pts_ns_ < 0) {
            first_pts_ns_ = pkt.pts.count();
        }
        const int64_t pts_ns = pkt.pts.count() - first_pts_ns_;
        const int64_t dts_ns = pkt.dts.count() - first_pts_ns_;

        AVPacket* p = av_packet_alloc();
        if (!p) {
            return err(Error::make(Error::Code::MuxerFailed, "av_packet_alloc failed"));
        }

        // Copy into a refcounted AV buffer. Cheaper than I'd like, but it's
        // 5-50 KB / frame at typical bitrates so still negligible CPU.
        int rc = av_new_packet(p, static_cast<int>(pkt.data.size()));
        if (rc < 0) {
            av_packet_free(&p);
            return err(Error::make(Error::Code::MuxerFailed,
                                    "av_new_packet failed: " + av_err(rc)));
        }
        std::memcpy(p->data, pkt.data.data(), pkt.data.size());

        p->stream_index = video_stream_->index;
        p->pts          = pts_ns;
        p->dts          = dts_ns;
        p->duration     = 1'000'000'000LL / std::max<uint32_t>(params_.video_fps, 1);
        p->flags        = pkt.keyframe ? AV_PKT_FLAG_KEY : 0;

        rc = av_interleaved_write_frame(fmt_ctx_, p);
        av_packet_free(&p);
        if (rc < 0) {
            return err(Error::make(Error::Code::MuxerFailed,
                                    "av_interleaved_write_frame failed: " + av_err(rc)));
        }
        return ok();
    }

    Result<void> write_audio(const EncodedAudioPacket& pkt) override {
        if (!fmt_ctx_)     return err(Error::make(Error::Code::NotInitialised, "muxer not open"));
        if (!audio_stream_) return ok();   // audio not configured for this session
        if (pkt.data.empty()) return ok();

        // Audio packets are written after the first video packet to ensure
        // the header (and time-base rebasing) is already established.
        if (!header_written_) {
            return ok();   // drop pre-roll audio rather than write before header
        }

        if (first_pts_ns_ < 0) first_pts_ns_ = pkt.pts.count();
        const int64_t pts_ns = pkt.pts.count() - first_pts_ns_;

        AVPacket* p = av_packet_alloc();
        if (!p) return err(Error::make(Error::Code::MuxerFailed, "av_packet_alloc(audio) failed"));

        int rc = av_new_packet(p, static_cast<int>(pkt.data.size()));
        if (rc < 0) {
            av_packet_free(&p);
            return err(Error::make(Error::Code::MuxerFailed,
                                    "av_new_packet(audio) failed: " + av_err(rc)));
        }
        std::memcpy(p->data, pkt.data.data(), pkt.data.size());

        p->stream_index = audio_stream_->index;
        p->pts          = pts_ns;
        p->dts          = pts_ns;
        p->duration     = pkt.duration.count();
        p->flags        = AV_PKT_FLAG_KEY;   // every AAC frame is self-contained

        rc = av_interleaved_write_frame(fmt_ctx_, p);
        av_packet_free(&p);
        if (rc < 0) {
            return err(Error::make(Error::Code::MuxerFailed,
                                    "av_interleaved_write_frame(audio) failed: " + av_err(rc)));
        }
        return ok();
    }

private:
    AVFormatContext* fmt_ctx_       {nullptr};
    AVStream*        video_stream_  {nullptr};
    AVStream*        audio_stream_  {nullptr};
    OpenParams       params_        {};
    int64_t          first_pts_ns_  {-1};
    bool             header_written_{false};
};

} // namespace

std::unique_ptr<IMuxer> make_annexb_writer() {
    return std::make_unique<AnnexBFileWriter>();
}

std::unique_ptr<IMuxer> make_ffmpeg_muxer() {
    return std::make_unique<FfmpegMatroskaMuxer>();
}

std::unique_ptr<IMuxer> make_muxer_for_path(const std::filesystem::path& output) {
    auto ext = output.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    // Raw bitstream — Phase 0 default, useful for debugging the encoder.
    if (ext == ".h264" || ext == ".264" || ext == ".h265" || ext == ".265" || ext == ".hevc") {
        return make_annexb_writer();
    }
    // Everything else goes through FFmpeg. Today that means .mkv reliably and
    // .mp4 with caveats (see Phase 1.B).
    return make_ffmpeg_muxer();
}

} // namespace gpur::core::mux
