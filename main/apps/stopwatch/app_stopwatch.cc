#include "apps/app_base.h"
#include "stopwatch_core.h"
#include "launcher/timer_resume.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace {
stopwatch_t s_watch{};  // Retained across app switches; reset on device restart.
lv_obj_t *s_screen;
lv_timer_t *s_timer;
lv_obj_t *s_time, *s_fraction, *s_empty, *s_lap_caption;
const lv_font_t *s_time_font;
lv_obj_t *s_status;
lv_obj_t *s_primary;
lv_obj_t *s_laps[STOPWATCH_LAPS];
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
ui_pixel_battery_t s_battery{};
uint64_t s_battery_updated_us;

lv_obj_t *label(const char *text, const lv_font_t *font, uint32_t color,
                int x, int y, int w, int h) {
    lv_obj_t *o = lv_label_create(s_screen);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}
void set_text(lv_obj_t *obj, const char *text) {
    if (std::strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
}
void refresh() {
    if (!s_screen) return;
    const uint64_t now = esp_timer_get_time();
    const uint64_t elapsed = stopwatch_elapsed(&s_watch, now);
    char time[40];
    stopwatch_format(elapsed, time, sizeof(time));
    // Hours get a smaller, bounded font so the value never runs over the edges.
    const lv_font_t *font = elapsed < 360000000000ULL
                               ? &ui_digits_32 : &lv_font_montserrat_20;
    if (font != s_time_font) {
        lv_obj_set_style_text_font(s_time, font, 0);
        s_time_font = font;
    }
    char *dot = std::strrchr(time, '.');
    const char *fraction = dot ? dot : ".00";
    char hundredths[8]; std::snprintf(hundredths, sizeof(hundredths), "%s", fraction);
    if (dot) *dot = '\0';
    lv_point_t extent; lv_text_get_size(&extent, time, font, 0, 0, 216, LV_TEXT_FLAG_NONE);
    int left = (240 - extent.x - 45) / 2;
    lv_obj_set_x(s_time, left); lv_obj_set_width(s_time, extent.x + 1);
    lv_obj_set_pos(s_fraction, left + extent.x + 3, 113);
    lv_obj_set_y(s_time, font == &ui_digits_32 ? 98 : 111);
    set_text(s_time, time); set_text(s_fraction, hundredths);
    set_text(s_status, s_watch.running ? "正在计时" : (elapsed ? "已暂停" : "准备计时"));
    set_text(s_primary, s_watch.running ? "确定暂停 · 上键记段" : (elapsed ? "确定继续 · 下键清零" : "确定开始计时"));
    set_text(s_empty, s_watch.running ? "还没有分段记录" : elapsed ? "退出后保留计时" : "开始后可记录分段");
    lv_obj_set_flag(s_empty, LV_OBJ_FLAG_HIDDEN, s_watch.lap_count > 0);
    lv_obj_set_flag(s_lap_caption, LV_OBJ_FLAG_HIDDEN, s_watch.lap_count == 0);
    ui_pixel_text_color(s_status, s_watch.running ? UI_ACCENT : UI_TEXT_DIM);
    // Elapsed time comes from esp_timer and remains correct while asleep.
    for (uint32_t i = 0; i < STOPWATCH_LAPS; ++i) {
        char row[72];
        if (i < s_watch.lap_count) {
            stopwatch_format(s_watch.laps_us[i], time, sizeof(time));
            std::snprintf(row, sizeof(row), "#%02lu     %s",
                          static_cast<unsigned long>(s_watch.lap_count - i), time);
        } else {
            std::snprintf(row, sizeof(row), "--");
        }
        set_text(s_laps[i], row);
        lv_obj_set_flag(s_laps[i], LV_OBJ_FLAG_HIDDEN, i >= s_watch.lap_count);
    }
    if (!s_battery_updated_us || now - s_battery_updated_us >= 5000000) {
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        const bool wifi_ok = WifiManager::GetInstance().IsConnected();
        int bars = 0;
        uint32_t wifi_col = UI_TEXT_DIM;
        if (wifi_ok) {
            int rssi = WifiManager::GetInstance().GetRssi();
            bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
            wifi_col = UI_TEXT;
        }
        ui_pixel_wifi_bars_set(s_wifi_bars, bars, wifi_col);
        s_battery_updated_us = now;
    }
}
void tick(lv_timer_t *timer) {
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible && s_watch.running ? 50 : 1000);
    if (visible) refresh();
}
void init() {}
void start() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "秒表", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    s_status = label("", &buddy_font_16, UI_ACCENT, 12, 50, 216, 24);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    s_time = label("", &ui_digits_32,
        ui_theme_is_multicolor() ? UI_SECONDARY : UI_TEXT, 12, 98, 164, 46);
    s_fraction = label("", &lv_font_montserrat_20, UI_TEXT_DIM, 172, 113, 52, 28);
    s_empty = label("", &buddy_font_16, UI_TEXT_DIM, 12, 188, 216, 24);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    s_lap_caption = label("最近三段", &buddy_font_16, UI_TEXT_DIM, 12, 156, 216, 22);
    for (int i = 0; i < STOPWATCH_LAPS; ++i)
        s_laps[i] = label("", &lv_font_montserrat_14, UI_TEXT, 12, 186 + 24 * i, 216, 20);
    s_primary = ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
    s_time_font = &ui_digits_32;
    s_battery_updated_us = 0;
    refresh();
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, s_watch.running ? 50 : 1000, nullptr);
    ESP_LOGI("stopwatch", "started; running=%d", s_watch.running);
    bsp_lvgl_unlock();
}
void stop() {
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
    s_battery = {};
    bsp_lvgl_unlock();
}
void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (event != BTN_EVT_CLICK || !bsp_lvgl_lock(500)) return;
    if (s_screen) {
        if (btn == BSP_BTN_OK) stopwatch_toggle(&s_watch, esp_timer_get_time());
        else if (btn == BSP_BTN_UP) stopwatch_lap(&s_watch, esp_timer_get_time());
        else if (btn == BSP_BTN_DOWN) stopwatch_reset(&s_watch);
        refresh();
        if (s_timer) lv_timer_set_period(s_timer, s_watch.running ? 50 : 1000);
        ESP_LOGI("stopwatch", "running=%d elapsed_us=%llu laps=%lu",
                 s_watch.running, (unsigned long long)stopwatch_elapsed(&s_watch, esp_timer_get_time()),
                 (unsigned long)s_watch.lap_count);
    }
    bsp_lvgl_unlock();
}
} // namespace

void stopwatch_export_state(stopwatch_t *out) { *out = s_watch; }
void stopwatch_import_state(const stopwatch_t &state) { s_watch = state; }

extern const passport_app_t g_stopwatch_app = {
    .id = APP_ID_STOPWATCH,
    .name = "秒表",
    .en_name = "STOPWATCH",
    .desc = "计时与最近三段记录\n退出后继续计时",
    .tag = "TIMER",
    .theme_color = 0xDDB36C,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
