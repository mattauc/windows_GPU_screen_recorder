// Audio mixer + resampler.
//
// Phase 1.E.1 scope:
//   - Resample loopback blocks (any format/rate/channels) to the configured
//     output_format using libswresample.
//   - Pass through to the user's callback.
//   - Mic path is plumbed through the interface but currently dropped; real
//     loopback+mic mixing with proper sync alignment lands in v1.x once we
//     have a reliable PTS-aligned source-mixer scheme.

#include "mixer.h"
#include "shared/log.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
}

#include <atomic>
#include <mutex>

namespace gpur::core::audio {

namespace {

AVSampleFormat to_av_sample_fmt(SampleFormat f) {
    switch (f) {
        case SampleFormat::F32: return AV_SAMPLE_FMT_FLT;
        case SampleFormat::S16: return AV_SAMPLE_FMT_S16;
    }
    return AV_SAMPLE_FMT_NONE;
}

size_t bytes_per_sample(SampleFormat f) {
    switch (f) {
        case SampleFormat::F32: return 4;
        case SampleFormat::S16: return 2;
    }
    return 0;
}

bool same_format(const Format& a, const Format& b) {
    return a.sample_rate == b.sample_rate
        && a.channels    == b.channels
        && a.sample_fmt  == b.sample_fmt;
}

class MixerImpl final : public Mixer {
public:
    explicit MixerImpl(Params p) : params_(p) {}
    ~MixerImpl() override { (void)stop(); }

    Result<void> start(OnBlock cb) override {
        if (running_.exchange(true)) {
            return err(Error::make(Error::Code::AlreadyInitialised, "mixer already started"));
        }
        on_block_ = std::move(cb);
        GPUR_INFO("Mixer started: target {} Hz, {} ch, {} (loopback_gain={:.2f}, mic_gain={:.2f})",
                  params_.output_format.sample_rate,
                  params_.output_format.channels,
                  params_.output_format.sample_fmt == SampleFormat::F32 ? "F32" : "S16",
                  params_.loopback_gain, params_.mic_gain);
        return ok();
    }

    Result<void> stop() override {
        if (!running_.exchange(false)) return ok();
        std::scoped_lock lk(mu_);
        free_swr(swr_loopback_);
        free_swr(swr_mic_);
        on_block_ = nullptr;
        return ok();
    }

    Result<void> push_loopback(Block b) override {
        if (!running_.load()) return ok();
        return push(b, swr_loopback_, src_loopback_, params_.loopback_gain);
    }

    Result<void> push_mic(Block b) override {
        if (!running_.load()) return ok();
        // TODO(phase-1.x): mix into a shared output stream with loopback.
        // Today we silently drop mic input so the recording is loopback-only.
        (void)b;
        return ok();
    }

private:
    Result<void> push(Block& b, SwrContext*& swr, Format& src_remembered, float gain) {
        if (b.data.empty()) return ok();

        std::scoped_lock lk(mu_);

        if (!swr || !same_format(src_remembered, b.format)) {
            if (auto r = build_swr(swr, b.format); !r) return r;
            src_remembered = b.format;
        }

        const size_t src_sample_bytes  = bytes_per_sample(b.format.sample_fmt) * b.format.channels;
        if (src_sample_bytes == 0) {
            return err(Error::make(Error::Code::AudioFailed, "unsupported source sample format"));
        }
        const int in_samples = static_cast<int>(b.data.size() / src_sample_bytes);
        if (in_samples <= 0) return ok();

        // Worst case: upsample 8x. Allocate generously, swr will only fill what's needed.
        const int out_max_samples = static_cast<int>(av_rescale_rnd(
            in_samples,
            params_.output_format.sample_rate,
            b.format.sample_rate,
            AV_ROUND_UP)) + 32;

        const auto out_fmt = params_.output_format;
        const size_t out_sample_bytes = bytes_per_sample(out_fmt.sample_fmt) * out_fmt.channels;

        std::vector<uint8_t> out_buf(static_cast<size_t>(out_max_samples) * out_sample_bytes);
        uint8_t* out_ptrs[1] = {out_buf.data()};
        const uint8_t* in_ptrs[1] = {b.data.data()};

        int got = swr_convert(swr, out_ptrs, out_max_samples, in_ptrs, in_samples);
        if (got < 0) {
            return err(Error::make(Error::Code::AudioFailed, "swr_convert failed"));
        }
        if (got == 0) return ok();

        out_buf.resize(static_cast<size_t>(got) * out_sample_bytes);

        // Apply gain (skip when ~1.0 to save cycles).
        if (gain != 1.0f) {
            apply_gain(out_buf.data(), got, out_fmt, gain);
        }

        Block out_block;
        out_block.format   = out_fmt;
        out_block.data     = std::move(out_buf);
        out_block.pts      = b.pts;
        out_block.duration = std::chrono::nanoseconds(
            (static_cast<int64_t>(got) * 1'000'000'000LL) / out_fmt.sample_rate);

        if (on_block_) on_block_(std::move(out_block));
        return ok();
    }

    Result<void> build_swr(SwrContext*& swr, const Format& src) {
        free_swr(swr);

        AVChannelLayout in_layout{}, out_layout{};
        av_channel_layout_default(&in_layout,  static_cast<int>(src.channels));
        av_channel_layout_default(&out_layout, static_cast<int>(params_.output_format.channels));

        int rc = swr_alloc_set_opts2(
            &swr,
            &out_layout, to_av_sample_fmt(params_.output_format.sample_fmt),
            static_cast<int>(params_.output_format.sample_rate),
            &in_layout,  to_av_sample_fmt(src.sample_fmt),
            static_cast<int>(src.sample_rate),
            0, nullptr);

        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);

        if (rc < 0 || !swr) {
            return err(Error::make(Error::Code::AudioFailed, "swr_alloc_set_opts2 failed"));
        }
        rc = swr_init(swr);
        if (rc < 0) {
            free_swr(swr);
            return err(Error::make(Error::Code::AudioFailed, "swr_init failed"));
        }
        return ok();
    }

    void free_swr(SwrContext*& swr) {
        if (swr) {
            swr_free(&swr);
            swr = nullptr;
        }
    }

    static void apply_gain(uint8_t* data, int samples, const Format& fmt, float gain) {
        const int total = samples * static_cast<int>(fmt.channels);
        if (fmt.sample_fmt == SampleFormat::F32) {
            auto* f = reinterpret_cast<float*>(data);
            for (int i = 0; i < total; ++i) {
                float v = f[i] * gain;
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                f[i] = v;
            }
        } else {
            auto* s = reinterpret_cast<int16_t*>(data);
            for (int i = 0; i < total; ++i) {
                int32_t v = static_cast<int32_t>(s[i] * gain);
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                s[i] = static_cast<int16_t>(v);
            }
        }
    }

    Params              params_;
    std::mutex          mu_;
    SwrContext*         swr_loopback_ {nullptr};
    SwrContext*         swr_mic_      {nullptr};
    Format              src_loopback_ {};
    Format              src_mic_      {};
    std::atomic<bool>   running_      {false};
    OnBlock             on_block_;
};

} // namespace

std::unique_ptr<Mixer> Mixer::create(const Params& params) {
    return std::make_unique<MixerImpl>(params);
}

} // namespace gpur::core::audio
