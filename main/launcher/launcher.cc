// Application drawer: shared phosphor palette and pixel icons.
#include "launcher/launcher.h"
#include "apps/voice/time_sync.h"
#include "launcher/app_registry.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "ui/pixel_scene.h"
#include "app_fonts.h"
#include "esp_log.h"
#include "lvgl.h"

#include <cstdio>
#include <initializer_list>

static const char *TAG = "launcher";

namespace {

constexpr int kIconSize = 88;
constexpr int kTitleY = 110;

// 旅行仪器配色:与首页(app_home.cc)完全同源,风格统一。
#define kBg (UI_BG)          // 屏底炭黑
#define kCardBg (UI_SURFACE)      // 卡片底
#define kPhos (UI_ACCENT)        // 琥珀:图标/标题/选中态
#define kPhosDim (UI_TEXT_DIM)     // 灰米色:副标题/未选中
#define kPhosBorder (UI_BORDER)  // 卡片/底板描边
#define kStatus (ui_theme_is_multicolor() ? UI_TEXT : kPhos)

lv_obj_t *s_screen;
lv_obj_t *s_card;
lv_obj_t *s_previous, *s_next;
lv_obj_t *s_app_title;
lv_obj_t *s_clock;
lv_obj_t *s_page_number;
lv_obj_t *s_icon;
passport_app_id_t s_icon_id = APP_ID_COUNT;
uint32_t s_icon_color = 0;
lv_obj_t *s_wifi_bars[3];
ui_pixel_battery_t s_battery;
size_t s_selected_index;

void draw_catalogue_stage(lv_event_t *event) {
    pixel_scene::Ink ink(event);
    if (ui_theme_is_studio()) {
        ink.rbox(0, 0, 216, 146, UI_SURFACE, 10, UI_BORDER, 1);
        ink.rbox(14, 12, 24, 4, s_icon_color ? s_icon_color : UI_ACCENT, 2);
        ink.rbox(74, 28, 68, 68, UI_SURFACE_ALT, 10);
        return;
    }
    // A single ticket: notched corners and one accent tab. The page number in
    // the corner is the only position cue, so no brackets or pager dots.
    ink.box(0, 0, 216, 146, UI_SURFACE);
    for (int x : {0, 210}) {
        ink.box(x, 0, 6, 6, UI_BG); ink.box(x, 140, 6, 6, UI_BG);
    }
    ink.box(14, 12, 14, 2, UI_ACCENT);
}

// Up/down neighbours read as a wheel: a 7x4 pixel chevron before the name.
void draw_neighbour_chevron(lv_event_t *event) {
    auto *label = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const char *text = lv_label_get_text(label);
    if (!text || !text[0]) return;
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int w = lv_obj_get_width(label);
    const int x = (w - size.x) / 2 - 14;
    const bool up = lv_event_get_user_data(event) != nullptr;
    pixel_scene::Ink ink(event);
    for (int row = 0; row < 4; ++row) {
        const int half = up ? row : 3 - row;
        ink.box(x + 3 - half, 8 + row, 1 + half * 2, 1, UI_BORDER);
    }
}

const char *app_chinese_name(passport_app_id_t id) {
    // 2026-09-13 修复(D1):名称原本在 launcher 里硬编码一份、在 passport_app_t
    // 里又各写一份,两边悄悄跑偏(注册表"城市收音机" vs 抽屉"收音机",
    // "随身 AI 语音" vs "AI语音")。改成**以注册表为唯一来源**,只有没注册进
    // 注册表的内置页(配网/设置)才走兜底表。
    const passport_app_t *app = app_registry_get_by_id(id);
    if (app != nullptr && app->name != nullptr && app->name[0] != '\0') {
        return app->name;
    }
    switch (id) {
        case APP_ID_WIFI: return "无线网络";
        case APP_ID_SETTINGS: return "设置";
        default: return "";
    }
}

// Shared neon accents stay outside every application's 88x88 icon geometry.
void draw_icon_accents(lv_event_t *event) {
    auto *icon = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_area_t origin;
    lv_obj_get_coords(icon, &origin);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_t paint;
    lv_draw_rect_dsc_init(&paint);
    paint.bg_opa = LV_OPA_COVER;
    if (ui_theme_get() == UI_THEME_NEON) {
        const lv_area_t traces[] = {
            {origin.x1 + 2, origin.y1 + 12, origin.x1 + 3, origin.y1 + 31},
            {origin.x1 + 2, origin.y1 + 7, origin.x1 + 15, origin.y1 + 8},
            {origin.x2 - 18, origin.y2 - 5, origin.x2 - 2, origin.y2 - 4},
            {origin.x2 - 5, origin.y2 - 19, origin.x2 - 4, origin.y2 - 9},
        };
        for (int i = 0; i < 4; ++i) {
            paint.bg_color = lv_color_hex(i < 2 ? UI_ACCENT : UI_SECONDARY);
            lv_draw_rect(layer, &paint, &traces[i]);
        }
    }
}

// One line-icon vocabulary: a 17x17 grid of 3px cells drawn in the app color,
// strokes one cell wide, no fills or second tone. Each glyph is centered on
// the grid, so new apps add one table entry instead of hand-placed strokes.
constexpr int kGlyphCells = 17;
constexpr int kGlyphCell = 3;
constexpr int kGlyphOrigin = (kIconSize - kGlyphCells * kGlyphCell) / 2;
struct IconGlyph {
    passport_app_id_t id;
    const char *rows[kGlyphCells];
};
constexpr IconGlyph kIconGlyphs[] = {
    {APP_ID_XIAOZHI, {  // antenna robot
        ".................",
        ".......###.......",
        "........#........",
        "..#############..",
        "..#...........#..",
        "..#...........#..",
        "###...#...#...###",
        "###...#...#...###",
        "###...........###",
        "..#...........#..",
        "..#....###....#..",
        "..#...........#..",
        "..#############..",
        "........#........",
        ".....#######.....",
        ".................",
        ".................",
    }},
    {APP_ID_RADIO, {  // radio with antenna
        ".................",
        ".........##......",
        ".......##........",
        ".....##..........",
        "...##............",
        "#################",
        "#...............#",
        "#...............#",
        "#.######..#####.#",
        "#.........#...#.#",
        "#.######..#.#.#.#",
        "#.........#...#.#",
        "#.######..#####.#",
        "#...............#",
        "#################",
        ".................",
        ".................",
    }},
    {APP_ID_PODCAST, {  // broadcast waves and stand
        ".................",
        ".....#######.....",
        "...##.......##...",
        "..#...........#..",
        ".#....#####....#.",
        ".#...#.....#...#.",
        ".#..#.......#..#.",
        ".#..#..###..#..#.",
        ".#..#..#.#..#..#.",
        ".#..#..###..#..#.",
        ".#...#.....#...#.",
        "..#.....#.....#..",
        "...#....#....#...",
        "........#........",
        "........#........",
        ".....#######.....",
        ".................",
    }},
    {APP_ID_AUDIOBOOK, {  // open book
        ".................",
        ".................",
        "..######.######..",
        ".#......#......#.",
        ".#......#......#.",
        ".#..###.#.###..#.",
        ".#......#......#.",
        ".#..###.#.###..#.",
        ".#......#......#.",
        ".#..###.#.###..#.",
        ".#......#......#.",
        ".#......#......#.",
        ".#......#......#.",
        "..######.######..",
        "........#........",
        ".................",
        ".................",
    }},
    {APP_ID_NETEASE, {  // beamed notes
        ".................",
        ".................",
        ".....###########.",
        ".....###########.",
        ".....#.........#.",
        ".....#.........#.",
        ".....#.........#.",
        ".....#.........#.",
        ".....#.........#.",
        ".....#.........#.",
        ".....#.........#.",
        "..####......####.",
        ".#...#.....#...#.",
        ".#...#.....#...#.",
        "..###.......###..",
        ".................",
        ".................",
    }},
    {APP_ID_VOICE, {  // microphone
        ".................",
        ".................",
        "......#####......",
        ".....#.....#.....",
        ".....#.....#.....",
        ".....#.....#.....",
        ".....#.....#.....",
        "..#..#.....#..#..",
        "..#..#.....#..#..",
        "..#...#####...#..",
        "...#.........#...",
        "....#########....",
        "........#........",
        "........#........",
        ".....#######.....",
        ".................",
        ".................",
    }},
    {APP_ID_MUSE, {  // chat bubble with sparkle
        ".................",
        "..............#..",
        ".............###.",
        "..............#..",
        ".###########.....",
        "#...........#....",
        "#...........#....",
        "#...........#....",
        "#..#..#..#..#....",
        "#...........#....",
        "#...........#....",
        "#...........#....",
        ".#.#########.....",
        ".##..............",
        ".#...............",
        ".................",
        ".................",
    }},
    {APP_ID_POMODORO, {  // tomato clock
        "........#........",
        "......##.##......",
        "......#####......",
        "....##.....##....",
        "...#.........#...",
        "..#.....#.....#..",
        "..#.....#.....#..",
        ".#......#......#.",
        ".#......#......#.",
        ".#......####...#.",
        ".#.............#.",
        ".#.............#.",
        "..#...........#..",
        "..#...........#..",
        "...#.........#...",
        "....##.....##....",
        "......#####......",
    }},
    {APP_ID_STOPWATCH, {  // stopwatch
        ".......###.......",
        "........#.....#..",
        "......#####..#...",
        "....##.....##....",
        "...#.........#...",
        "..#...........#..",
        "..#........#..#..",
        ".#........#....#.",
        ".#.......#.....#.",
        ".#......#......#.",
        ".#.............#.",
        ".#.............#.",
        "..#...........#..",
        "..#...........#..",
        "...#.........#...",
        "....##.....##....",
        "......#####......",
    }},
    {APP_ID_REMINDERS, {  // bell
        ".................",
        ".................",
        "........#........",
        "......#####......",
        ".....#.....#.....",
        "....#.......#....",
        "....#.......#....",
        "....#.......#....",
        "....#.......#....",
        "...#.........#...",
        "...#.........#...",
        "..#...........#..",
        ".###############.",
        ".................",
        ".......###.......",
        ".................",
        ".................",
    }},
    {APP_ID_LISTS, {  // list page
        ".................",
        ".................",
        "..#############..",
        "..#...........#..",
        "..#...........#..",
        "..#..#..#####.#..",
        "..#...........#..",
        "..#...........#..",
        "..#..#..#####.#..",
        "..#...........#..",
        "..#...........#..",
        "..#..#..###...#..",
        "..#...........#..",
        "..#...........#..",
        "..#############..",
        ".................",
        ".................",
    }},
    {APP_ID_TETRIS, {  // T tetromino
        ".................",
        ".................",
        ".................",
        "################.",
        "#....#....#....#.",
        "#....#....#....#.",
        "#....#....#....#.",
        "#....#....#....#.",
        "################.",
        ".....#....#......",
        ".....#....#......",
        ".....#....#......",
        ".....#....#......",
        ".....######......",
        ".................",
        ".................",
        ".................",
    }},
    {APP_ID_GAME, {  // paw print
        ".................",
        ".................",
        "....###...###....",
        "...#...#.#...#...",
        "...#...#.#...#...",
        "....###...###....",
        ".###.........###.",
        "#...#.......#...#",
        "#...#..###..#...#",
        ".###..#...#..###.",
        ".....#.....#.....",
        "....#.......#....",
        "....#.......#....",
        "....#.......#....",
        ".....###.###.....",
        ".................",
        ".................",
    }},
    {APP_ID_SOUND_JUMP, {  // Claude character
        ".................",
        ".................",
        "..#############..",
        "..#...........#..",
        "..#...........#..",
        "..#..#.....#..#..",
        "###..#.....#..###",
        "#.#...........#.#",
        "###...........###",
        "..#...........#..",
        "..#...........#..",
        "..#############..",
        "...#.#.....#.#...",
        "...#.#.....#.#...",
        ".................",
        ".................",
        ".................",
    }},
    {APP_ID_SETTINGS, {  // sliders
        ".................",
        ".................",
        "....###..........",
        "#####.###########",
        "....###..........",
        ".................",
        ".................",
        "..........###....",
        "###########.#####",
        "..........###....",
        ".................",
        ".................",
        "..###............",
        "###.#############",
        "..###............",
        ".................",
        ".................",
    }},
};

// The icon is drawn into its existing object; no per-part heap allocations.
void draw_app_icon_event(lv_event_t *event) {
    const IconGlyph *glyph = nullptr;
    for (const IconGlyph &g : kIconGlyphs)
        if (g.id == s_icon_id) glyph = &g;
    if (!glyph) return;
    pixel_scene::Ink ink(event);
    for (int y = 0; y < kGlyphCells; ++y) {
        const char *row = glyph->rows[y];
        for (int x = 0; x < kGlyphCells;) {
            if (row[x] != '#') { ++x; continue; }
            const int start = x;
            while (x < kGlyphCells && row[x] == '#') ++x;
            ink.box(kGlyphOrigin + start * kGlyphCell, kGlyphOrigin + y * kGlyphCell,
                    (x - start) * kGlyphCell, kGlyphCell, s_icon_color);
        }
    }
}

void draw_app_icon(passport_app_id_t id, uint32_t color) {
    s_icon_id = id;
    s_icon_color = color;
    lv_obj_invalidate(s_icon);
}

void update_card_content() {
    const passport_app_t *app = app_registry_get_by_index(s_selected_index);
    if (!app) return;
    lv_obj_set_style_text_font(s_app_title, &ui_font_20, 0);
    ui_pixel_text_color(s_app_title, UI_TEXT);
    lv_label_set_text(s_app_title, app_chinese_name(app->id));
    lv_label_set_text_fmt(s_page_number, "%02u / %02u", (unsigned)s_selected_index + 1,
                          (unsigned)app_registry_get_count());
    uint32_t color = kPhos;
    if (ui_theme_is_studio()) {
        switch (app->id) {
            case APP_ID_XIAOZHI: case APP_ID_MUSE: color = UI_ACCENT; break;
            case APP_ID_RADIO: case APP_ID_VOICE: case APP_ID_TETRIS: color = UI_SECONDARY; break;
            case APP_ID_AUDIOBOOK: case APP_ID_GAME: color = UI_TERTIARY; break;
            case APP_ID_PODCAST: case APP_ID_REMINDERS: color = UI_WARNING; break;
            case APP_ID_NETEASE: case APP_ID_POMODORO: color = UI_DANGER; break;
            case APP_ID_STOPWATCH: case APP_ID_LISTS: case APP_ID_SOUND_JUMP: color = UI_SUCCESS; break;
            default: color = UI_TEXT; break;
        }
    } else if (ui_theme_is_multicolor()) {
        switch (app->id) {
            case APP_ID_RADIO: case APP_ID_GAME: case APP_ID_POMODORO: color = UI_TERTIARY; break;
            case APP_ID_VOICE: case APP_ID_TETRIS: color = UI_SECONDARY; break;
            case APP_ID_XIAOZHI: case APP_ID_STOPWATCH: case APP_ID_LISTS: color = UI_SUCCESS; break;
            case APP_ID_REMINDERS: case APP_ID_AUDIOBOOK: color = UI_WARNING; break;
            case APP_ID_NETEASE: color = UI_DANGER; break;
            default: break;
        }
    }
    draw_app_icon(app->id, color);
    lv_obj_invalidate(s_card);

    const size_t count = app_registry_get_count();
    const auto *previous = app_registry_get_by_index((s_selected_index + count - 1) % count);
    const auto *next = app_registry_get_by_index((s_selected_index + 1) % count);
    ui_pixel_label_set_text(s_previous, previous ? app_chinese_name(previous->id) : "");
    ui_pixel_label_set_text(s_next, next ? app_chinese_name(next->id) : "");
}

// Selection follows physical UP/DOWN: only the icon settles, text stays readable.
// One 140ms animation, reset to the same resting position even on rapid repeats.
void icon_y_anim(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

void card_nudge(int dir) {
    if (!s_icon) return;
    lv_anim_delete(s_icon, icon_y_anim);
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_icon);
    lv_anim_set_exec_cb(&b, icon_y_anim);
    lv_anim_set_values(&b, 18 - 6 * dir, 18);
    lv_anim_set_duration(&b, 140);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);
}

lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

}  // namespace

void launcher_ui_init(void) {
    bsp_lvgl_lock(-1);

    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    // 与首页一致的绿磷光底(不再用天空/云/草地的像素风背景)
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    // ---- 状态栏 (y0..24):与首页同款 —— 炭灰条 + 共享 Wi-Fi 格 + 电池 ----
    ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(s_screen, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    s_battery = ui_pixel_battery_create(s_screen, UI_STATUS_BATTERY_X, 6, kStatus,
                                        &lv_font_montserrat_14);

    lv_obj_t *catalogue = ui_pixel_label(s_screen, "应用", &buddy_font_16, UI_TEXT);
    ui_pixel_status_text_align(catalogue, 42, 40, LV_TEXT_ALIGN_LEFT);
    s_clock = ui_pixel_label(s_screen, "--:--", &lv_font_montserrat_14, UI_TEXT);
    ui_pixel_status_text_align(s_clock, 92, 56, LV_TEXT_ALIGN_CENTER);
    s_previous = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    s_next = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_pos(s_previous, 12, 50); lv_obj_set_size(s_previous, 216, 22);
    lv_obj_set_pos(s_next, 12, 242); lv_obj_set_size(s_next, 216, 22);
    for (lv_obj_t *peek : {s_previous, s_next}) {
        lv_obj_set_style_text_align(peek, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(peek, LV_OPA_80, 0);
        lv_label_set_long_mode(peek, LV_LABEL_LONG_DOT);
    }
    lv_obj_add_event_cb(s_previous, draw_neighbour_chevron, LV_EVENT_DRAW_MAIN_END, s_previous);
    lv_obj_add_event_cb(s_next, draw_neighbour_chevron, LV_EVENT_DRAW_MAIN_END, nullptr);
    s_card = block(s_screen, 12, 84, 216, 146, kCardBg);
    if (ui_theme_is_studio()) lv_obj_set_style_bg_opa(s_card, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_card, draw_catalogue_stage, LV_EVENT_DRAW_MAIN, nullptr);
    s_page_number = ui_pixel_label(s_card, "", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_set_pos(s_page_number, 140, 6);
    lv_obj_set_size(s_page_number, 64, 18);
    lv_obj_set_style_text_align(s_page_number, LV_TEXT_ALIGN_RIGHT, 0);
    s_icon = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, kIconSize, kIconSize);
    lv_obj_set_pos(s_icon, 64, 18);
    lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_icon, draw_icon_accents, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(s_icon, draw_app_icon_event, LV_EVENT_DRAW_MAIN, nullptr);
    s_app_title = ui_pixel_label(s_card, "", &ui_font_20, UI_TEXT);
    lv_obj_set_pos(s_app_title, 0, kTitleY); lv_obj_set_size(s_app_title, 216, 28);
    lv_label_set_long_mode(s_app_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_app_title, LV_TEXT_ALIGN_CENTER, 0);

    ui_pixel_footer(s_screen, "上下选择 · 确定打开", "长按确定返回首页", &buddy_font_16);

    update_card_content();
    launcher_ui_update_status();
    lv_scr_load(s_screen);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "Launcher UI initialized (travel instrument), %u apps", (unsigned)app_registry_get_count());
}

void launcher_ui_show(void) {
    if (!s_screen) {
        launcher_ui_init();
        return;
    }
    bsp_lvgl_lock(-1);
    update_card_content();
    launcher_ui_update_status();
    lv_scr_load(s_screen);
    bsp_lvgl_unlock();
}

void launcher_ui_hide(void) {
    // Launcher 不与重型 App 同时常驻。Radio 的音频驱动和复杂 UI 会把单核
    // C3 的内部堆推到极限，进入 App 前释放主页可避免 LVGL 分配失败崩溃。
    bsp_lvgl_lock(-1);
    if (s_screen) lv_obj_delete(s_screen);
    s_screen = nullptr;
    s_card = nullptr;
    s_previous = s_next = nullptr;
    s_app_title = nullptr;
    s_page_number = nullptr;
    s_icon = nullptr;
    s_battery = {nullptr, nullptr};
    for (lv_obj_t *&bar : s_wifi_bars) bar = nullptr;
    bsp_lvgl_unlock();
}

void launcher_ui_update_status(void) {
    if (!s_screen) return;
    char time[8]; time_sync_format_local(time, sizeof(time)); ui_pixel_label_set_text(s_clock, time);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    const bool connected = WifiManager::GetInstance().IsConnected();
    int bars = 0;
    uint32_t color = kPhosDim;
    if (connected) {
        const int rssi = WifiManager::GetInstance().GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        color = kStatus;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, color);
}

void launcher_ui_on_key(uint8_t btn, app_btn_event_t event) {
    if (event != BTN_EVT_CLICK && event != BTN_EVT_LONG) return;
    const size_t count = app_registry_get_count();
    if (!count) return;

    if (btn == BSP_BTN_UP && event == BTN_EVT_CLICK) {
        // Previous app: a short vertical icon cue, with text kept still.
        s_selected_index = (s_selected_index + count - 1) % count;
        bsp_lvgl_lock(-1);
        update_card_content();
        card_nudge(1);
        bsp_lvgl_unlock();
    } else if (btn == BSP_BTN_DOWN && event == BTN_EVT_CLICK) {
        // Next app: reverse the same short cue.
        s_selected_index = (s_selected_index + 1) % count;
        bsp_lvgl_lock(-1);
        update_card_content();
        card_nudge(-1);
        bsp_lvgl_unlock();
    } else if (btn == BSP_BTN_OK) {
        const passport_app_t *app = app_registry_get_by_index(s_selected_index);
        if (app) {
            ESP_LOGI(TAG, "User launched: %s (ID: %d)", app->name, app->id);
            app_registry_switch_to(app->id);
        }
    }
}
