#pragma once

#include "core/d3d_context.h"
#include "shared/result.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gpur::core::encode {

using Microsoft::WRL::ComPtr;

enum class Codec {
    H264,
    HEVC,
    AV1,   // future / NVENC AD102+
};

enum class RateControl {
    ConstantBitrate,
    VariableBitrate,
    ConstantQuality,   // CQP / CRF analogue
};

enum class Profile {
    Baseline,
    Main,
    High,
    Main10,            // HEVC 10-bit
};

struct EncoderParams {
    uint32_t        width{};
    uint32_t        height{};
    uint32_t        fps{60};
    uint32_t        bitrate_bps{50'000'000};
    uint32_t        keyframe_interval_frames{120}; // 2s @ 60fps
    Codec           codec{Codec::H264};
    Profile         profile{Profile::High};
    RateControl     rate_control{RateControl::ConstantBitrate};
    uint32_t        cqp_quality{20};   // only when rate_control == ConstantQuality
    uint32_t        bframes{0};        // 0 = no B-frames (lowest latency)
    bool            low_latency{true};
};

// Encoder consumes NV12 D3D11 textures and produces raw Annex-B NAL units.
// Implementations: NvencEncoder (NVIDIA), AmfEncoder (AMD), QsvEncoder (Intel).
class IEncoder {
public:
    virtual ~IEncoder() = default;

    struct EncodedPacket {
        std::vector<uint8_t>      data;          // Annex-B framed
        std::chrono::nanoseconds  pts{};
        std::chrono::nanoseconds  dts{};
        bool                      keyframe{false};
    };

    virtual Result<void> initialise(const EncoderParams& params) = 0;
    virtual Result<void> shutdown()                              = 0;

    // Submit one NV12 texture for encoding. Caller retains ownership of the
    // texture; the encoder copies/references it as needed for its session.
    virtual Result<void> submit(ID3D11Texture2D* nv12_frame,
                                std::chrono::nanoseconds pts) = 0;

    // Request a forced keyframe on the next submitted frame.
    virtual Result<void> request_keyframe() = 0;

    // Drain any encoded packets currently available. Non-blocking.
    virtual Result<std::vector<EncodedPacket>> poll() = 0;

    // Flush encoder, returning all remaining packets. Call before shutdown.
    virtual Result<std::vector<EncodedPacket>> flush() = 0;

    virtual const EncoderParams& params() const noexcept = 0;
    virtual std::string_view backend_name() const noexcept = 0;
};

// Backend selection. Falls through NVENC → AMF → QSV based on detected GPU.
Result<std::unique_ptr<IEncoder>> make_encoder(std::shared_ptr<D3dContext> ctx);

} // namespace gpur::core::encode
