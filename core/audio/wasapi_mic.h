#pragma once

#include "audio_types.h"

#include <functional>
#include <memory>
#include <string>

namespace gpur::core::audio {

// Microphone capture via WASAPI shared-mode capture stream.
//
// STATUS: stub. Phase 1 work.
class WasapiMic {
public:
    using OnBlock = std::function<void(Block)>;

    static std::unique_ptr<WasapiMic> create(std::wstring_view device_id = {});
    virtual ~WasapiMic() = default;

    virtual Result<void> start(OnBlock cb)        = 0;
    virtual Result<void> stop()                   = 0;
    virtual const Format& format() const noexcept = 0;
};

} // namespace gpur::core::audio
