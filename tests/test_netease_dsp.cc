#include "apps/netease/netease_dsp.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
using netease::SpeakerDsp;
static double rms_out(double hz, double amp, uint8_t vol, int *peak) {
    SpeakerDsp d; d.configure(44100, vol);
    std::vector<int16_t> s(44100);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(amp * 32767 * std::sin(2 * M_PI * hz * i / 44100));
    d.process(s.data(), s.size());
    double e = 0; *peak = 0;
    for (size_t i = 22050; i < s.size(); ++i) { e += double(s[i]) * s[i]; *peak = std::max(*peak, std::abs(int(s[i]))); }
    return std::sqrt(e / 22050) / (amp * 32767 / std::sqrt(2.0));
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
    std::puts("SpeakerDsp PASS");
}
