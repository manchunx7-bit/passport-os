// main/ui/ui_pixel.c —— 全机共享的像素风设计系统实现。见 ui_pixel.h。
#include "ui_pixel.h"

#include <stdio.h>
#include <string.h>

static ui_theme_t s_ui_theme = UI_THEME_CLASSIC;

/* Palettes share fonts, images and layout; colour data adds no live UI tree. */
static const uint32_t s_palettes[UI_THEME_COUNT][UI_COLOR_COUNT] = {
    [UI_THEME_CLASSIC] = {
        0x03120B, 0x091E14, 0x122E20, 0x37694B, 0xA8EDBB,
        0x80AF90, 0x8CF5A6, 0xFF9585, 0x03120B, 0x8CF5A6, 0xF2CA6D,
        0x8CF5A6, 0x8CF5A6,
    },
    [UI_THEME_TRAVEL] = {
        0x181B1C, 0x202526, 0x303738, 0x59605D, 0xEEE7D6,
        0xB3B2A5, 0xDDB36C, 0xF09B87, 0x181B1C, 0x9DB89A, 0xDDB36C,
        0xDDB36C, 0xDDB36C,
    },
    [UI_THEME_PLAYFUL] = {
        0xF5F7F6, 0xE8EFEC, 0xD7E9E1, 0x7B8885, 0x252D30,
        0x56635F, 0xA5315B, 0xAE302E, 0xFFFFFF, 0x286B59, 0x805710,
        0x28648B, 0xA64B29,
    },
    [UI_THEME_NEON] = {
        0x0A0B10, 0x17151F, 0x252B32, 0x596373, 0xF4F4F6,
        0xA4AFC0, 0x38E8E1, 0xFF747D, 0x10131A, 0xAAEE6D, 0xFFD45A,
        0xFF78CA, 0xFFB75E,
    },
};

static const uint32_t s_playful_dark[UI_COLOR_COUNT] = {
    0x181B1C, 0x202725, 0x2D3935, 0x53605D, 0xE9EDEB,
    0xAFBDB7, 0xF58FAA, 0xFF968A, 0x181B1C, 0x8DDFC5, 0xFFD37D,
    0x86C9FF, 0xFFAD78,
};

void ui_theme_set(ui_theme_t theme)
{
    s_ui_theme = (unsigned)theme < UI_THEME_COUNT ? theme : UI_THEME_CLASSIC;
}

ui_theme_t ui_theme_get(void) { return s_ui_theme; }
bool ui_theme_is_classic(void) { return s_ui_theme == UI_THEME_CLASSIC; }
bool ui_theme_is_multicolor(void)
{
    return s_ui_theme == UI_THEME_PLAYFUL || s_ui_theme == UI_THEME_NEON;
}
uint32_t ui_theme_color(ui_color_role_t role)
{
    if ((unsigned)role >= UI_COLOR_COUNT) role = UI_COLOR_TEXT;
    return s_palettes[s_ui_theme][role];
}
uint32_t ui_theme_color_on_dark(ui_color_role_t role)
{
    if ((unsigned)role >= UI_COLOR_COUNT) role = UI_COLOR_TEXT;
    return s_ui_theme == UI_THEME_PLAYFUL ? s_playful_dark[role] : ui_theme_color(role);
}
uint32_t ui_theme_portrait_color(uint8_t level)
{
    const uint32_t bg = ui_theme_color_on_dark(UI_COLOR_BG);
    const uint32_t fg = s_ui_theme == UI_THEME_PLAYFUL ? 0xF1EEE6 : ui_theme_color_on_dark(UI_COLOR_TEXT);
    if (level > 3) level = 3;
    uint32_t color = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const unsigned a = (bg >> shift) & 255, b = (fg >> shift) & 255;
        color |= ((a * (3-level) + b * level) / 3) << shift;
    }
    return color;
}
uint32_t ui_theme_portrait_subject_color(uint8_t level)
{
    return ui_theme_portrait_theme_color(s_ui_theme, level);
}

static void start_blink(lv_obj_t *eye);

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *ui_pixel_label_ls(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color,
                            int letter_space)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, letter_space, 0);
    return label;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color)
{
    return ui_pixel_label_ls(parent, text, font, color, UI_LS_BODY);
}

void ui_pixel_label_set_text(lv_obj_t *label, const char *text)
{
    if (label && text && strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

void ui_pixel_bg_color(lv_obj_t *obj, uint32_t color)
{
    if (obj && !lv_color_eq(lv_obj_get_style_bg_color(obj, 0), lv_color_hex(color)))
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
}

void ui_pixel_text_color(lv_obj_t *obj, uint32_t color)
{
    if (obj && !lv_color_eq(lv_obj_get_style_text_color(obj, 0), lv_color_hex(color)))
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

static void focus_opa(void *obj, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, value, 0);
}

void ui_pixel_focus_feedback(lv_obj_t *obj)
{
    if (!obj) return;
    lv_anim_delete(obj, focus_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, focus_opa);
    lv_anim_set_values(&a, LV_OPA_70, LV_OPA_COVER);
    lv_anim_set_duration(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

lv_obj_t *ui_pixel_footer(lv_obj_t *parent, const char *action, const char *navigation,
                          const lv_font_t *font)
{
    // Key hints recede behind content: dim action, navigation one step quieter
    // (80% keeps >= 3.8:1 even on the light theme).
    block(parent, 12, 272, 216, 1, UI_BORDER);
    lv_obj_t *primary = ui_pixel_label(parent, action, font, UI_TEXT_DIM);
    lv_obj_t *secondary = ui_pixel_label(parent, navigation, font, UI_TEXT_DIM);
    lv_obj_set_style_text_opa(secondary, LV_OPA_80, 0);
    lv_obj_t *lines[] = {primary, secondary};
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_pos(lines[i], 12, 276 + i * 22);
        lv_obj_set_size(lines[i], 216, 20);
        lv_obj_set_style_text_align(lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lines[i], LV_LABEL_LONG_MODE_DOTS);
    }
    return primary;
}

// Preserve the clock-theme API without retaining pointers to deleted screens.
static bool s_night;
bool ui_pixel_theme_is_night(void) { return s_night; }
bool ui_pixel_set_hour_theme(int hour)
{
    const bool night = hour >= 0 && hour < 24 && (hour >= 19 || hour < 6);
    if (night == s_night) return false;
    s_night = night;
    return true;
}
void ui_pixel_background(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}
bool ui_pixel_theme_apply(lv_obj_t *scr)
{
    if (!scr) return false;
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    return true;
}

lv_obj_t *ui_pixel_screen_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(scr);

    ui_pixel_title_plate(scr, 10, title, &lv_font_montserrat_20);
    return scr;
}

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color)
{
    lv_obj_t *panel = block(parent, x, y, w, h, color);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(panel, ui_theme_is_classic() ? 1 : 0, 0);
    lv_obj_set_style_pad_all(panel, 7, 0);
    return panel;
}

lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 38, 48);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);

    /* 原创“小电视机器人”：天线、发光屏幕脸、橙色围巾与履带脚。 */
    block(m, 18, 0, 3, 6, UI_INK);
    block(m, 16, 0, 7, 3, UI_ORANGE);
    block(m, 3, 6, 32, 24, UI_INK);
    block(m, 0, 12, 5, 10, 0x7557D9);
    block(m, 33, 12, 5, 10, 0x7557D9);
    block(m, 7, 10, 24, 16, 0xB9F3FF);
    lv_obj_t *left_eye = block(m, 11, 14, 4, 6, 0x294B7A);
    lv_obj_t *right_eye = block(m, 23, 14, 4, 6, 0x294B7A);
    block(m, 16, 22, 7, 2, 0x7557D9);
    block(m, 10, 29, 18, 4, UI_ORANGE);
    block(m, 8, 33, 22, 11, 0x7557D9);
    block(m, 3, 35, 5, 7, 0xB9F3FF);
    block(m, 30, 35, 5, 7, 0xB9F3FF);
    block(m, 8, 44, 9, 4, UI_INK);
    block(m, 21, 44, 9, 4, UI_INK);
    start_blink(left_eye);
    start_blink(right_eye);
    return m;
}

static void jump_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void blink_eye(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void start_blink(lv_obj_t *eye)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, eye);
    lv_anim_set_exec_cb(&anim, blink_eye);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_playback_duration(&anim, 70);
    lv_anim_set_repeat_delay(&anim, 1700);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

void ui_pixel_mascot_jump(lv_obj_t *mascot)
{
    if (!mascot) return;
    int y = lv_obj_get_y(mascot);
    lv_anim_delete(mascot, jump_y);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, mascot);
    lv_anim_set_exec_cb(&anim, jump_y);
    lv_anim_set_values(&anim, y, y - 5);
    lv_anim_set_duration(&anim, 110);
    lv_anim_set_playback_duration(&anim, 140);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled)
{
    uint32_t color = selected && enabled ? UI_SURFACE_ALT : UI_SURFACE;
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(panel,
        lv_color_hex(selected && enabled ? UI_AMBER : UI_BORDER), 0);
}

// ==================== 2026-09-13 新增共享构件 ====================

lv_obj_t *ui_pixel_title_plate(lv_obj_t *parent, int y, const char *text,
                               const lv_font_t *font)
{
    lv_obj_t *plate = block(parent, 0, y, 216, 36,
                            ui_theme_is_classic() ? UI_SURFACE : UI_BG);
    lv_obj_set_style_border_width(plate, 1, 0);
    lv_obj_set_style_border_side(plate, ui_theme_is_classic() ? LV_BORDER_SIDE_FULL
                                                               : LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(plate, lv_color_hex(UI_BORDER), 0);
    lv_obj_t *t = ui_pixel_label_ls(plate, text, font, UI_TEXT, UI_LS_TITLE);
    const int w = 216;
    const int h = (int)lv_font_get_line_height(font) + 12;
    lv_obj_set_size(plate, w, h);
    lv_obj_set_pos(plate, (240 - w) / 2, y);
    lv_obj_set_width(t, w - 8);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, -2);
    return plate;
}

lv_obj_t *ui_pixel_hint_pill(lv_obj_t *parent, const char *text,
                             const lv_font_t *font)
{
    // A single label can be updated safely; no stale duplicate shadow text.
    lv_obj_t *t = ui_pixel_label(parent, text, font, UI_TEXT_DIM);
    lv_obj_set_width(t, 224);
    lv_obj_set_style_text_letter_space(t, 0, 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -6);
    return t;
}

lv_obj_t *ui_pixel_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = block(parent, 0, 0, 240, 26, UI_SURFACE);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_BORDER), 0);
    if (s_ui_theme == UI_THEME_NEON) {
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BG), 0);
    }
    return bar;
}

void ui_pixel_status_text_align(lv_obj_t *label, int x, int width,
                                lv_text_align_t align)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int height = lv_font_get_line_height(font);
    lv_obj_set_pos(label, x, (25 - height) / 2);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, align, 0);
}

ui_pixel_battery_t ui_pixel_battery_create(lv_obj_t *parent, int x, int y,
                                           uint32_t frame_color,
                                           const lv_font_t *font)
{
    ui_pixel_battery_t b = { NULL, NULL };
    lv_obj_t *frame = block(parent, x, y, 24, 12, UI_INK);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(frame_color), 0);
    lv_obj_set_style_radius(frame, 0, 0);
    block(parent, x + 24, y + 3, 3, 6, frame_color);      // 右缘触点

    b.fill = block(frame, 2, 2, 2, 6, frame_color);
    lv_obj_set_style_radius(b.fill, 0, 0);

    // Reserve the widest percentage as a single status-bar line.
    b.label = ui_pixel_label(parent, "--", font, frame_color);
    const int height = lv_font_get_line_height(font);
    lv_obj_set_pos(b.label, x - 46, y + (12 - height) / 2);
    lv_obj_set_size(b.label, 42, height);
    lv_label_set_long_mode(b.label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(b.label, LV_TEXT_ALIGN_RIGHT, 0);
    return b;
}

static void battery_set(ui_pixel_battery_t *batt, int soc, bool dark)
{
    if (!batt || !batt->fill || !batt->label) return;
    char text[8];
    ui_color_role_t role;
    if (soc >= 0 && soc <= 100) {
        // 满电绿、中段黄、低电红 —— 与整机"状态色"语义一致。
        role = soc <= 15 ? UI_COLOR_DANGER
                        : (soc <= 35 ? UI_COLOR_WARNING : UI_COLOR_SUCCESS);
        snprintf(text, sizeof(text), "%d%%", soc);
        lv_obj_set_width(batt->fill, 1 + (17 * soc) / 100);
    } else {
        role = UI_COLOR_TEXT_DIM;
        snprintf(text, sizeof(text), "--");
        lv_obj_set_width(batt->fill, 1);
    }
    lv_obj_set_flag(batt->fill, LV_OBJ_FLAG_HIDDEN, soc <= 0 || soc > 100);
    ui_pixel_bg_color(batt->fill, dark ? ui_theme_color_on_dark(role) : ui_theme_color(role));
    ui_pixel_label_set_text(batt->label, text);
}

void ui_pixel_battery_set(ui_pixel_battery_t *batt, int soc)
{
    battery_set(batt, soc, false);
}

void ui_pixel_battery_set_on_dark(ui_pixel_battery_t *batt, int soc)
{
    battery_set(batt, soc, true);
}

void ui_pixel_wifi_bars_create(lv_obj_t *parent, int x, int y, lv_obj_t *bars[3])
{
    // y 为最低一条的底边;三条逐级升高,占位 16×13。
    for (int i = 0; i < 3; ++i) {
        const int h = 5 + i * 4;
        bars[i] = block(parent, x + i * 6, y - h, 4, h, UI_TEXT_DIM);
        lv_obj_set_style_radius(bars[i], 0, 0);
    }
}

void ui_pixel_wifi_bars_set(lv_obj_t *bars[3], int level, uint32_t color)
{
    for (int i = 0; i < 3; ++i) {
        if (!bars[i]) continue;
        ui_pixel_bg_color(bars[i], color);
        const lv_opa_t opa = i < level ? LV_OPA_COVER : LV_OPA_30;
        if (lv_obj_get_style_bg_opa(bars[i], 0) != opa)
            lv_obj_set_style_bg_opa(bars[i], opa, 0);
    }
}
