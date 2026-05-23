// Audio mixer + resampler.
//
// TODO(phase-1): implement.
//   - Per-source ring buffer aligned on the output_format's sample rate.
//   - Sample-rate conversion (libsamplerate, or hand-rolled linear for now).
//   - Mix loopback + mic with gain weights, clamp to F32 [-1, 1].
//   - Emit output blocks at a fixed cadence (~20 ms) for downstream encoder.

#include "mixer.h"
#include "shared/log.h"

namespace gpur::core::audio {

namespace {
class StubMixer final : public Mixer {
public:
    explicit StubMixer(Params p) : params_(p) {}
    Result<void> push_loopback(Block) override {
        return err(Error::not_implemented("Mixer::push_loopback (phase 1)"));
    }
    Result<void> push_mic(Block) override {
        return err(Error::not_implemented("Mixer::push_mic (phase 1)"));
    }
    Result<void> start(OnBlock) override {
        return err(Error::not_implemented("Mixer::start (phase 1)"));
    }
    Result<void> stop() override { return ok(); }
private:
    Params params_;
};
} // namespace

std::unique_ptr<Mixer> Mixer::create(const Params& params) {
    return std::make_unique<StubMixer>(params);
}

} // namespace gpur::core::audio
