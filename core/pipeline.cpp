#include "pipeline.h"
#include "core/encode/nvenc_encoder.h"
#include "shared/log.h"

#include <chrono>

namespace gpur::core {

namespace {
constexpr std::chrono::milliseconds kFrameWait{200};
} // namespace

Result<std::unique_ptr<Pipeline>> Pipeline::create(std::shared_ptr<D3dContext> ctx) {
    auto self  = std::unique_ptr<Pipeline>(new Pipeline());
    self->ctx_ = std::move(ctx);
    return self;
}

Pipeline::~Pipeline() = default;

Result<void> Pipeline::run(const Config& cfg) {
    // 1. Capture
    auto cap = capture::make_capture(ctx_);
    if (!cap) return err(std::move(cap.error()));
    capture_ = std::move(*cap);

    capture::ICapture::StartParams cap_params{};
    cap_params.target         = cfg.target;
    cap_params.fps_hint       = cfg.encoder.fps;
    cap_params.include_cursor = true;
    if (auto r = capture_->start(cap_params); !r) return err(std::move(r.error()));

    // Width/height may need to come from the capturer rather than caller (the
    // monitor dimensions). Fix up the encoder params if caller passed zeros.
    encode::EncoderParams enc_params = cfg.encoder;
    if (enc_params.width == 0)  enc_params.width  = capture_->width();
    if (enc_params.height == 0) enc_params.height = capture_->height();

    // 2. Encoder
    auto enc = encode::make_encoder(ctx_);
    if (!enc) {
        (void)capture_->stop();
        return err(std::move(enc.error()));
    }
    encoder_ = std::move(*enc);
    if (auto r = encoder_->initialise(enc_params); !r) {
        (void)capture_->stop();
        return err(std::move(r.error()));
    }

    // 3. Converter + NV12 scratch texture
    auto conv = convert::ColorConverter::create(ctx_);
    if (!conv) {
        (void)encoder_->shutdown();
        (void)capture_->stop();
        return err(std::move(conv.error()));
    }
    converter_ = std::move(*conv);

    auto nv12 = encode::create_nv12_input_texture(*ctx_, enc_params.width, enc_params.height);
    if (!nv12) {
        return err(std::move(nv12.error()));
    }
    nv12_scratch_ = *nv12;

    // 4. Audio chain (must be initialised before muxer.open so we have the
    //    AAC AudioSpecificConfig to pass as extradata).
    std::vector<uint8_t> aac_codec_config;
    audio::Format        mixer_output_format{48'000, 2, audio::SampleFormat::F32};

    if (cfg.capture_audio) {
        loopback_ = audio::WasapiLoopback::create();

        audio::Mixer::Params mp{};
        mp.output_format  = mixer_output_format;
        mp.loopback_gain  = 1.0f;
        mp.mic_gain       = 0.0f;
        mixer_            = audio::Mixer::create(mp);

        aac_encoder_      = audio::AacEncoder::create();
        audio::AacEncoder::Params ap{};
        ap.sample_rate    = mixer_output_format.sample_rate;
        ap.channels       = mixer_output_format.channels;
        ap.bitrate_bps    = cfg.audio_bitrate_bps;

        auto cfg_res = aac_encoder_->initialise(ap,
            [this](audio::AacEncoder::Packet pkt) {
                mux::IMuxer::EncodedAudioPacket ep;
                ep.data     = std::move(pkt.data);
                ep.pts      = pkt.pts;
                ep.duration = pkt.duration;
                std::scoped_lock lk(mux_mutex_);
                if (muxer_) (void)muxer_->write_audio(ep);
            });
        if (!cfg_res) {
            GPUR_WARN("AAC encoder init failed: {} — recording without audio",
                      cfg_res.error().message);
            aac_encoder_.reset();
            mixer_.reset();
            loopback_.reset();
        } else {
            aac_codec_config = std::move(*cfg_res);
        }
    }

    // 5. Muxer — picks AnnexB for raw .h264/.h265, FFmpeg MKV otherwise.
    muxer_ = mux::make_muxer_for_path(cfg.output_path);
    mux::IMuxer::OpenParams mux_params{};
    mux_params.output_path        = cfg.output_path;
    mux_params.video_codec        = enc_params.codec;
    mux_params.video_fps          = enc_params.fps;
    mux_params.video_width        = enc_params.width;
    mux_params.video_height       = enc_params.height;
    mux_params.has_audio          = (aac_encoder_ != nullptr);
    mux_params.audio_format       = mixer_output_format;
    mux_params.audio_codec_config = aac_codec_config;
    mux_params.audio_bitrate_bps  = cfg.audio_bitrate_bps;
    if (auto r = muxer_->open(mux_params); !r) return err(std::move(r.error()));

    // 6. Start audio capture (mixer -> aac_encoder is set up; loopback pumps
    //    blocks through them).
    if (loopback_ && mixer_ && aac_encoder_) {
        if (auto r = mixer_->start([this](audio::Block b) {
                if (aac_encoder_) (void)aac_encoder_->push(b);
            }); !r) {
            GPUR_WARN("Mixer start failed: {} — recording without audio", r.error().message);
            mixer_.reset();
            aac_encoder_.reset();
            loopback_.reset();
        } else if (auto r2 = loopback_->start([this](audio::Block b) {
                if (mixer_) (void)mixer_->push_loopback(std::move(b));
            }); !r2) {
            GPUR_WARN("Loopback start failed: {} — recording without audio", r2.error().message);
            (void)mixer_->stop();
            mixer_.reset();
            aac_encoder_.reset();
            loopback_.reset();
        }
    }

    GPUR_INFO("Pipeline running: {}x{} @ {}fps → {}",
              enc_params.width, enc_params.height, enc_params.fps,
              cfg.output_path.string());

    // 5. Pump loop
    started_at_ = std::chrono::steady_clock::now();
    auto deadline = (cfg.duration.count() > 0)
        ? started_at_ + cfg.duration
        : std::chrono::steady_clock::time_point::max();

    while (!stop_requested_.load() && std::chrono::steady_clock::now() < deadline) {
        auto frame = capture_->next_frame(kFrameWait);
        if (!frame) {
            if (frame.error().code == Error::Code::Cancelled) break;
            if (frame.error().code == Error::Code::CaptureFailed) {
                continue; // timeout; loop back and check stop_requested_
            }
            return err(std::move(frame.error()));
        }
        frames_captured_.fetch_add(1, std::memory_order_relaxed);

        // GPU color convert: BGRA → NV12 (no CPU touch).
        if (auto r = converter_->convert(frame->texture.Get(), nv12_scratch_.Get()); !r) {
            return err(std::move(r.error()));
        }

        // Submit to encoder.
        if (auto r = encoder_->submit(nv12_scratch_.Get(), frame->timestamp); !r) {
            return err(std::move(r.error()));
        }

        // Drain encoded packets.
        auto packets = encoder_->poll();
        if (!packets) return err(std::move(packets.error()));
        for (auto& pkt : *packets) {
            bytes_written_.fetch_add(pkt.data.size(), std::memory_order_relaxed);
            frames_encoded_.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lk(mux_mutex_);
            if (auto r = muxer_->write_video(pkt); !r) return err(std::move(r.error()));
        }
    }

    // Shut down audio first so the AAC encoder's drained packets get muxed
    // before we close the file.
    if (loopback_) (void)loopback_->stop();
    if (mixer_)    (void)mixer_->stop();
    if (aac_encoder_) {
        (void)aac_encoder_->drain();
        (void)aac_encoder_->shutdown();
    }

    // Flush encoder + write trailing packets.
    if (auto tail = encoder_->flush(); tail) {
        for (auto& pkt : *tail) {
            bytes_written_.fetch_add(pkt.data.size(), std::memory_order_relaxed);
            std::scoped_lock lk(mux_mutex_);
            (void)muxer_->write_video(pkt);
        }
    }

    {
        std::scoped_lock lk(mux_mutex_);
        (void)muxer_->close();
    }
    (void)encoder_->shutdown();
    (void)capture_->stop();

    GPUR_INFO("Pipeline stopped. {} frames captured, {} encoded, {} bytes written",
              frames_captured_.load(), frames_encoded_.load(), bytes_written_.load());
    return ok();
}

void Pipeline::stop() {
    stop_requested_.store(true);
}

Pipeline::Stats Pipeline::stats() const {
    Stats s{};
    s.frames_captured = frames_captured_.load();
    s.frames_encoded  = frames_encoded_.load();
    s.bytes_written   = bytes_written_.load();
    if (started_at_.time_since_epoch().count() != 0) {
        s.wall_time = std::chrono::steady_clock::now() - started_at_;
    }
    return s;
}

} // namespace gpur::core
