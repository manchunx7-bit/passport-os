#include "apps/app_base.h"
#include "app_pomodoro.h"
#include "pomodoro_core.h"
#include "launcher/timer_resume.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "ui_sfx.h"
#include <cstdio>

namespace {
constexpr char kTag[] = "pomodoro";
constexpr int kProgressWidth = 192;
constexpr uint64_t kNoticeUs = 10000000ULL;
pomodoro_t s_clock = POMODORO_INITIALIZER;
static_assert(sizeof(s_clock) <= 32, "Background clock must remain tiny");
// Every access, including the system heartbeat, holds the existing LVGL lock.
lv_obj_t *s_screen, *s_phase, *s_time, *s_status, *s_count, *s_primary;
lv_obj_t *s_progress, *s_dots[4], *s_notice;
lv_timer_t *s_timer;
ui_pixel_battery_t s_battery{};
uint64_t s_battery_updated_us, s_notice_until_us;
bool s_reset_confirm;

const char *phase_name(uint8_t phase) {
    if (phase == POMODORO_SHORT_BREAK) return "短休息";
    if (phase == POMODORO_LONG_BREAK) return "长休息";
    return "专注时间";
}

lv_obj_t *label(const char *text, const lv_font_t *font, uint32_t color,
                int x, int y, int w, int h) {
    lv_obj_t *obj = ui_pixel_label(s_screen, text, font, color);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    return obj;
}

lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

void remove_notice() {
    if (s_notice) { lv_obj_delete(s_notice); s_notice = nullptr; }
    s_notice_until_us = 0;
}

void refresh() {
    if (!s_screen) return;
    const uint64_t now = esp_timer_get_time();
    pomodoro_poll(&s_clock, now);
    const bool finished = s_clock.status == POMODORO_FINISHED;
    if (finished) s_reset_confirm = false;
    const uint32_t accent = s_clock.phase == POMODORO_FOCUS ? UI_ACCENT : UI_WARNING;
    ui_pixel_label_set_text(s_phase, finished ?
        (s_clock.phase == POMODORO_FOCUS ? "专注完成" : "休息结束") : phase_name(s_clock.phase));
    ui_pixel_text_color(s_phase, accent);
    char text[80];
    const unsigned seconds = pomodoro_remaining_seconds(&s_clock, now);
    std::snprintf(text, sizeof(text), "%02u:%02u", seconds / 60, seconds % 60);
    ui_pixel_label_set_text(s_time, text);
    const char *status = s_clock.phase == POMODORO_FOCUS ? "给一件事，完整的时间" : "放松一下，再出发";
    const char *action = "确定开始 · 上下切换";
    if (finished) {
        const uint8_t next = pomodoro_next_phase(&s_clock);
        status = next == POMODORO_FOCUS ? "休息好了，再开始" :
                 next == POMODORO_LONG_BREAK ? "接下来长休息 15 分钟" : "接下来休息 5 分钟";
        action = next == POMODORO_FOCUS ? "确定开始专注 · 上下切换" : "确定开始休息 · 上下切换";
    } else if (s_clock.status == POMODORO_RUNNING) {
        status = s_clock.phase == POMODORO_FOCUS ? "专注进行中" : "休息进行中";
        action = "确定暂停 · 下键重置";
    } else if (s_clock.status == POMODORO_PAUSED) {
        status = "已暂停，随时继续";
        action = "确定继续 · 下键重置";
    }
    if (s_reset_confirm) {
        status = "重置当前计时？";
        action = "确定重置 · 上键取消";
    }
    ui_pixel_label_set_text(s_status, status);
    ui_pixel_text_color(s_status, s_reset_confirm ? UI_WARNING : UI_TEXT_DIM);
    ui_pixel_label_set_text(s_primary, action);
    const unsigned shown = s_clock.completed > 999 ? 999 : s_clock.completed;
    std::snprintf(text, sizeof(text), "已完成 %u%s 轮专注", shown, s_clock.completed > 999 ? "+" : "");
    ui_pixel_label_set_text(s_count, text);
    unsigned dots = s_clock.completed % 4;
    if (!dots && s_clock.completed) dots = 4;
    for (unsigned i = 0; i < 4; ++i)
        ui_pixel_bg_color(s_dots[i], i < dots ? UI_ACCENT : UI_SURFACE_ALT);
    const uint64_t total = pomodoro_duration_us(s_clock.phase);
    const uint64_t left = pomodoro_remaining_us(&s_clock, now);
    const int width = static_cast<int>((total - (left < total ? left : total)) * kProgressWidth / total);
    lv_obj_set_flag(s_progress, LV_OBJ_FLAG_HIDDEN, width == 0);
    if (width && lv_obj_get_width(s_progress) != width) lv_obj_set_width(s_progress, width);
    ui_pixel_bg_color(s_progress, accent);
    if (!s_battery_updated_us || now - s_battery_updated_us >= 5000000ULL) {
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        s_battery_updated_us = now;
    }
}

void tick(lv_timer_t *) {
    if (app_registry_screen_is_on()) refresh();
}

void start() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    const uint32_t before = esp_get_free_heap_size();
    remove_notice();
    s_reset_confirm = false;
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = ui_pixel_label(top, "番茄时钟", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    s_phase = label("", &pomodoro_font_20, UI_ACCENT, 12, 49, 216, 28);
    s_time = label("", &pomodoro_digits_48, UI_TEXT, 12, 90, 216, 62);
    s_status = label("", &buddy_font_16, UI_TEXT_DIM, 12, 154, 216, 24);
    lv_obj_t *track = block(s_screen, 24, 189, kProgressWidth, 6, UI_SURFACE_ALT);
    s_progress = block(track, 0, 0, 1, 6, UI_ACCENT);
    for (unsigned i = 0; i < 4; ++i)
        s_dots[i] = block(s_screen, 74 + i * 25, 215, 17, 5, UI_SURFACE_ALT);
    s_count = label("", &buddy_font_16, UI_TEXT_DIM, 12, 233, 216, 24);
    s_primary = ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
    s_battery_updated_us = 0;
    refresh();
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 1000, nullptr);
    if (app_registry_screen_is_on() && s_clock.status == POMODORO_FINISHED)
        s_clock.notice_pending = false;
    ESP_LOGI(kTag, "started; state_bytes=%u ui_heap_delta=%ld free=%u status=%u remaining=%u completed=%u",
        (unsigned)sizeof(s_clock), (long)before - (long)esp_get_free_heap_size(),
        (unsigned)esp_get_free_heap_size(), s_clock.status,
        (unsigned)pomodoro_remaining_seconds(&s_clock, esp_timer_get_time()), (unsigned)s_clock.completed);
    bsp_lvgl_unlock();
}

void stop() {
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_phase = s_time = s_status = s_count = s_primary = s_progress = nullptr;
    for (auto &dot : s_dots) dot = nullptr;
    s_battery = {};
    s_reset_confirm = false;
    ESP_LOGI(kTag, "UI released; status=%u remaining=%u completed=%u", s_clock.status,
        (unsigned)pomodoro_remaining_seconds(&s_clock, esp_timer_get_time()), (unsigned)s_clock.completed);
    bsp_lvgl_unlock();
}

void on_key(uint8_t button, app_btn_event_t event, uint16_t) {
    if (event != BTN_EVT_CLICK || !bsp_lvgl_lock(500)) return;
    if (s_screen) {
        pomodoro_poll(&s_clock, esp_timer_get_time());
        if (s_clock.status == POMODORO_FINISHED) s_reset_confirm = false;
        if (s_reset_confirm) {
            if (button == BSP_BTN_OK) { pomodoro_reset(&s_clock); remove_notice(); }
            if (button == BSP_BTN_OK || button == BSP_BTN_UP) s_reset_confirm = false;
        } else if (button == BSP_BTN_OK) {
            pomodoro_toggle(&s_clock, esp_timer_get_time());
            remove_notice();
        } else if (s_clock.status == POMODORO_READY || s_clock.status == POMODORO_FINISHED) {
            if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
                pomodoro_select(&s_clock, button == BSP_BTN_DOWN ? 1 : -1);
                remove_notice();
            }
        } else if (button == BSP_BTN_DOWN) {
            s_reset_confirm = true;
        }
        refresh();
        if (!s_clock.sound_pending) ui_sfx_cancel_alarm();
        ESP_LOGI(kTag, "key; status=%u phase=%u remaining=%u completed=%u confirm=%d", s_clock.status,
            s_clock.phase, (unsigned)pomodoro_remaining_seconds(&s_clock, esp_timer_get_time()),
            (unsigned)s_clock.completed, s_reset_confirm);
    }
    bsp_lvgl_unlock();
}
} // namespace

void pomodoro_export_state(pomodoro_t *out) { *out = s_clock; }
void pomodoro_import_state(const pomodoro_t &state) { s_clock = state; }

bool pomodoro_control(pomodoro_action_t action, uint8_t phase,
                      PomodoroSnapshot *out, pomodoro_command_result_t *result) {
    if (!bsp_lvgl_lock(50)) return false;
    const uint64_t now = esp_timer_get_time();
    *result = pomodoro_command(&s_clock, action, phase, now);
    if (*result == POMODORO_COMMAND_OK && action != POMODORO_QUERY) {
        s_reset_confirm = false;
        remove_notice();
        if (!s_clock.sound_pending) ui_sfx_cancel_alarm();
        refresh();
    }
    *out = {pomodoro_remaining_seconds(&s_clock, now), s_clock.completed,
            s_clock.phase, s_clock.status};
    ESP_LOGI(kTag, "voice action=%u result=%u status=%u phase=%u remaining=%u completed=%u",
        unsigned(action), unsigned(*result), unsigned(out->status), unsigned(out->phase),
        unsigned(out->remaining_seconds), unsigned(out->completed));
    bsp_lvgl_unlock();
    return true;
}

void pomodoro_service_tick() {
    if (!bsp_lvgl_lock(30)) return;
    const uint64_t now = esp_timer_get_time();
    if (pomodoro_poll(&s_clock, now)) {
        ESP_LOGI(kTag, "interval complete; phase=%u completed=%u", s_clock.phase, (unsigned)s_clock.completed);
    }
    if (s_notice && now >= s_notice_until_us) remove_notice();
    if (s_clock.sound_pending && app_registry_sound_notice()) {
        s_clock.sound_pending = false;
        ESP_LOGI(kTag, "sound notice complete (or master muted)");
    }
    if (s_clock.notice_pending) {
        const bool own_screen = s_screen && app_registry_get_active() == APP_ID_POMODORO;
        // Low-memory audio apps take priority. Latch the completion and retry
        // later rather than allocating into a fragmented or exhausted heap.
        const bool room = own_screen || (esp_get_free_heap_size() >= 12288 &&
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 4096);
        if (room && app_registry_wake_for_notice()) {
            if (own_screen) {
                refresh();
            } else {
                remove_notice();
                const uint32_t before = esp_get_free_heap_size();
                s_notice = lv_label_create(lv_layer_top());
                lv_label_set_text_static(s_notice, s_clock.phase == POMODORO_FOCUS ?
                    "番茄钟 · 专注完成" : "番茄钟 · 休息结束");
                lv_obj_set_style_text_font(s_notice, &buddy_font_16, 0);
                lv_obj_set_style_text_color(s_notice, lv_color_hex(UI_ON_ACCENT), 0);
                lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_bg_color(s_notice, lv_color_hex(UI_ACCENT), 0);
                lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
                lv_obj_set_style_pad_all(s_notice, 6, 0);
                lv_obj_set_pos(s_notice, 8, 30);
                lv_obj_set_size(s_notice, 224, 32);
                lv_label_set_long_mode(s_notice, LV_LABEL_LONG_DOT);
                s_notice_until_us = now + kNoticeUs;
                ESP_LOGI(kTag, "visual notice; heap_delta=%ld foreground=%d", (long)before -
                    (long)esp_get_free_heap_size(), app_registry_get_active());
            }
            s_clock.notice_pending = false;
        }
    }
    bsp_lvgl_unlock();
}

extern const passport_app_t g_pomodoro_app = {
    .id = APP_ID_POMODORO,
    .name = "番茄时钟",
    .en_name = "POMODORO",
    .desc = "专注与休息\n后台继续计时",
    .tag = "OFFLINE",
    .theme_color = 0x8CF5A6,
    .init = nullptr,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
