// Speaker-friendly output stage for NetEase music (mono PCM16, in place).
//
// Commercial masters peak near 0 dBFS; at master volume 100 the codec curve
// adds +6 dB (bsp_audio_volume.c), which clips into harsh distortion. A tiny
// speaker also breaks up on content below ~100 Hz. So: a 120 Hz high-pass,
// then a peak limiter whose ceiling sits just under the codec's clip point
// for the current volume. Integer-only: the ESP32-C3 has no FPU.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace netease {

class SpeakerDsp {
public:
    void configure(uint32_t sample_rate, uint8_t master_volume) {
        if (sample_rate != rate_) {
            rate_ = sample_rate;
            // RBJ cookbook Butterworth high-pass, Q = 0.7071, fc = 120 Hz.
            const double w0 = 2.0 * 3.14159265358979 * 120.0 / sample_rate;
            const double c = std::cos(w0), alpha = std::sin(w0) / (2.0 * 0.70710678);
            const double a0 = 1.0 + alpha;
            b0_ = q28((1.0 + c) / 2.0 / a0);
            b1_ = q28(-(1.0 + c) / a0);
            b2_ = b0_;
            a1_ = q28(-2.0 * c / a0);
            a2_ = q28((1.0 - alpha) / a0);
            x1_ = x2_ = y1_ = y2_ = 0;
        }
        if (master_volume != volume_) {
            volume_ = master_volume;
            // Codec gain from the volume curve's top segments (80% = -10 dB,
            // 90% = -2 dB, 100% = +6 dB); keep 1 dB below the clip point.
            const double db = master_volume <= 80 ? -10.0
                              : master_volume <= 90 ? -10.0 + (master_volume - 80) * 0.8
                                                    : -2.0 + (master_volume - 90) * 0.8;
            const double headroom = db > -1.0 ? db + 1.0 : 0.0;
            ceiling_ = static_cast<int32_t>(32767.0 * std::pow(10.0, -headroom / 20.0) * 0.94);
        }
    }

    void process(int16_t *samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const int32_t x = samples[i];
            // Poles sit close to z = 1, so integer truncation in the feedback
            // path would be amplified thousands of times. Keep the output
            // state with 12 fractional bits (Q12); products are Q40.
            const int64_t feed = (int64_t(b0_) * x + int64_t(b1_) * x1_ + int64_t(b2_) * x2_) * 4096;
            const int64_t back = int64_t(a1_) * y1_ + int64_t(a2_) * y2_;
            const int32_t yq = static_cast<int32_t>((feed - back) >> 28);
            const int32_t y = (yq + 2048) >> 12;
            x2_ = x1_; x1_ = x;
            y2_ = y1_; y1_ = yq;

            // Instant attack, ~50 ms release (gain in Q15).
            const int32_t magnitude = y < 0 ? -y : y;
            int32_t out = static_cast<int32_t>((int64_t(y) * gain_) >> 15);
            if ((out < 0 ? -out : out) > ceiling_ && magnitude) {
                gain_ = static_cast<int32_t>((int64_t(ceiling_) << 15) / magnitude);
                out = static_cast<int32_t>((int64_t(y) * gain_) >> 15);
            }
            gain_ += (32768 - gain_) >> 11;
            samples[i] = static_cast<int16_t>(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
        }
    }

private:
    static int32_t q28(double v) { return static_cast<int32_t>(std::lround(v * (1 << 28))); }

    uint32_t rate_ = 0;
    uint8_t volume_ = 255;
    int32_t b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    int32_t x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;  // y in Q12
    int32_t gain_ = 32768;
    int32_t ceiling_ = 32767;
};

}  // namespace netease
