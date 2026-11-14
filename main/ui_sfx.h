#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UI_SFX_NAV, UI_SFX_CONFIRM, UI_SFX_BACK, UI_SFX_ALARM } ui_sfx_kind_t;
void ui_sfx_set_enabled(bool enabled);
bool ui_sfx_enabled(void);
// Called under the registry lifecycle lock, only in apps without audio owners.
void ui_sfx_play(ui_sfx_kind_t kind);
// Under lifecycle lock: retry until true (played or master-muted).
// Independent of button sounds; one outstanding timer reminder at a time.
bool ui_sfx_alarm_tick(void);
void ui_sfx_cancel_alarm(void);
bool ui_sfx_prepare_stop(void);
void ui_sfx_log_diagnostics(void);

#define UI_SFX_RATE 16000
// Alarm output level, fixed regardless of master volume (rings even at 0).
// 90 maps to -2 dB on the speaker curve; the -1.4 dBFS beep stays unclipped.
#define UI_SFX_ALARM_VOLUME 90
#define UI_SFX_ALARM_BEEP 1920
#define UI_SFX_ALARM_GAP 640
unsigned ui_sfx_samples(ui_sfx_kind_t kind);
int16_t ui_sfx_sample(ui_sfx_kind_t kind, unsigned index);

#ifdef __cplusplus
}
#endif
