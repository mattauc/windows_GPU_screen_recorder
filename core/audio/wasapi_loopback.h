#pragma once

#include "audio_types.h"

#include <functional>
#include <memory>
#include <string>

namespace gpur::core::audio {

// System audio capture via WASAPI loopback (renders coming out of the default
// playback device).
//
// STATUS: stub. Phase 1 work.
class WasapiLoopback {
public:
    using OnBlock = std::function<void(Block)>;

    static std::unique_ptr<WasapiLoopback> create();
    virtual ~WasapiLoopback() = default;

    virtual Result<void> start(OnBlock cb)        = 0;
    virtual Result<void> stop()                   = 0;
    virtual const Format& format() const noexcept = 0;
};

} // namespace gpur::core::audio
