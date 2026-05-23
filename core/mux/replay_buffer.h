#pragma once

#include "core/audio/audio_types.h"
#include "core/encode/iencoder.h"
#include "shared/result.h"

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace gpur::core::mux {

// Ring buffer of encoded packets — the heart of Instant Replay.
//
// STATUS: stub. Phase 2.
//
// Design:
//   - Holds encoded video packets (Annex-B) + encoded audio packets.
//   - Indexed by keyframe so save_last(N seconds) can trim cleanly without
//     re-encoding: find the last IDR <= start_time, drop everything before.
//   - Size-bounded by configurable window (default 5 minutes).
class ReplayBuffer {
public:
    struct Params {
        std::chrono::seconds   window{300};       // how much history to keep
        uint64_t               max_bytes{2ULL << 30}; // hard cap 2 GiB
    };

    static std::unique_ptr<ReplayBuffer> create(const Params& p);
    virtual ~ReplayBuffer() = default;

    virtual void push_video(encode::IEncoder::EncodedPacket pkt) = 0;
    virtual void push_audio(audio::Block block)                  = 0;

    struct Snapshot {
        std::vector<encode::IEncoder::EncodedPacket> video;
        std::vector<audio::Block>                    audio;
    };
    // Capture the most recent `duration` worth of packets, trimmed to the
    // last keyframe at or before the start instant.
    virtual Snapshot snapshot(std::chrono::seconds duration) = 0;

    virtual void clear() = 0;
};

} // namespace gpur::core::mux
