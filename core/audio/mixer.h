#pragma once

#include "audio_types.h"

#include <functional>
#include <memory>

namespace gpur::core::audio {

// Mixes loopback (system) + microphone into a single stream at a target
// format. Handles per-source gain and sample-rate conversion.
//
// STATUS: stub. Phase 1 work.
class Mixer {
public:
    struct Params {
        Format   output_format;
        float    loopback_gain{1.0f};
        float    mic_gain{1.0f};
    };

    using OnBlock = std::function<void(Block)>;

    static std::unique_ptr<Mixer> create(const Params& params);
    virtual ~Mixer() = default;

    virtual Result<void> push_loopback(Block b) = 0;
    virtual Result<void> push_mic(Block b)      = 0;

    virtual Result<void> start(OnBlock cb) = 0;
    virtual Result<void> stop()            = 0;
};

} // namespace gpur::core::audio
