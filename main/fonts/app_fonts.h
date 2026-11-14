#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Type roles are recorded in DESIGN.md (Typography). Every font here is linked;
// do not add one without a role there.

// Body (正文): 5,886-glyph SourceHanSansSC 16px subset, 2bpp (GB2312 level 1,
// common level-2 characters and chat particles; see gen_buddy_font.sh).
// Top-bar app names, body text, lists, key hints and all dynamic text (city
// names, transcripts, replies). 2bpp is a flash budget decision: 4bpp would
// add ~320 KB.
LV_FONT_DECLARE(buddy_font_16);
// Title (标题): shared SourceHanSansSC 20px/4bpp subset for static titles.
// Missing glyphs fall back to the 16px body font, so new title text must add
// its glyphs; regenerate with:
//   wsl -d Ubuntu-24.04 bash /mnt/f/WORK/AI硬件/_diag/gen_ui_font.sh
LV_FONT_DECLARE(ui_font_20);
// Display readout (大读数): Montserrat 32 digits, '.', ':', space and "NET",
// copied from LVGL's built-in font by tools/gen_digit_font.py.
LV_FONT_DECLARE(ui_digits_32);
// Five additional 20px glyphs, with existing ui_font_20 as fallback.
LV_FONT_DECLARE(pomodoro_font_20);
// Hero readout (特大读数): digits and colon only; do not link the full
// Montserrat 48px repertoire.
LV_FONT_DECLARE(pomodoro_digits_48);

#ifdef __cplusplus
}
#endif
