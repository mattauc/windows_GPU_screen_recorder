#pragma once

#include "shared/result.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace gpur::core::audio {

enum class SampleFormat {
    S16,
    F32,
};

struct Format {
    uint32_t      sample_rate{48000};
    uint32_t      channels{2};
    SampleFormat  sample_fmt{SampleFormat::F32};
};

// One block of PCM audio. Interleaved samples, format described by `format`.
struct Block {
    Format                          format;
    std::vector<uint8_t>            data;
    std::chrono::nanoseconds        pts{};
    std::chrono::nanoseconds        duration{};
};

} // namespace gpur::core::audio
