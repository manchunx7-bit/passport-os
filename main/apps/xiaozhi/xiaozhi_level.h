#pragma once

#include <cstddef>
#include <cstdint>

namespace xiaozhi::face {

constexpr int kWaveBars = 13;

// Mean absolute PCM amplitude with a quiet floor and compressed display range.
// Integer-only, bounded storage; INT16_MIN is widened before taking its absolute.
inline uint8_t pcm_level(const int16_t *pcm, size_t samples, unsigned gain = 100) {
    if (!pcm || !samples || !gain) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t value = pcm[i];
        sum += value < 0 ? -value : value;
    }
    const uint32_t amplitude = static_cast<uint32_t>((sum / samples) * (gain > 100 ? 100 : gain) / 100);
    constexpr uint32_t limits[] = {128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    if (amplitude <= limits[0]) return 0;
    for (unsigned i = 1; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        if (amplitude < limits[i])
            return static_cast<uint8_t>((i - 1) * 14 + (amplitude - limits[i - 1]) * 14 /
                                        (limits[i] - limits[i - 1]));
    }
    return 100;
}

struct Waveform {
    uint8_t bars[kWaveBars]{};
    uint8_t envelope = 0;

    void clear() { *this = Waveform{}; }
    void step(uint8_t level) {
        if (level > 100) level = 100;
        // Fast attack, softer release. Each column is recent measured amplitude,
        // never a fabricated frequency band or a random animation.
        envelope = level >= envelope ? level :
            static_cast<uint8_t>(envelope - (envelope - level + 2) / 3);
        for (int i = 0; i < kWaveBars - 1; ++i) bars[i] = bars[i + 1];
        bars[kWaveBars - 1] = envelope;
    }
};

} // namespace xiaozhi::face
