#include "speaker_dsp.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
static double rms_out(double hz, double amp, uint8_t vol, int *peak, uint32_t rate = 44100) {
    SpeakerDsp d; d.configure(rate, vol);
    std::vector<int16_t> s(rate);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(amp * 32767 * std::sin(2 * M_PI * hz * i / rate));
    d.process(s.data(), s.size());
    double e = 0; *peak = 0;
    const size_t half = s.size() / 2;
    for (size_t i = half; i < s.size(); ++i) { e += double(s[i]) * s[i]; *peak = std::max(*peak, std::abs(int(s[i]))); }
    return std::sqrt(e / double(s.size() - half)) / (amp * 32767 / std::sqrt(2.0));
}
int main() {
    int p;
    const double low = rms_out(40, 0.3, 80, &p), mid = rms_out(1000, 0.3, 80, &p), sub = rms_out(120, 0.3, 80, &p);
    std::printf("gain 40Hz=%.3f 120Hz=%.3f 1kHz=%.3f\n", low, sub, mid);
    assert(low < 0.15 && mid > 0.97 && mid < 1.03 && sub > 0.6 && sub < 0.8);
    rms_out(1000, 1.0, 100, &p);
    std::printf("vol100 full-scale peak=%d (ceiling ~13.8k)\n", p);
    assert(p <= 14000);
    rms_out(1000, 0.2, 100, &p);  // quiet passage untouched
    std::printf("vol100 quiet peak=%d\n", p);
    assert(p > 6400 && p < 6700);
    rms_out(1000, 1.0, 80, &p);
    std::printf("vol80 full-scale peak=%d\n", p);
    assert(p <= 30900 && p > 30000);
    // Xiaozhi plays Opus at 16/24 kHz and radio may be 22.05/32 kHz: the same
    // filter and ceiling must hold at every rate the players accept.
    for (uint32_t rate : {16000u, 22050u, 24000u, 32000u, 48000u}) {
        const double pass = rms_out(1000, 0.3, 80, &p, rate), cut = rms_out(40, 0.3, 80, &p, rate);
        assert(pass > 0.97 && pass < 1.03 && cut < 0.15);
        rms_out(1000, 1.0, 100, &p, rate);
        assert(p <= 14000);
        rms_out(1000, 0.2, 90, &p, rate);
        assert(p > 6400 && p < 6700);
    }
    std::puts("SpeakerDsp PASS (16-48 kHz)");
}
