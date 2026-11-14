#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace netease {
constexpr size_t kMeterBars = 18;
// Same windowed RMS/dB meter as city radio. These are time-domain levels,
// not frequency bands. Silence stays at zero; no synthetic/random motion.
inline void meter_levels(const int16_t *pcm, size_t count, uint8_t *levels) {
    for (size_t bar = 0; bar < kMeterBars; ++bar) {
        const size_t begin = bar * count / kMeterBars, end = (bar + 1) * count / kMeterBars;
        int64_t energy = 0;
        for (size_t i = begin; pcm && i < end; ++i) energy += int64_t(pcm[i]) * pcm[i];
        const float rms = end > begin ? std::sqrt(float(energy) / (end - begin)) : 0;
        const float db = rms > 1 ? 20 * std::log10(rms / 32768.f) : -80;
        levels[bar] = static_cast<uint8_t>(std::clamp(int((db + 58) * 2.25f), 0, 100));
    }
}
}  // namespace netease
