// Replay buffer.
//
// TODO(phase-2): real ring implementation. For now this is a no-op
// pass-through so callers compile without conditional code.

#include "replay_buffer.h"
#include "shared/log.h"

namespace gpur::core::mux {

namespace {
class StubReplayBuffer final : public ReplayBuffer {
public:
    explicit StubReplayBuffer(Params p) : params_(p) {}
    void push_video(encode::IEncoder::EncodedPacket) override {}
    void push_audio(audio::Block) override {}
    Snapshot snapshot(std::chrono::seconds) override { return {}; }
    void clear() override {}
private:
    Params params_;
};
} // namespace

std::unique_ptr<ReplayBuffer> ReplayBuffer::create(const Params& p) {
    return std::make_unique<StubReplayBuffer>(p);
}

} // namespace gpur::core::mux
