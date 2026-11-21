#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_THEME_CLASSIC = 0,
    UI_THEME_TRAVEL = 1,
    UI_THEME_PLAYFUL = 2,
    UI_THEME_NEON = 3,
    UI_THEME_COUNT,
} ui_theme_t;

typedef enum {
    UI_COLOR_BG = 0,
    UI_COLOR_SURFACE,
    UI_COLOR_SURFACE_ALT,
    UI_COLOR_BORDER,
    UI_COLOR_TEXT,
    UI_COLOR_TEXT_DIM,
    UI_COLOR_ACCENT,
    UI_COLOR_DANGER,
    UI_COLOR_ON_ACCENT,
    UI_COLOR_SUCCESS,
    UI_COLOR_WARNING,
    UI_COLOR_SECONDARY,
    UI_COLOR_TERTIARY,
    UI_COLOR_COUNT,
} ui_color_role_t;

void ui_theme_set(ui_theme_t theme);
ui_theme_t ui_theme_get(void);
bool ui_theme_is_classic(void);
bool ui_theme_is_multicolor(void);
uint32_t ui_theme_color(ui_color_role_t role);
// Dark artwork scenes keep the same hues with foregrounds suited to a dark ground.
uint32_t ui_theme_color_on_dark(ui_color_role_t role);
uint32_t ui_theme_portrait_color(uint8_t level);
uint32_t ui_theme_portrait_subject_color(uint8_t level);
uint32_t ui_theme_portrait_theme_color(ui_theme_t theme, uint8_t level);
const char *ui_theme_name(ui_theme_t theme);

// 头像风格取值：0 保留照片本色，其余为主题枚举编号 + 1；id 直接写入 PT36 头部。
// 风格列表与主题枚举共用一张表，设置页和手机页面都从这里取选项，不另维护副本。
typedef struct {
    uint8_t id;
    const char *label;
    uint32_t palette[4];  // id 0 不使用，其余为对应主题的色阶
} ui_portrait_style_t;

size_t ui_portrait_style_count(void);
bool ui_portrait_style_valid(uint8_t id);
ui_portrait_style_t ui_portrait_style_get(size_t index);

#ifdef __cplusplus
}
#endif

#define UI_BG          ui_theme_color(UI_COLOR_BG)
#define UI_SURFACE     ui_theme_color(UI_COLOR_SURFACE)
#define UI_SURFACE_ALT ui_theme_color(UI_COLOR_SURFACE_ALT)
#define UI_BORDER      ui_theme_color(UI_COLOR_BORDER)
#define UI_TEXT        ui_theme_color(UI_COLOR_TEXT)
#define UI_TEXT_DIM    ui_theme_color(UI_COLOR_TEXT_DIM)
#define UI_ACCENT      ui_theme_color(UI_COLOR_ACCENT)
#define UI_DANGER      ui_theme_color(UI_COLOR_DANGER)
#define UI_ON_ACCENT   ui_theme_color(UI_COLOR_ON_ACCENT)
#define UI_SUCCESS     ui_theme_color(UI_COLOR_SUCCESS)
#define UI_WARNING     ui_theme_color(UI_COLOR_WARNING)
#define UI_SECONDARY   ui_theme_color(UI_COLOR_SECONDARY)
#define UI_TERTIARY    ui_theme_color(UI_COLOR_TERTIARY)

#define UI_SKY        UI_BG
#define UI_SKY_DARK   (ui_theme_is_multicolor() ? UI_SECONDARY : UI_ACCENT)
#define UI_INK        (ui_theme_get() == UI_THEME_PLAYFUL ? UI_TEXT : UI_ON_ACCENT)
#define UI_PAPER      UI_SURFACE
#define UI_GRASS      UI_SUCCESS
#define UI_GRASS_DARK UI_SUCCESS
#define UI_AMBER      (ui_theme_is_multicolor() ? UI_TERTIARY : UI_ACCENT)
#define UI_YELLOW     (ui_theme_is_multicolor() ? UI_WARNING : UI_ACCENT)
#define UI_ORANGE     (ui_theme_is_multicolor() ? UI_TERTIARY : UI_ACCENT)
#define UI_RED        UI_DANGER
#define UI_MUTED      UI_TEXT_DIM
#define UI_SUB        UI_TEXT_DIM
