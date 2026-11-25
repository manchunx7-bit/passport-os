/* 头像风格列表必须完全由 ui_theme_t 枚举推导，新增主题时自动出现。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_palette.h"

int main(void)
{
    assert(ui_portrait_style_count() == UI_THEME_COUNT + 1);
    assert(ui_portrait_style_valid(0));
    assert(ui_portrait_style_valid(UI_THEME_COUNT));
    assert(!ui_portrait_style_valid(UI_THEME_COUNT + 1));

    const ui_portrait_style_t fallback = ui_portrait_style_get(ui_portrait_style_count());
    assert(fallback.label[0] == '\0');

    bool seen[UI_THEME_COUNT] = {false};
    for (size_t index = 0; index < ui_portrait_style_count(); ++index) {
        const ui_portrait_style_t style = ui_portrait_style_get(index);
        assert(style.id == index);
        assert(style.label[0] != '\0');
        if (index == 0) {
            assert(!strcmp(style.label, "默认"));
            continue;
        }
        const ui_theme_t theme = (ui_theme_t)(index - 1);
        assert(!seen[theme]);
        seen[theme] = true;
        assert(!strcmp(style.label, ui_theme_name(theme)));
        for (uint8_t level = 0; level < 4; ++level)
            assert(style.palette[level] == ui_theme_portrait_theme_color(theme, level));
    }
    for (unsigned theme = 0; theme < UI_THEME_COUNT; ++theme) assert(seen[theme]);
    printf("Portrait styles: %zu options derived from all %d themes: PASS\n",
           ui_portrait_style_count(), UI_THEME_COUNT);
    return 0;
}
