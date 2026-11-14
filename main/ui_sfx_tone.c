#include "ui_sfx.h"
#include <stddef.h>
#include "ui_sfx_pcm.inc"

static const struct {
    const int16_t *pcm;
    unsigned count;
} kTones[] = {
    {kNav, sizeof(kNav) / sizeof(kNav[0])},
    {kConfirm, sizeof(kConfirm) / sizeof(kConfirm[0])},
    {kBack, sizeof(kBack) / sizeof(kBack[0])},
};

unsigned ui_sfx_samples(ui_sfx_kind_t kind) {
    if (kind == UI_SFX_ALARM) return UI_SFX_RATE * 3 / 2;
    if (kind < UI_SFX_NAV || kind > UI_SFX_BACK) return 0;
    return kTones[kind].count;
}

int16_t ui_sfx_sample(ui_sfx_kind_t kind, unsigned index) {
    if (index >= ui_sfx_samples(kind)) return 0;
    if (kind == UI_SFX_ALARM) {
        // Three loud 2 kHz / 2.67 kHz beep pairs. Both periods divide 16 kHz,
        // so tiny tables give exact sines without a PCM allocation or FPU.
        static const int16_t kLow[8] = {0, 19799, 28000, 19799, 0, -19799, -28000, -19799};
        static const int16_t kHigh[6] = {0, 24249, 24249, 0, -24249, -24249};
        const unsigned in_group = index % (UI_SFX_RATE / 2);
        if (in_group >= UI_SFX_ALARM_BEEP * 2 + UI_SFX_ALARM_GAP) return 0;
        const bool high = in_group >= UI_SFX_ALARM_BEEP;
        if (high && in_group < UI_SFX_ALARM_BEEP + UI_SFX_ALARM_GAP) return 0;
        const unsigned i = high ? in_group - UI_SFX_ALARM_BEEP - UI_SFX_ALARM_GAP : in_group;
        // 4 ms linear ramps keep the square-cornered gate from clicking.
        const unsigned edge = i < UI_SFX_ALARM_BEEP - 1 - i ? i : UI_SFX_ALARM_BEEP - 1 - i;
        const int sample = high ? kHigh[i % 6] : kLow[i % 8];
        return (int16_t)(edge >= 64 ? sample : sample * (int)edge / 64);
    }
    return kTones[kind].pcm[index];
}
