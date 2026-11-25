// System settings: shared terminal rows and amber focus.
#include "launcher/settings.h"

#include "launcher/app_registry.h"
#include "launcher/wifi_picker.h"
#include "launcher/network_diagnostics.h"
#include "esp_timer.h"
#include <atomic>
#include "bsp_audio.h"
#include "ui_sfx.h"
#include "bsp_battery.h"
#include "launcher/battery_guard.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "app_fonts.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include "nvs.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr char kTag[] = "settings";
constexpr char kNvsNamespace[] = "system_prefs";

// Volume is adjusted in place: OK enters, UP/DOWN step and stop at the ends,
// OK leaves. Cycling made one press past 100% go silent, easy to miss in a
// noisy street.
constexpr uint8_t kVolumeStep = 10;
constexpr uint8_t kBrightness[] = {20, 40, 60, 80, 100};
constexpr uint16_t kTimeouts[] = {0, 15, 30, 60, 120, 300};
constexpr size_t kRowCount = 9;

uint8_t s_volume = 80;
uint8_t s_brightness = 80;
uint16_t s_timeout = 60;
ui_theme_t s_theme = UI_THEME_CLASSIC;
size_t s_selected = 0;
bool s_volume_editing = false;
lv_obj_t *s_screen;
lv_obj_t *s_rows[kRowCount];
lv_obj_t *s_values[kRowCount];
lv_obj_t *s_names[kRowCount];
lv_obj_t *s_hint, *s_help, *s_confirm;
lv_timer_t *s_battery_timer;
ui_pixel_battery_t s_battery{};
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
bool s_reset_pending = false;
std::atomic<int64_t> s_outdoor_until_us{0};
std::atomic<uint8_t> s_outdoor_restore{80};

template <typename T, size_t N>
T validated(const T (&values)[N], T value, T fallback) {
    for (T candidate : values) {
        if (candidate == value) return value;
    }
    return fallback;
}

template <typename T, size_t N>
T next_value(const T (&values)[N], T current) {
    for (size_t i = 0; i < N; ++i) {
        if (values[i] > current) return values[i];
    }
    return values[0];
}

void save_all() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t result = nvs_set_u8(handle, "volume", s_volume);
    if (result == ESP_OK) result = nvs_set_u8(handle, "brightness", s_brightness);
    if (result == ESP_OK) result = nvs_set_u16(handle, "timeout", s_timeout);
    if (result == ESP_OK) result = nvs_set_u8(handle, "theme", static_cast<uint8_t>(s_theme));
    if (result == ESP_OK) result = nvs_set_u8(handle, "key_sound", ui_sfx_enabled() ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) ESP_LOGW(kTag, "Failed to save settings: %s", esp_err_to_name(result));
}

void apply_settings() {
    s_outdoor_until_us = 0;
    bsp_audio_set_master_volume(s_volume);
    bsp_display_set_master_brightness(battery_brightness(bsp_battery_soc(), s_brightness));
    app_registry_set_screen_timeout(s_timeout);
    ui_theme_set(s_theme);
}

void refresh_ui() {
    if (!s_screen) return;
    for (size_t i = 0; i < kRowCount; ++i) {
        const bool selected = i == s_selected;
        const size_t first = s_selected < 7 ? 0 : kRowCount - 7;
        lv_obj_set_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN, s_reset_pending || i < first || i >= first + 7);
        if (i >= first) lv_obj_set_y(s_rows[i], 38 + static_cast<int>(i - first) * 29);
        const uint32_t focus = s_reset_pending && i == 8 ? UI_DANGER : UI_ACCENT;
        ui_pixel_bg_color(s_rows[i], selected ? focus : UI_BG);
        ui_pixel_text_color(s_names[i], selected ? UI_ON_ACCENT : UI_TEXT);
        ui_pixel_text_color(s_values[i], selected ? UI_ON_ACCENT : UI_TEXT_DIM);
    }

    char value[20];
    std::snprintf(value, sizeof(value), s_volume_editing ? "- %u%% +" : "%u%%",
                  static_cast<unsigned>(s_volume));
    lv_label_set_text(s_values[0], value);
    std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(s_brightness));
    lv_label_set_text(s_values[1], value);
    if (s_timeout == 0) {
        lv_label_set_text(s_values[2], "常亮");
    } else {
        std::snprintf(value, sizeof(value), "%u 秒", static_cast<unsigned>(s_timeout));
        lv_label_set_text(s_values[2], value);
    }
    lv_label_set_text(s_values[3], ui_theme_name(s_theme));
    lv_label_set_text(s_values[4], ui_sfx_enabled() ? "开" : "关");
    lv_label_set_text(s_values[5], "打开");
    lv_label_set_text(s_values[6], "打开");
    lv_label_set_text(s_values[7], "查看");
    lv_label_set_text(s_values[8], s_reset_pending ? "再次确定" : "恢复");
    static const char *hints[kRowCount] = {
        "确定后上下调音量", "长按下键满亮两分钟",
        "确定切换熄屏时间", "确定切换主题",
        "轻触反馈", "手机配网、资料与名片", "设备选网与已存网络", "查看网络、时间与电量", "仅恢复设备设置"
    };
    lv_obj_set_flag(s_confirm, LV_OBJ_FLAG_HIDDEN, !s_reset_pending);
    lv_obj_set_flag(s_help, LV_OBJ_FLAG_HIDDEN, s_reset_pending);
    ui_pixel_label_set_text(s_help, s_volume_editing ? "上键加 · 下键减" : hints[s_selected]);
    ui_pixel_label_set_text(s_hint, s_reset_pending ? "再次确定恢复"
                                    : s_volume_editing ? "确定完成"
                                    : "上下选择 · 确定调整");

    const bool wifi_ok = WifiManager::GetInstance().IsConnected();
    int bars = 0;
    uint32_t wifi_col = UI_TEXT_DIM;
    if (wifi_ok) {
        int rssi = WifiManager::GetInstance().GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        wifi_col = UI_TEXT;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, wifi_col);
}

void draw_neon_focus(lv_event_t *event) {
    auto *row = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (ui_theme_get() != UI_THEME_NEON ||
        !lv_color_eq(lv_obj_get_style_bg_color(row, LV_PART_MAIN), lv_color_hex(UI_ACCENT))) return;
    lv_area_t bounds;
    lv_obj_get_coords(row, &bounds);
    lv_draw_rect_dsc_t paint;
    lv_draw_rect_dsc_init(&paint);
    paint.bg_color = lv_color_hex(UI_BG);
    paint.bg_opa = LV_OPA_COVER;
    auto *layer = lv_event_get_layer(event);
    for (int y = 0; y < 6; ++y) {
        const lv_area_t top = {bounds.x1, bounds.y1 + y, bounds.x1 + 5 - y, bounds.y1 + y};
        const lv_area_t bottom = {bounds.x2 - 5 + y, bounds.y2 - y, bounds.x2, bounds.y2 - y};
        lv_draw_rect(layer, &paint, &top);
        lv_draw_rect(layer, &paint, &bottom);
    }
}

void create_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    lv_obj_t *title = ui_pixel_label(top, "设置", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    static const char *labels[kRowCount] = {
        "音量", "屏幕亮度", "自动熄屏", "外观主题", "按键音效", "手机设置", "无线网络", "网络与时间", "恢复默认"
    };
    for (size_t i = 0; i < kRowCount; ++i) {
        const int y = 38 + static_cast<int>(i) * 29;
        s_rows[i] = lv_obj_create(s_screen);
        lv_obj_set_size(s_rows[i], 216, 27);
        lv_obj_set_pos(s_rows[i], 12, y);
        lv_obj_set_style_radius(s_rows[i], ui_theme_is_studio() ? 6 : 0, 0);
        lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_bg_opa(s_rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_rows[i], 0, 0);
        lv_obj_set_style_border_color(s_rows[i], lv_color_hex(UI_BORDER), 0);
        lv_obj_set_style_pad_hor(s_rows[i], 12, 0);
        lv_obj_set_style_pad_ver(s_rows[i], 0, 0);
        lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        if (ui_theme_get() == UI_THEME_NEON)
            lv_obj_add_event_cb(s_rows[i], draw_neon_focus, LV_EVENT_DRAW_MAIN, nullptr);

        s_names[i] = ui_pixel_label(s_rows[i], labels[i], &buddy_font_16, UI_TEXT);
        lv_obj_align(s_names[i], LV_ALIGN_LEFT_MID, 0, 0);

        s_values[i] = ui_pixel_label(s_rows[i], "", &buddy_font_16, UI_SKY_DARK);
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    s_help = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    // Aligned with the row labels (row x 12 + 12 px inner padding).
    lv_obj_set_pos(s_help, 24, 244); lv_obj_set_size(s_help, 192, 22);
    lv_label_set_long_mode(s_help, LV_LABEL_LONG_DOT);
    s_hint = ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
    s_confirm = lv_obj_create(s_screen); lv_obj_remove_style_all(s_confirm);
    lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_pos(s_confirm, 12, 42);lv_obj_set_size(s_confirm, 216, 220);
    auto *question = ui_pixel_label(s_confirm, "恢复默认设置？", &ui_font_20, UI_DANGER);
    lv_obj_set_pos(question, 0, 0);lv_obj_set_width(question, 216);
    auto *consequence = ui_pixel_label(s_confirm, "音量、亮度设为 80%\n60 秒自动熄屏\n主题恢复经典绿\n按键音效开启\nWi-Fi 与个人资料保留\n上下键取消", &buddy_font_16, UI_TEXT);
    lv_obj_set_pos(consequence, 0, 54);lv_obj_set_width(consequence, 216);
    lv_obj_set_style_text_line_space(consequence, 5, 0);

    refresh_ui();
}

void settings_init() {
    // 启动配置已由 settings_bootstrap 恢复；这里不创建 UI，保持惰性加载。
}

void settings_start() {
    bsp_lvgl_lock(-1);
    s_reset_pending = false;
    s_volume_editing = false;
    if (!s_screen) create_ui();
    refresh_ui();
    lv_scr_load(s_screen);
    if (!s_battery_timer) s_battery_timer = lv_timer_create([](lv_timer_t *) {
        if (s_screen && app_registry_screen_is_on()) ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    }, 5000, nullptr);
    bsp_lvgl_unlock();
}

void settings_stop() {
    wifi_picker_close();
    network_diagnostics_close();
    if (s_volume_editing) {
        s_volume_editing = false;
        save_all();
    }
    bsp_lvgl_lock(-1);
    if (s_battery_timer) {lv_timer_delete(s_battery_timer);s_battery_timer = nullptr;}
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
        for (size_t i = 0; i < kRowCount; ++i) {
            s_rows[i] = nullptr;
            s_names[i] = nullptr;
            s_values[i] = nullptr;
        }
        s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
    }
    bsp_lvgl_unlock();
}

void settings_on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (wifi_picker_active()) {
        wifi_picker_key(btn, event);
        if (!wifi_picker_active()) settings_start();
        return;
    }
    if (network_diagnostics_active()) {
        if (btn == BSP_BTN_OK && event == BTN_EVT_CLICK) { network_diagnostics_close(); settings_start(); }
        return;
    }
    if (s_selected == 1 && btn == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        if (bsp_battery_soc() >= 0 && bsp_battery_soc() < 10) return;
        s_outdoor_restore = s_brightness;
        s_outdoor_until_us = esp_timer_get_time() + 120000000;
        bsp_display_set_master_brightness(100);
        return;
    }
    if (event != BTN_EVT_CLICK) return;
    bool rebuild = false;
    if (s_volume_editing && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        if (btn == BSP_BTN_UP) s_volume = s_volume + kVolumeStep > 100 ? 100 : s_volume + kVolumeStep;
        else s_volume = s_volume < kVolumeStep ? 0 : s_volume - kVolumeStep;
        apply_settings(); // audible at once; saved when editing ends
    } else if (s_volume_editing && btn == BSP_BTN_OK) {
        s_volume_editing = false;
        save_all();
    } else if (btn == BSP_BTN_UP) {
        s_reset_pending = false;
        s_selected = (s_selected + kRowCount - 1) % kRowCount;
    } else if (btn == BSP_BTN_DOWN) {
        s_reset_pending = false;
        s_selected = (s_selected + 1) % kRowCount;
    } else if (btn == BSP_BTN_OK) {
        switch (s_selected) {
            case 0:
                s_volume_editing = true;
                bsp_lvgl_lock(-1);
                refresh_ui();
                bsp_lvgl_unlock();
                return;
            case 1: s_brightness = next_value(kBrightness, s_brightness); break;
            case 2: s_timeout = next_value(kTimeouts, s_timeout); break;
            case 3:
                s_theme = static_cast<ui_theme_t>((static_cast<unsigned>(s_theme) + 1) % UI_THEME_COUNT);
                rebuild = true;
                break;
            case 4:
                ui_sfx_set_enabled(!ui_sfx_enabled());
                break;
            case 5:
                app_registry_switch_to(APP_ID_WIFI);
                return;
            case 6:
                settings_stop();
                if (!wifi_picker_open()) settings_start();
                return;
            case 7:
                settings_stop(); network_diagnostics_open(); return;
            case 8:
                if (!s_reset_pending) {
                    s_reset_pending = true;
                    bsp_lvgl_lock(-1);
                    refresh_ui();
                    bsp_lvgl_unlock();
                    return;
                }
                s_reset_pending = false;
                s_volume = 80;
                s_brightness = 80;
                s_timeout = 60;
                s_theme = UI_THEME_CLASSIC;
                ui_sfx_set_enabled(true);
                rebuild = true;
                break;
        }
        apply_settings();
        save_all();
    } else {
        return;
    }

    bsp_lvgl_lock(-1);
    if (rebuild) {
        // Load the replacement before deleting the currently active screen.
        lv_obj_t *old_screen = s_screen;
        s_screen = nullptr;
        for (size_t i = 0; i < kRowCount; ++i) {
            s_rows[i] = nullptr;
            s_names[i] = nullptr;
            s_values[i] = nullptr;
        }
        s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
        create_ui();
        lv_scr_load(s_screen);
        lv_obj_delete(old_screen);
    } else {
        refresh_ui();
        ui_pixel_focus_feedback(s_rows[s_selected]);
    }
    bsp_lvgl_unlock();
}

}  // namespace

void settings_tick(void) {
    const int64_t until = s_outdoor_until_us.load();
    if (until && esp_timer_get_time() >= until) {
        s_outdoor_until_us = 0;
        bsp_display_set_master_brightness(battery_brightness(bsp_battery_soc(), s_outdoor_restore.load()));
    }
}

void settings_bootstrap(void) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "volume", &s_volume);
        nvs_get_u8(handle, "brightness", &s_brightness);
        nvs_get_u16(handle, "timeout", &s_timeout);
        uint8_t theme = static_cast<uint8_t>(UI_THEME_CLASSIC);
        uint8_t key_sound = 1;
        if (nvs_get_u8(handle, "key_sound", &key_sound) == ESP_OK)
            ui_sfx_set_enabled(key_sound != 0);
        if (nvs_get_u8(handle, "theme", &theme) == ESP_OK && theme < UI_THEME_COUNT) {
            s_theme = static_cast<ui_theme_t>(theme);
        }
        nvs_close(handle);
    }
    if (s_volume > 100) s_volume = 80;
    if (s_brightness < 10 || s_brightness > 100) s_brightness = 80;
    s_timeout = validated(kTimeouts, s_timeout, static_cast<uint16_t>(60));
    ui_theme_set(s_theme);
    apply_settings();
    ESP_LOGI(kTag, "Loaded volume=%u brightness=%u timeout=%u theme=%s",
             static_cast<unsigned>(s_volume), static_cast<unsigned>(s_brightness),
             static_cast<unsigned>(s_timeout), ui_theme_name(s_theme));
}

namespace {
bool save_percent(const char *key, uint8_t percent) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, key, percent);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) ESP_LOGW(kTag, "Failed to save %s: %s", key, esp_err_to_name(result));
    return result == ESP_OK;
}
bool save_u16(const char *key, uint16_t value) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u16(handle, key, value);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) ESP_LOGW(kTag, "Failed to save %s: %s", key, esp_err_to_name(result));
    return result == ESP_OK;
}
}
uint8_t settings_get_volume(void) { return s_volume; }
uint8_t settings_get_brightness(void) { return s_brightness; }
uint16_t settings_get_timeout(void) { return s_timeout; }
ui_theme_t settings_get_theme(void) { return s_theme; }
bool settings_get_key_sound(void) { return ui_sfx_enabled(); }
bool settings_save_volume(uint8_t percent) {
    if (percent > 100 || !save_percent("volume", percent)) return false;
    s_volume = percent;
    bsp_audio_set_master_volume(percent);
    return true;
}
bool settings_save_brightness(uint8_t percent) {
    if (percent < 10 || percent > 100 || !save_percent("brightness", percent)) return false;
    s_brightness = percent;
    s_outdoor_until_us = 0;
    s_outdoor_restore = percent;
    bsp_display_set_master_brightness(battery_brightness(bsp_battery_soc(), percent));
    return true;
}
bool settings_save_theme(ui_theme_t theme) {
    if (theme >= UI_THEME_COUNT || !save_percent("theme", static_cast<uint8_t>(theme))) return false;
    s_theme = theme;
    ui_theme_set(theme);
    return true;
}
bool settings_save_timeout(uint16_t seconds) {
    if (validated(kTimeouts, seconds, static_cast<uint16_t>(0xFFFF)) == 0xFFFF) return false;
    if (!save_u16("timeout", seconds)) return false;
    s_timeout = seconds;
    app_registry_set_screen_timeout(seconds);
    return true;
}
bool settings_save_key_sound(bool enabled) {
    if (!save_percent("key_sound", enabled ? 1 : 0)) return false;
    ui_sfx_set_enabled(enabled);
    return true;
}
void settings_set_volume(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    if (s_volume != percent) settings_save_volume(percent);
}

const passport_app_t g_settings_app = {
    .id = APP_ID_SETTINGS,
    .name = "设置",
    .en_name = "SETTINGS",
    .desc = "音量、亮度与主题\n调整设备使用习惯",
    .tag = "DEVICE",
    .theme_color = 0x181B1C,
    .init = settings_init,
    .start = settings_start,
    .stop = settings_stop,
    .on_key = settings_on_key,
    .prepare_stop = []() { return !wifi_picker_busy(); },
};
