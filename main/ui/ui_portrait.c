// 头像风格与主题色阶：只依赖 ui_theme_t 枚举，供固件和主机测试共用。
#include "ui_palette.h"

static const uint32_t s_portrait_theme_palettes[UI_THEME_COUNT][4] = {
    {0x254D36, 0x4B8060, 0x80B795, 0xA8EDBB},
    {0x484D43, 0x797D69, 0xB6B7A0, 0xEEE7D6},
    {0x374543, 0x637771, 0xADBFB1, 0xF1EEE6},
    {0x343C55, 0x657593, 0xB3C5D8, 0xF4F4F6},
    {0x343740, 0x646978, 0xA8AEB8, 0xF5F3EE},
};

uint32_t ui_theme_portrait_theme_color(ui_theme_t theme, uint8_t level)
{
    if ((unsigned)theme >= UI_THEME_COUNT) theme = UI_THEME_CLASSIC;
    return s_portrait_theme_palettes[theme][level > 3 ? 3 : level];
}

const char *ui_theme_name(ui_theme_t theme)
{
    switch (theme) {
        case UI_THEME_TRAVEL: return "旅行仪器";
        case UI_THEME_PLAYFUL: return "像素白";
        case UI_THEME_NEON: return "霓虹黑";
        case UI_THEME_STUDIO: return "曜石工坊";
        default: return "经典绿";
    }
}

size_t ui_portrait_style_count(void)
{
    return UI_THEME_COUNT + 1;
}

bool ui_portrait_style_valid(uint8_t id)
{
    return id < UI_THEME_COUNT + 1;
}

ui_portrait_style_t ui_portrait_style_get(size_t index)
{
    ui_portrait_style_t style = {.id = 0, .label = "", .palette = {0, 0, 0, 0}};
    if (index >= ui_portrait_style_count()) return style;
    style.id = (uint8_t)index;
    if (index == 0) {
        style.label = "原图";
        return style;
    }
    const ui_theme_t theme = (ui_theme_t)(index - 1);
    style.label = ui_theme_name(theme);
    for (uint8_t level = 0; level < 4; ++level)
        style.palette[level] = ui_theme_portrait_theme_color(theme, level);
    return style;
}
