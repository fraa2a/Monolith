#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace encoding::detail {
struct PcmInput {
    const uint8_t* data = nullptr;
    int frames = 0;
    int bit_depth = 0;
    std::vector<uint8_t> silence;
    std::vector<int32_t> expanded;

    bool prepare(const uint8_t* input, int bytes, int channels, int bits, bool floating)
    {
        if (bytes <= 0 || channels <= 0 || channels > 64) return false;
        if (floating ? (bits != 32 && bits != 64)
                     : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
        const int frame_bytes = (bits / 8) * channels;
        if (bytes % frame_bytes != 0) return false;
        frames = bytes / frame_bytes;
        bit_depth = bits;
        data = input;
        if (!data) {
            silence.assign(static_cast<size_t>(bytes), bits == 8 ? 128 : 0);
            data = silence.data();
        }
        if (bits == 24) {
            expanded.resize(static_cast<size_t>(frames) * channels);
            for (size_t i = 0; i < expanded.size(); ++i) {
                // Packed little-endian PCM becomes left-aligned signed S32.
                const uint32_t sample = (uint32_t(data[i * 3]) << 8) |
                    (uint32_t(data[i * 3 + 1]) << 16) | (uint32_t(data[i * 3 + 2]) << 24);
                std::memcpy(&expanded[i], &sample, sizeof(sample));
            }
            data = reinterpret_cast<const uint8_t*>(expanded.data());
            bit_depth = 32;
        }
        return frames > 0;
    }
};
}
