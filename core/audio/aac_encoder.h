#pragma once

#include "audio_types.h"

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace gpur::core::audio {

// AAC-LC encoder via Media Foundation's AAC MFT.
//
// Input PCM is expected at the configured sample_rate/channels in F32 or S16.
// We convert F32 -> S16 internally because the MS AAC encoder MFT only
// accepts 16-bit PCM input.
//
// Output: raw AAC frames (no ADTS headers). Caller is expected to pass
// the AudioSpecificConfig (returned from initialise) into the muxer's
// codecpar->extradata so the container can decode without ADTS framing.
class AacEncoder {
public:
    struct Params {
        uint32_t sample_rate{48000};
        uint32_t channels{2};
        uint32_t bitrate_bps{192000};   // 192 kbps AAC LC
    };

    struct Packet {
        std::vector<uint8_t>      data;
        std::chrono::nanoseconds  pts{};
        std::chrono::nanoseconds  duration{};
    };

    using OnPacket = std::function<void(Packet)>;

    static std::unique_ptr<AacEncoder> create();
    virtual ~AacEncoder() = default;

    // Returns the AudioSpecificConfig blob to be set as muxer extradata.
    // (May be empty if the MFT doesn't expose it; callers should handle that.)
    virtual Result<std::vector<uint8_t>> initialise(const Params& p, OnPacket cb) = 0;

    virtual Result<void> push(const Block& pcm) = 0;
    virtual Result<void> drain()                = 0;
    virtual Result<void> shutdown()             = 0;

    virtual const Params& params() const noexcept = 0;
};

} // namespace gpur::core::audio
