#include "apps/xiaozhi/xiaozhi_level.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    using namespace xiaozhi::face;
    int16_t pcm[1440]{};
    assert(pcm_level(pcm, 1440) == 0);
    assert(pcm_level(nullptr, 1440) == 0 && pcm_level(pcm, 0) == 0);
    uint8_t previous = 0;
    for (int amplitude : {64, 128, 256, 512, 1024, 4096, 8192, 16384, 32767}) {
        for (int i = 0; i < 1440; ++i) pcm[i] = i & 1 ? -amplitude : amplitude;
        const auto level = pcm_level(pcm, 1440);
        assert(level >= previous && level <= 100);
        previous = level;
        assert(pcm_level(pcm, 1440, 0) == 0);
        assert(pcm_level(pcm, 1440, 25) <= level);
    }
    for (auto &sample : pcm) sample = INT16_MIN;
    assert(pcm_level(pcm, 1440) == 100);
    // Very quiet capture and a lone impulse must not pin a loud meter.
    for (auto &sample : pcm) sample = 100;
    assert(pcm_level(pcm, 1440) == 0);
    for (auto &sample : pcm) sample = 0;
    pcm[0] = INT16_MIN;
    assert(pcm_level(pcm, 1440) == 0);
    Waveform wave;
    wave.step(90);
    assert(wave.bars[kWaveBars - 1] == 90);
    wave.step(0);
    assert(wave.bars[kWaveBars - 2] == 90 && wave.bars[kWaveBars - 1] == 60);
    for (int i = 0; i < 40; ++i) wave.step(0);
    for (auto bar : wave.bars) assert(bar == 0);
    wave.step(255);assert(wave.bars[kWaveBars - 1] == 100);
    wave.clear();for (auto bar : wave.bars) assert(bar == 0);
    std::puts("Xiaozhi level: silence, amplitude, mute, clipping and waveform release PASS");
}
