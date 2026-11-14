#include "radio_ui.h"

#include "radio_display_settings.h"
#include "radio_sleep_timer.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "ui/pixel_scene.h"
#include "ui/ssid_text.h"
#include "apps/home/home_clock.h"
#include "launcher/app_registry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// 2026-09-13 深夜:应用户要求换回"夜间座舱"深色主题(天空蓝+纸面板观感杂乱)。
// 全部颜色走下面常量,换主题只动这里;琥珀强调是收音机的"电台橙"。
#define kBackground (UI_BG)   // 深夜蓝黑底
#define kPanel (UI_SURFACE)        // 面板
#define kPanelSoft (UI_SURFACE_ALT)    // 次级面板(电平条/音量条底)
#define kGrid (UI_BORDER)         // 刻度/描边
#define kText (UI_TEXT)         // 主文字(冷白)
#define kMuted (UI_TEXT_DIM)        // 次级文字
#define kAmber (UI_AMBER)        // 琥珀强调(指针/频率/CH 号)
#define kAmberSoft (UI_SURFACE_ALT)    // 亮琥珀(选中)
#define kGreen (UI_GRASS)
#define kRed (UI_RED)

// Dial geometry. The scale spans 88-108 MHz, the band used by Chinese FM
// broadcasting, so a real frequency maps onto it directly.
constexpr int kDialBandLow = 870;    // Match catalog's 87.0-108.0 MHz band.
constexpr int kDialBandHigh = 1080;  // 108.0 MHz in tenths
constexpr int kDialTrackLeft = 16;
constexpr int kDialTrackRight = 198;
constexpr int kDialNeedleWidth = 3;
constexpr int kDialNeedleHome = (kDialTrackLeft + kDialTrackRight) / 2;
constexpr std::size_t kMeterCount = 18;
constexpr std::size_t kWifiVisibleRows = 6;
constexpr std::size_t kWifiMaxKeys = 35;
constexpr uint16_t kVideoWidth = 240;
constexpr uint16_t kVideoHeight = 320;
constexpr uint16_t kJpegMcuHeight = 16;
constexpr uint8_t kMaxVideoScale = 2;
constexpr std::size_t kJpegWorkSize = 4096;

enum class Page {
    Main,
    Settings,
    Config,
    WifiList,
    WifiKeyboard,
    WifiConnecting,
    EasterEgg,
};

lv_obj_t *s_screen;
lv_timer_t *s_ui_timer;
lv_obj_t *s_main_group;
lv_obj_t *s_station_number;
lv_obj_t *s_station_name;
lv_obj_t *s_station_description;
lv_obj_t *s_play_icon;
lv_obj_t *s_main_hint;
lv_obj_t *s_status;
lv_obj_t *s_location;
lv_obj_t *s_clock;
ui_pixel_battery_t s_battery = {nullptr, nullptr};
lv_obj_t *s_sleep_timer;
lv_obj_t *s_wifi_bars[3];
lv_obj_t *s_meter[kMeterCount];
lv_obj_t *s_volume_value;
lv_obj_t *s_volume_bar;
lv_obj_t *s_settings_rows[8];
lv_obj_t *s_settings_labels[8];
lv_obj_t *s_pair_dot;
lv_obj_t *s_dial_needle;
lv_obj_t *s_dial_glow;
lv_obj_t *s_dial_readout;
lv_obj_t *s_wifi_state;
lv_obj_t *s_wifi_list_hint;
lv_obj_t *s_wifi_row_box[kWifiVisibleRows];
lv_obj_t *s_wifi_row_ssid[kWifiVisibleRows];
lv_obj_t *s_wifi_row_bars[kWifiVisibleRows][3];
lv_obj_t *s_wifi_keyboard_network;
lv_obj_t *s_wifi_password_mask;
lv_obj_t *s_wifi_keyboard_status;
lv_obj_t *s_easter_message;
lv_obj_t *s_wifi_key_box[kWifiMaxKeys];
lv_obj_t *s_wifi_key_label[kWifiMaxKeys];
uint8_t *s_jpeg_work;
uint16_t *s_video_stripe;
bool s_video_color_pending;

struct DirectJpegContext {
    const uint8_t *data;
    std::size_t size;
    std::size_t position;
    uint16_t source_width;
    uint16_t source_height;
    uint8_t scale;
    bool failed;
};

Page s_page = Page::Main;
bool s_network_connected;
uint8_t s_volume = 55;
RadioSettingsPage s_settings_page = RadioSettingsPage::Menu;
std::size_t s_settings_selected;
std::size_t s_settings_row_count;
std::size_t s_station_index;
std::size_t s_station_count = 1;
char s_station_name_text[64] = "清晨音乐";
char s_station_description_text[96] = "轻松音乐 · 网络直播";
char s_network_detail[64] = "等待网络";
char s_location_text[48] = "NATIONAL";
char s_playback_detail[64] = "准备播放";
RadioPlaybackState s_playback_state = RadioPlaybackState::Stopped;
std::atomic<uint32_t> s_level_generation{0};
uint8_t s_levels[kMeterCount] = {};
uint32_t s_last_level_generation;
uint8_t s_animation;
uint32_t s_battery_ticks;
uint32_t s_clock_tick;
uint32_t s_last_sleep_seconds = UINT32_MAX;
uint32_t s_volume_until;
uint16_t s_station_frequency;
int32_t s_needle_x = kDialNeedleHome;

bool drain_video_color_transfers() {
    if (!bsp_display_io()) return true;
    const esp_err_t result =
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    if (result != ESP_OK) {
        ESP_LOGE("radio_ui", "Panel color drain failed: %s",
                 esp_err_to_name(result));
        return false;
    }
    return true;
}

std::size_t direct_jpeg_input(JDEC *decoder, uint8_t *buffer,
                              std::size_t requested) {
    auto *context = static_cast<DirectJpegContext *>(decoder->device);
    if (!context || context->position >= context->size) return 0;
    const std::size_t count =
        std::min(requested, context->size - context->position);
    if (buffer) std::memcpy(buffer, context->data + context->position, count);
    context->position += count;
    return count;
}

int direct_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<DirectJpegContext *>(decoder->device);
    if (!context || !bitmap || !rectangle || context->failed) return 0;

    const uint16_t block_width = rectangle->right - rectangle->left + 1;
    const uint16_t block_height = rectangle->bottom - rectangle->top + 1;
    if (rectangle->right >= context->source_width ||
        rectangle->bottom >= context->source_height) {
        context->failed = true;
        return 0;
    }

    if (rectangle->left == 0 && s_video_color_pending) {
        // The shared stripe is still owned by the previous color transfer.
        // A parameter command drains every queued color transaction before we
        // overwrite it with the next band.
        if (!drain_video_color_transfers()) {
            context->failed = true;
            return 0;
        }
        s_video_color_pending = false;
    }
    uint16_t *target = s_video_stripe;
    if (!target) {
        context->failed = true;
        return 0;
    }

    // TJPGD's JD_FORMAT=0 output is stored as B, G, R bytes (matching
    // LVGL's little-endian RGB888 layout), not conventional R, G, B.
    const auto *bgr = static_cast<const uint8_t *>(bitmap);
    for (uint16_t y = 0; y < block_height; ++y) {
        for (uint16_t x = 0; x < block_width; ++x) {
            const std::size_t input =
                (static_cast<std::size_t>(y) * block_width + x) * 3U;
            const uint16_t color =
                static_cast<uint16_t>(((bgr[input + 2] & 0xf8U) << 8U) |
                                      ((bgr[input + 1] & 0xfcU) << 3U) |
                                      (bgr[input] >> 3U));
            const uint16_t panel_color = __builtin_bswap16(color);
            const uint16_t output_x =
                static_cast<uint16_t>((rectangle->left + x) * context->scale);
            const uint16_t output_y = static_cast<uint16_t>(y * context->scale);
            for (uint8_t repeat_y = 0; repeat_y < context->scale; ++repeat_y) {
                uint16_t *row = target +
                    static_cast<std::size_t>(output_y + repeat_y) * kVideoWidth;
                for (uint8_t repeat_x = 0; repeat_x < context->scale; ++repeat_x) {
                    row[output_x + repeat_x] = panel_color;
                }
            }
        }
    }

    if (rectangle->right + 1U == context->source_width) {
        const uint16_t panel_top = rectangle->top * context->scale;
        const uint16_t panel_bottom =
            static_cast<uint16_t>((rectangle->bottom + 1U) * context->scale);
        if (esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, panel_top,
                                      kVideoWidth, panel_bottom, target) != ESP_OK) {
            context->failed = true;
            return 0;
        }
        s_video_color_pending = true;
    }
    return 1;
}

bool ensure_direct_video_buffers() {
    if (!s_jpeg_work) {
        s_jpeg_work = static_cast<uint8_t *>(std::malloc(kJpegWorkSize));
    }
    constexpr std::size_t kStripePixels =
        static_cast<std::size_t>(kVideoWidth) * kJpegMcuHeight * kMaxVideoScale;
    if (!s_video_stripe) {
        s_video_stripe = static_cast<uint16_t *>(
            std::malloc(kStripePixels * sizeof(uint16_t)));
    }
    return s_jpeg_work && s_video_stripe;
}

void release_direct_video_buffers() {
    // Color transfers are queued by the SPI panel driver. A parameter command
    // first drains every in-flight color transaction, so the buffers are no
    // longer owned by DMA when they are released below. ST7789 command 0x00 is
    // NOP and does not alter the visible frame.
    if (bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
    std::free(s_jpeg_work);
    s_jpeg_work = nullptr;
    std::free(s_video_stripe);
    s_video_stripe = nullptr;
    s_video_color_pending = false;
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   uint32_t color, int radius = 0) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    (void)radius;
    lv_obj_set_style_radius(object, 0, 0);
    if (color == UI_SURFACE || color == UI_SURFACE_ALT) {
        lv_obj_set_style_border_width(object, ui_theme_is_classic() ? 1 : 0, 0);
        lv_obj_set_style_border_color(object, lv_color_hex(UI_BORDER), 0);
    }
    return object;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                     uint32_t color, const lv_font_t *font = &buddy_font_16) {
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

void set_centered(lv_obj_t *object) {
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
}

static inline lv_obj_t *make_taskbar(lv_obj_t *parent) {
    (void)parent;
    return nullptr;
}

const char *playback_text() {
    switch (s_playback_state) {
        case RadioPlaybackState::Stopped: return "已暂停";
        case RadioPlaybackState::Connecting: return "正在连接电台";
        case RadioPlaybackState::Buffering: return "正在缓冲";
        case RadioPlaybackState::Playing: return "正在播放";
        case RadioPlaybackState::Reconnecting: return "信号中断，正在重连";
        case RadioPlaybackState::Error: return "暂时无法播放";
    }
    return "准备播放";
}

void apply_network() {
    // 共享信号格:已连接满格亮绿,未连接全暗(与首页/Launcher 同语义)
    ui_pixel_wifi_bars_set(s_wifi_bars, s_network_connected ? 3 : 0,
                           s_network_connected ? kGreen : kMuted);
}

// Maps a station onto the dial. A real frequency lands where it belongs on the
// scale. Most directory entries carry no frequency, so those stations are
// spread evenly across the band by their position in the list, which keeps the
// needle meaningful (it still says "which station") and always moving.
int dial_x_for_station() {
    const int span = kDialTrackRight - kDialTrackLeft;
    if (s_station_frequency >= kDialBandLow && s_station_frequency <= kDialBandHigh) {
        const int offset = (s_station_frequency - kDialBandLow) * span /
                           (kDialBandHigh - kDialBandLow);
        return kDialTrackLeft + offset;
    }
    // Inset the synthetic positions so the needle never pins to either end,
    // which would read as "stuck" rather than "tuned".
    const int count = static_cast<int>(std::max<std::size_t>(s_station_count, 1));
    const int index = static_cast<int>(std::min<std::size_t>(s_station_index,
                                                             s_station_count - 1));
    const int inset = span / 8;
    const int usable = span - inset * 2;
    const int offset = count > 1 ? inset + usable * index / (count - 1)
                                 : inset + usable / 2;
    return kDialTrackLeft + offset;
}

void place_needle(int32_t x) {
    s_needle_x = x;
    if (s_dial_needle) {
        lv_obj_set_pos(s_dial_needle, static_cast<int>(x) - kDialNeedleWidth / 2, 18);
    }
    if (s_dial_glow) {
        lv_obj_set_pos(s_dial_glow, static_cast<int>(x) - 9, 33);
    }
}

void needle_anim_cb(void *, int32_t value) { place_needle(value); }

void draw_radio_dial(lv_event_t *event) {
    pixel_scene::Ink ink(event);
    constexpr int count = (kDialBandHigh - kDialBandLow) / 5 + 1;
    for (int i = 0; i < count; ++i) {
        const int x = kDialTrackLeft + i * (kDialTrackRight - kDialTrackLeft) / (count - 1);
        const bool major = (kDialBandLow + i * 5) % 40 == 0;
        ink.box(x, major ? 25 : 30, major ? 2 : 1, major ? 11 : 6, kMuted);
    }
    ink.box(kDialTrackLeft, 36, kDialTrackRight - kDialTrackLeft + 1, 1, kGrid);
}

void draw_radio_body(lv_event_t *event) {
    pixel_scene::Ink ink(event);
    // A compact physical receiver: station window, tuner and speaker grille.
    ink.box(12, 74, 216, 77, kPanel);
    ink.box(12, 74, 3, 12, kAmber);
    ink.box(225, 139, 3, 12, kAmber);
    for (int y = 211; y < 237; y += 6)
        for (int x = 164; x < 224; x += 6) ink.box(x, y, 2, 2, kMuted);
}

void apply_dial(bool animate) {
    if (!s_dial_needle) return;
    const int target = dial_x_for_station();

    if (s_dial_readout) {
        char text[16];
        if (s_station_frequency >= kDialBandLow && s_station_frequency <= kDialBandHigh) {
            std::snprintf(text, sizeof(text), "%u.%u",
                          static_cast<unsigned>(s_station_frequency / 10U),
                          static_cast<unsigned>(s_station_frequency % 10U));
        } else {
            // Network stations without a broadcast frequency show their index.
            std::snprintf(text, sizeof(text), "NET %02u", unsigned(s_station_index + 1));
        }
        lv_label_set_text(s_dial_readout, text);
    }

    lv_anim_delete(s_dial_needle, needle_anim_cb);
    if (!animate || s_needle_x == target) {
        place_needle(target);
        return;
    }

    // Sweep time scales with distance so a neighbouring station feels quick and
    // a jump across the band still reads as a deliberate tune.
    const int distance = std::abs(target - static_cast<int>(s_needle_x));
    const uint32_t duration =
        static_cast<uint32_t>(std::clamp(160 + distance * 3, 200, 620));

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_dial_needle);
    lv_anim_set_exec_cb(&animation, needle_anim_cb);
    lv_anim_set_values(&animation, s_needle_x, target);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static bool contains_ignore_case(const char *hay, const char *sub) {
    if (!hay || !sub || !sub[0]) return false;
    std::size_t hlen = std::strlen(hay), slen = std::strlen(sub);
    if (slen > hlen) return false;
    for (std::size_t i = 0; i <= hlen - slen; ++i) {
        if (strncasecmp(hay + i, sub, slen) == 0) return true;
    }
    return false;
}

void apply_location();

void apply_station() {
    if (!s_station_name || !s_station_number || !s_station_description) return;
    char preset[20];
    std::snprintf(preset, sizeof(preset), "CH %02u / %02u",
                  static_cast<unsigned>(s_station_index + 1),
                  static_cast<unsigned>(s_station_count));
    lv_label_set_text(s_station_number, preset);
    lv_label_set_text(s_station_name, s_station_name_text);
    ui_pixel_label_set_text(s_station_description, s_station_description_text);
    apply_location();
    apply_dial(true);
}

bool showing_playback_detail() {
    return s_network_connected && (s_playback_state == RadioPlaybackState::Error ||
                                   s_playback_state == RadioPlaybackState::Reconnecting);
}

// The location row doubles as the explanation while playback is failing or
// retrying, so "播放失败" never stands without a reason.
void apply_location() {
    if (s_location && showing_playback_detail()) {
        ui_pixel_label_set_text(s_location, s_playback_detail);
        ui_pixel_text_color(s_location, s_playback_state == RadioPlaybackState::Error
                                            ? UI_DANGER : UI_WARNING);
        return;
    }
    if (s_location) {
        ui_pixel_text_color(s_location, UI_TEXT_DIM);
        char location[152];
        const bool is_national = (std::strcmp(s_location_text, "NATIONAL") == 0);
        const bool city_in_name = (!is_national && s_location_text[0] &&
            contains_ignore_case(s_station_name_text, s_location_text));
        if (!s_station_description_text[0] && !is_national) {
            std::snprintf(location, sizeof(location), "%s", s_location_text);
        } else if (is_national || city_in_name || !s_location_text[0]) {
            std::snprintf(location, sizeof(location), "%s", s_station_description_text);
        } else {
            std::snprintf(location, sizeof(location), "%s · %s", s_location_text, s_station_description_text);
        }
        ui_pixel_label_set_text(s_location, location);
    }
}

void apply_playback() {
    if (!s_status || !s_play_icon) return;
    const char *status=!s_network_connected?"网络未连接":s_playback_state==RadioPlaybackState::Playing?"播放中":
        s_playback_state==RadioPlaybackState::Connecting?"连接中":s_playback_state==RadioPlaybackState::Buffering?"缓冲中":
        s_playback_state==RadioPlaybackState::Reconnecting?"重连中":s_playback_state==RadioPlaybackState::Error?"播放失败":"已暂停";
    ui_pixel_label_set_text(s_status,status);
    ui_pixel_text_color(s_status,s_playback_state==RadioPlaybackState::Error?UI_DANGER:!s_network_connected?UI_WARNING:UI_TEXT_DIM);
    const bool playing = s_playback_state == RadioPlaybackState::Playing ||
                         s_playback_state == RadioPlaybackState::Buffering ||
                         s_playback_state == RadioPlaybackState::Connecting ||
                         s_playback_state == RadioPlaybackState::Reconnecting;
    if (s_main_hint) ui_pixel_label_set_text(s_main_hint, !s_network_connected ? "先用手机完成配网" : playing
        ? "上下换台 · 确定暂停"
        : s_playback_state == RadioPlaybackState::Error
            ? "上下换台 · 确定重试" : "上下换台 · 确定播放");
    apply_location();
    ui_pixel_label_set_text(s_play_icon, playing ? "II" : ">");
    ui_pixel_text_color(s_play_icon,
        s_playback_state == RadioPlaybackState::Error ? kRed : kAmber);
}

void apply_volume() {
    if (!s_volume_value || !s_volume_bar) return;
    char text[16];
    std::snprintf(text, sizeof(text), "%u%%", s_volume);
    lv_label_set_text(s_volume_value, text);
    lv_bar_set_value(s_volume_bar, s_volume, LV_ANIM_ON);
}

void apply_sleep_timer() {
    if (!s_sleep_timer || !s_status) return;
    const uint32_t seconds=radio_sleep_timer_remaining_seconds(),now=lv_tick_get();
    const bool volume=s_volume_until && (int32_t)(s_volume_until-now)>0;
    const bool visible=s_network_connected&&s_playback_state!=RadioPlaybackState::Error&&(volume||seconds>0);
    lv_obj_set_flag(s_sleep_timer,LV_OBJ_FLAG_HIDDEN,!visible);
    lv_obj_set_flag(s_status,LV_OBJ_FLAG_HIDDEN,visible);
    char value[32];
    if(volume)std::snprintf(value,sizeof(value),"音量 %u%%",s_volume);
    else std::snprintf(value,sizeof(value),"定时 %02u:%02u",(unsigned)(seconds/60),(unsigned)(seconds%60));
    ui_pixel_label_set_text(s_sleep_timer,value);
    s_last_sleep_seconds=seconds;
}

void build_main() {
    lv_obj_clean(s_screen);
    s_page = Page::Main;
    s_station_number = nullptr;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_main_hint = nullptr;
    s_location = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_pair_dot = nullptr;
    s_sleep_timer = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    s_needle_x = kDialNeedleHome;
    s_last_sleep_seconds = UINT32_MAX;

    s_main_group = make_box(s_screen, 0, 0, 240, 320, kBackground);
    lv_obj_add_event_cb(s_main_group, draw_radio_body, LV_EVENT_DRAW_MAIN, nullptr);

    ui_pixel_top_bar(s_main_group);
    ui_pixel_wifi_bars_create(s_main_group, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    make_label(s_main_group, "收音机", 42, 3, 106, UI_TEXT, &buddy_font_16);
    s_clock = nullptr;
    s_battery = ui_pixel_battery_create(s_main_group, UI_STATUS_BATTERY_X, 6, UI_TEXT,
                                        &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    s_battery_ticks = 0;
    s_clock_tick = lv_tick_get() - 1000;

    s_station_number=make_label(s_main_group,"",12,42,104,UI_TEXT_DIM,&lv_font_montserrat_14);
    s_status=make_label(s_main_group,"",116,42,112,UI_TEXT_DIM);
    lv_obj_set_height(s_status,20);lv_obj_set_style_text_align(s_status,LV_TEXT_ALIGN_RIGHT,0);
    s_sleep_timer=make_label(s_main_group,"",116,42,112,UI_TEXT_DIM);
    lv_obj_set_style_text_align(s_sleep_timer,LV_TEXT_ALIGN_RIGHT,0);
    // One font for every dynamic name: the old 26px subset and 16px fallback
    // made individual Han glyphs jump in size within the same title.
    s_station_name=make_label(s_main_group,"",22,82,196,kText,&buddy_font_16);
    lv_obj_set_height(s_station_name,44);lv_label_set_long_mode(s_station_name,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(s_station_name,3,0);
    s_station_description=make_label(s_main_group,"",12,126,216,UI_TEXT_DIM);
    lv_obj_add_flag(s_station_description,LV_OBJ_FLAG_HIDDEN);
    s_location=make_label(s_main_group,"",22,126,196,UI_TEXT_DIM);
    lv_obj_set_height(s_location,22);
    lv_obj_t *dial=make_box(s_main_group,12,156,216,42,kBackground);
    lv_obj_add_event_cb(dial, draw_radio_dial, LV_EVENT_DRAW_MAIN, nullptr);
    for(int mhz=88;mhz<=108;mhz+=4){
        const int x=kDialTrackLeft+(mhz*10-kDialBandLow)*(kDialTrackRight-kDialTrackLeft)/(kDialBandHigh-kDialBandLow);
        auto *number=make_label(dial,mhz==88?"88":mhz==92?"92":mhz==96?"96":mhz==100?"100":mhz==104?"104":"108",x-15,3,30,kMuted,&lv_font_montserrat_14);
        set_centered(number);
    }
    s_dial_glow=nullptr;
    s_dial_needle=make_box(dial,kDialNeedleHome-kDialNeedleWidth/2,18,kDialNeedleWidth,23,kAmber);
    s_dial_readout=make_label(s_main_group,"",12,203,146,kAmber,&ui_digits_32);
    lv_obj_set_height(s_dial_readout,40);
    auto *meter_panel=make_box(s_main_group,12,242,216,22,kBackground);
    for(std::size_t i=0;i<kMeterCount;i++){s_meter[i]=make_box(meter_panel,2+i*12,20,8,2,kGrid);lv_obj_set_style_bg_opa(s_meter[i],LV_OPA_TRANSP,0);}
    s_play_icon=make_label(s_main_group,"",0,0,1,kAmber);lv_obj_add_flag(s_play_icon,LV_OBJ_FLAG_HIDDEN);
    s_main_hint=ui_pixel_footer(s_main_group,"上下换台 · 确定播放","长按：上下音量 · 确定返回",&buddy_font_16);

    apply_network();
    apply_station();
    apply_playback();
    apply_sleep_timer();
}

void build_config(const char *ssid, const char *url) {
    lv_obj_clean(s_screen);
    s_page = Page::Config;
    s_station_number = nullptr;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_main_hint = nullptr;
    s_location = nullptr;
    s_clock = nullptr;
    s_battery = {nullptr, nullptr};
    s_sleep_timer = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    for (auto &bar : s_wifi_bars) bar = nullptr;
    for (auto &bar : s_meter) bar = nullptr;

    make_label(s_screen, "联网收音机", 12, 12, 216, kText, &ui_font_20);
    lv_obj_t *tag = make_label(s_screen, "首次配网", 12, 45, 216, kAmber);
    set_centered(tag);
    s_pair_dot = make_box(s_screen, 112, 77, 16, 16, kRed, LV_RADIUS_CIRCLE);

    lv_obj_t *panel = make_box(s_screen, 18, 111, 204, 132, kPanel, 10);
    lv_obj_set_style_border_color(panel, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_t *hint = make_label(panel, "手机连接热点", 0, 14, 204, kMuted);
    set_centered(hint);
    lv_obj_t *ssid_label = make_label(panel, ssid && ssid[0] ? ssid : "LEO Radio",
                                      8, 43, 188, kAmber);
    set_centered(ssid_label);
    hint = make_label(panel, "连接后会自动打开配网页面", 0, 77, 204, kText);
    set_centered(hint);
    lv_obj_t *address = make_label(panel, url && url[0] ? url : "192.168.4.1",
                                   0, 104, 204, kMuted,
                                   &lv_font_montserrat_14);
    set_centered(address);
    hint = make_label(s_screen, "请选择 2.4G 无线网络", 12, 259, 216, kText);
    make_taskbar(s_screen);
    set_centered(hint);
    hint = make_label(s_screen, "配网完成后会自动开始播放", 12, 291, 216, UI_TEXT);
    set_centered(hint);
}

const lv_font_t *ssid_font(const char *ssid) {
    (void)ssid;
    return &buddy_font_16;
}

void reset_transient_objects(Page page) {
    lv_obj_clean(s_screen);
    s_page = page;
    s_station_number = nullptr;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_main_hint = nullptr;
    s_location = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_clock = nullptr;
    s_battery = {nullptr, nullptr};
    s_sleep_timer = nullptr;
    s_pair_dot = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    s_wifi_state = nullptr;
    s_wifi_list_hint = nullptr;
    s_wifi_keyboard_network = nullptr;
    s_wifi_password_mask = nullptr;
    s_wifi_keyboard_status = nullptr;
    s_easter_message = nullptr;
    for (auto &bar : s_wifi_bars) bar = nullptr;
    for (auto &bar : s_meter) bar = nullptr;
    for (auto &row : s_settings_rows) row = nullptr;
    for (auto &label : s_settings_labels) label = nullptr;
    for (auto &box : s_wifi_row_box) box = nullptr;
    for (auto &label : s_wifi_row_ssid) label = nullptr;
    for (auto &row_bars : s_wifi_row_bars) {
        for (auto &bar : row_bars) bar = nullptr;
    }
    for (auto &box : s_wifi_key_box) box = nullptr;
    for (auto &label : s_wifi_key_label) label = nullptr;
}

void apply_settings_selection() {
    for (std::size_t i = 0; i < s_settings_row_count; ++i) {
        if (!s_settings_rows[i] || !s_settings_labels[i]) continue;
        const bool active = i == s_settings_selected;
        ui_pixel_set_selected(s_settings_rows[i], active, true);
        lv_obj_set_style_bg_color(s_settings_rows[i],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        lv_obj_set_style_text_color(s_settings_labels[i],
                                    lv_color_hex(kText), 0);
    }
}

void build_settings(RadioSettingsPage page, std::size_t selected, uint8_t volume) {
    reset_transient_objects(Page::Settings);
    s_settings_page = page;
    s_settings_selected = selected;
    s_volume = volume;
    s_settings_row_count = 0;

    if (page == RadioSettingsPage::Volume) {
        lv_obj_t *title = make_label(s_screen, "音量", 12, 16, 216, kText,
                                     &buddy_font_16);
        set_centered(title);
        s_volume_value = make_label(s_screen, "55%", 12, 84, 216, kAmber,
                                    &lv_font_montserrat_20);
        set_centered(s_volume_value);
        s_volume_bar = lv_bar_create(s_screen);
        lv_obj_set_pos(s_volume_bar, 30, 136);
        lv_obj_set_size(s_volume_bar, 180, 14);
        lv_bar_set_range(s_volume_bar, 0, 100);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kGrid), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kAmber), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_volume_bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_volume_bar, 0, LV_PART_INDICATOR);
        make_taskbar(s_screen);
        lv_obj_t *hint = make_label(s_screen, "上下调节  确定返回",
                                    12, 291, 216, UI_TEXT);
        set_centered(hint);
        apply_volume();
        return;
    }

    static constexpr const char *kMenuItems[] = {
        "音量", "城市电台", "定时播放", "屏幕设置", "网络设置", "蓝牙", "返回",
    };
    static constexpr const char *kCityItems[] = {
        "自动定位", "北京", "上海", "广州", "深圳", "长沙", "杭州", "成都",
    };
    static constexpr const char *kSleepTimerItems[] = {
        "无定时", "15 分钟", "30 分钟", "60 分钟", "90 分钟", "返回",
    };
    char brightness_item[32];
    char timeout_item[48];
    std::snprintf(brightness_item, sizeof(brightness_item), "亮度 %u%%",
                  static_cast<unsigned>(radio_display_brightness()));
    const uint16_t timeout = radio_display_timeout_seconds();
    if (!timeout) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 关闭");
    } else if (timeout < 60) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 秒",
                      static_cast<unsigned>(timeout));
    } else {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 分钟",
                      static_cast<unsigned>(timeout / 60));
    }
    const char *display_items[] = {brightness_item, timeout_item, "返回"};
    static constexpr const char *kWifiMenuItems[] = {
        "连接新网络", "已存网络", "返回",
    };

    const char *const *items = nullptr;
    std::size_t item_count = 0;
    const char *title_text = "设置";
    const char *hint_text = "上下选择  确定进入";
    switch (page) {
        case RadioSettingsPage::Menu:
            items = kMenuItems;
            item_count = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
            break;
        case RadioSettingsPage::City:
            items = kCityItems;
            item_count = sizeof(kCityItems) / sizeof(kCityItems[0]);
            title_text = "城市电台";
            hint_text = "确定选择  长按返回";
            break;
        case RadioSettingsPage::SleepTimer:
            items = kSleepTimerItems;
            item_count = sizeof(kSleepTimerItems) / sizeof(kSleepTimerItems[0]);
            title_text = "定时播放";
            hint_text = "确定开始  长按返回";
            break;
        case RadioSettingsPage::Display:
            items = display_items;
            item_count = sizeof(display_items) / sizeof(display_items[0]);
            title_text = "屏幕设置";
            hint_text = "确定切换  长按返回";
            break;
        case RadioSettingsPage::WifiMenu:
            items = kWifiMenuItems;
            item_count = sizeof(kWifiMenuItems) / sizeof(kWifiMenuItems[0]);
            title_text = "网络设置";
            hint_text = "确定进入  长按返回";
            break;
        default:
            return;
    }

    const bool compact = item_count > 3;
    const bool extra_compact = item_count > 6;
    const bool city_rows = item_count >= 8;
    const int first_y = city_rows ? 34 : (extra_compact ? 39 : (compact ? 45 : 70));
    const int row_height = city_rows ? 27 : (extra_compact ? 29 : (compact ? 32 : 42));
    const int row_gap = city_rows ? 30 : (extra_compact ? 34 : (compact ? 38 : 55));
    s_settings_row_count = item_count;
    if (s_settings_selected >= item_count) s_settings_selected = 0;

    lv_obj_t *title = make_label(s_screen, title_text, 12, 13, 216, kText,
                                 &buddy_font_16);
    set_centered(title);
    for (std::size_t i = 0; i < item_count; ++i) {
        const int y = first_y + static_cast<int>(i) * row_gap;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, row_height, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], items[i], 0,
                                          city_rows ? 5 :
                                          extra_compact ? 5 : (compact ? 7 : 12),
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    make_taskbar(s_screen);
    lv_obj_t *hint = make_label(s_screen, hint_text, 12, 291, 216, UI_TEXT);
    set_centered(hint);
}

void build_saved_wifi(const char *const *ssids, std::size_t count,
                      std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::SavedWifi;
    const std::size_t total = count + 1;
    selected = std::min(selected, total - 1);

    lv_obj_t *title = make_label(s_screen, "已存网络", 12, 12, 216, kText,
                                 &buddy_font_16);
    set_centered(title);
    lv_obj_t *state = make_label(s_screen,
                                  count ? "上下选择  确定设置" : "无保存网络",
                                 12, 40, 216, count ? kMuted : kAmber);
    set_centered(state);

    const std::size_t first = total <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             total - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        if (index >= total) break;
        const bool active = index == selected;
        const int y = 67 + static_cast<int>(row) * 34;
        s_settings_rows[row] = make_box(s_screen, 18, y, 204, 29,
                                        active ? kAmberSoft : kPanel, 5);
        ui_pixel_set_selected(s_settings_rows[row], active, true);
        const char *text = index < count ? ssids[index] : "返回";
        s_settings_labels[row] = make_label(
            s_settings_rows[row], passport::ssid_display(text, &buddy_font_16).c_str(), 8, 6, 188, active ? UI_AMBER : kMuted,
            index < count ? ssid_font(text) : &buddy_font_16);
        set_centered(s_settings_labels[row]);
    }
    make_taskbar(s_screen);
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 291, 216, UI_TEXT);
    set_centered(hint);
}

void build_wifi_action(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::WifiAction;
    s_settings_selected = selected % 3;
    s_settings_row_count = 3;
    lv_obj_t *title = make_label(s_screen, "网络设置", 12, 13, 216, kText,
                                 &buddy_font_16);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str(), 12, 45, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    static constexpr const char *kItems[] = {"设为首选", "删除网络", "返回"};
    for (std::size_t i = 0; i < 3; ++i) {
        const int y = 83 + static_cast<int>(i) * 55;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 42, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 12,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    make_taskbar(s_screen);
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 291, 216, UI_TEXT);
    set_centered(hint);
}

void build_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::WifiDeleteConfirm;
    s_settings_selected = selected % 2;
    s_settings_row_count = 2;
    lv_obj_t *title = make_label(s_screen, "确认删除", 12, 18, 216, kText,
                                 &buddy_font_16);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str(), 12, 55, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    lv_obj_t *warning = make_label(s_screen, "删除后无法自动连接", 12, 88, 216,
                                   kMuted);
    set_centered(warning);
    static constexpr const char *kItems[] = {"返回", "确认删除"};
    for (std::size_t i = 0; i < 2; ++i) {
        const int y = 132 + static_cast<int>(i) * 61;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 45, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 13,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
}

void build_easter_egg(const char *message) {
    reset_transient_objects(Page::EasterEgg);
    lv_obj_t *title = make_label(s_screen, "蓝牙", 12, 12, 216, kAmber,
                                 &buddy_font_16);
    set_centered(title);
    s_easter_message = make_label(s_screen, message ? message : "正在加载彩蛋",
                                  12, 146, 216, kText);
    set_centered(s_easter_message);
    make_taskbar(s_screen);
    lv_obj_t *hint = make_label(s_screen, "确定返回", 12, 291, 216, UI_TEXT);
    set_centered(hint);
}

void update_wifi_list(const WifiNetworkView *networks, std::size_t count,
                      std::size_t selected, bool scanning, bool can_cancel) {
    const char *state_text = scanning ? "正在寻找网络" :
                             count ? "选择网络后输入密码" : "未找到网络 · 确定重试";
    lv_label_set_text(s_wifi_state, state_text);
    lv_obj_set_style_text_color(s_wifi_state,
                                lv_color_hex(scanning ? kAmber : kMuted), 0);
    if (s_wifi_list_hint) {
        lv_label_set_text(s_wifi_list_hint,
                          can_cancel ? "上下选择 · 长按确定返回" : "上下选择 · 确定连接");
    }

    const std::size_t first = count <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             count - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        const bool present = index < count;
        const bool active = present && index == selected;
        ui_pixel_set_selected(s_wifi_row_box[row], active, true);
        lv_obj_set_style_bg_color(s_wifi_row_box[row],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        if (!present) {
            lv_label_set_text(s_wifi_row_ssid[row], "");
            for (int b = 0; b < 3; ++b) {
                lv_obj_set_style_bg_opa(s_wifi_row_bars[row][b], LV_OPA_0, 0);
            }
            continue;
        }

        const uint32_t color = active ? UI_AMBER : kMuted;
        char ssid_disp[34 + 2];
        std::snprintf(ssid_disp, sizeof(ssid_disp), "%s%s",
                      networks[index].secure ? "*" : "", passport::ssid_display(networks[index].ssid, &buddy_font_16).c_str());
        lv_label_set_text(s_wifi_row_ssid[row], ssid_disp);
        lv_obj_set_style_text_font(s_wifi_row_ssid[row],
                                   ssid_font(networks[index].ssid), 0);
        lv_obj_set_style_text_color(s_wifi_row_ssid[row], lv_color_hex(color), 0);
        // 右侧"WiFi 信号"三格:RSSI 分档与首页一致(-60/-75)
        const int rssi = networks[index].rssi;
        const int level = rssi == -128 ? 0 : rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        for (int b = 0; b < 3; ++b) {
            lv_obj_set_style_bg_color(s_wifi_row_bars[row][b],
                                      lv_color_hex(active ? kGrid : kMuted), 0);
            lv_obj_set_style_bg_opa(s_wifi_row_bars[row][b],
                                    rssi == -128 ? LV_OPA_0 : b < level ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

void build_wifi_list(const WifiNetworkView *networks, std::size_t count,
                     std::size_t selected, bool scanning, const char *setup_ap_ssid,
                     bool can_cancel) {
    reset_transient_objects(Page::WifiList);
    // The compact UI font contains the complete setup vocabulary. The larger
    // station-title font intentionally contains only station-name glyphs.
    make_label(s_screen, "无线网络", 12, 12, 216, kText, &ui_font_20);
    s_wifi_state = make_label(s_screen, "", 12, 42, 216, kMuted);
    set_centered(s_wifi_state);

    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const int y = 68 + static_cast<int>(row) * 34;
        s_wifi_row_box[row] = make_box(s_screen, 12, y, 216, 29, kPanel, 5);
        s_wifi_row_ssid[row] = make_label(s_wifi_row_box[row], "", 9, 6, 172,
                                          kMuted, &buddy_font_16);
        // 右侧"WiFi 信号"三格条:与首页/桌面顶栏同一视觉语言
        for (int b = 0; b < 3; ++b) {
            const int h = 5 + b * 4;
            s_wifi_row_bars[row][b] =
                make_box(s_wifi_row_box[row], 190 + b * 6, 22 - h, 4, h, kMuted, 1);
        }
    }

    make_taskbar(s_screen);
    s_wifi_list_hint = make_label(s_screen, "", 12, 279, 216, UI_TEXT);
    set_centered(s_wifi_list_hint);
    char portal[96];
    if (setup_ap_ssid && setup_ap_ssid[0]) std::snprintf(portal, sizeof(portal), "手机连 %s", setup_ap_ssid);
    else std::snprintf(portal, sizeof(portal), "长按下返回");
    lv_obj_t *hint = make_label(s_screen, portal, 12, 300, 216, UI_MUTED,
                                &buddy_font_16);
    set_centered(hint);
    update_wifi_list(networks, count, selected, scanning, can_cancel);
}

void update_wifi_keyboard(const char *ssid, std::size_t password_length,
                          WifiKeyboardPage page, std::size_t selected,
                          const char *status) {
    lv_label_set_text(s_wifi_keyboard_network, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str());
    lv_obj_set_style_text_font(s_wifi_keyboard_network, ssid_font(ssid), 0);

    char password[48] = {};
    const std::size_t stars = std::min<std::size_t>(password_length, 38);
    std::memset(password, '*', stars);
    password[stars] = '\0';
    lv_label_set_text(s_wifi_password_mask, password_length ? password : "输入密码");
    lv_obj_set_style_text_color(s_wifi_password_mask,
                                lv_color_hex(password_length ? kText : kMuted), 0);

    const std::size_t key_count = radio_ui_wifi_key_count(page);
    if (key_count > 0) selected %= key_count;
    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        if (i >= key_count) {
            lv_obj_add_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
        const bool active = i == selected;
        // The keyboard uses the label itself as the key box to keep LVGL heap
        // usage bounded. ui_pixel_set_selected() only changes a border color;
        // labels have no border width by default. kAmberSoft and kPanelSoft
        // also both resolve to UI_SURFACE_ALT, so the old code rendered active
        // and inactive keys identically even though the selection index moved.
        // Give the focused key an explicit high-contrast fill and border so
        // the three physical buttons provide visible feedback on every click.
        lv_obj_set_style_bg_color(s_wifi_key_box[i],
                                  lv_color_hex(active ? UI_AMBER : kPanelSoft), 0);
        lv_obj_set_style_border_width(s_wifi_key_box[i], active ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_wifi_key_box[i],
                                      lv_color_hex(active ? UI_TEXT : UI_BORDER), 0);
        const char *value = radio_ui_wifi_key(page, i);
        // Display a visible keycap but keep its input value as ASCII U+0020.
        lv_label_set_text(s_wifi_key_label[i], std::strcmp(value, " ") == 0 ? "SP" :
            std::strcmp(value, "DEL") == 0 ? "BS" : value);
        lv_obj_set_style_text_color(s_wifi_key_label[i],
                                    lv_color_hex(active ? UI_BG : kText), 0);
    }

    char navigation[112];
    const char *selected_key = key_count > 0 ? radio_ui_wifi_key(page, selected) : "";
    const char *action = std::strcmp(selected_key, " ") == 0 ? "确定输入空格" :
        std::strcmp(selected_key, "DEL") == 0 ? (password_length ? "确定删除一位" : "确定返回网络列表") :
        std::strcmp(selected_key, "GO") == 0 ? "确定连接网络" :
        std::strcmp(selected_key, "ABC") == 0 ? "确定切换大写字母" :
        std::strcmp(selected_key, "abc") == 0 ? "确定切换小写字母" :
        std::strcmp(selected_key, "123") == 0 ? "确定切换数字符号" :
        std::strcmp(selected_key, "#+=") == 0 ? "确定查看更多符号" : nullptr;
    if (action) std::snprintf(navigation, sizeof(navigation), "%s", action);
    else std::snprintf(navigation, sizeof(navigation), "确定输入 %s · 已输 %u 位",
                       selected_key, (unsigned)password_length);
    lv_label_set_text(s_wifi_keyboard_status,
                      status && status[0] ? status : navigation);
    lv_obj_set_style_text_color(s_wifi_keyboard_status,
                                lv_color_hex(status && status[0] ? kRed : UI_TEXT), 0);
}

void build_wifi_keyboard(const char *ssid, std::size_t password_length,
                         WifiKeyboardPage page, std::size_t selected,
                         const char *status) {
    reset_transient_objects(Page::WifiKeyboard);
    make_label(s_screen, "输入密码", 12, 10, 216, kText, &ui_font_20);
    s_wifi_keyboard_network = make_label(s_screen, "", 12, 39, 216, kAmber,
                                          &buddy_font_16);
    set_centered(s_wifi_keyboard_network);

    lv_obj_t *password_box = make_box(s_screen, 18, 66, 204, 34, kPanel, 6);
    s_wifi_password_mask = make_label(password_box, "输入密码", 8, 8, 188, kMuted,
                                      &buddy_font_16);
    set_centered(s_wifi_password_mask);

    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        constexpr int kColumns = 6;
        const int x = 15 + static_cast<int>(i % kColumns) * 35;
        const int y = 109 + static_cast<int>(i / kColumns) * 28;
        // One styled label per key keeps the keyboard within the 24 KB LVGL
        // heap. A separate box + label for every key exhausted that heap near
        // key 31 and caused an immediate Store access fault.
        s_wifi_key_label[i] = make_label(s_screen, "", x, y, 31, kText,
                                         &lv_font_montserrat_14);
        s_wifi_key_box[i] = s_wifi_key_label[i];
        lv_obj_set_size(s_wifi_key_label[i], 31, 24);
        lv_obj_set_style_bg_color(s_wifi_key_label[i], lv_color_hex(kPanelSoft), 0);
        lv_obj_set_style_bg_opa(s_wifi_key_label[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wifi_key_label[i], 0, 0);
        lv_obj_set_style_pad_top(s_wifi_key_label[i], 4, 0);
        set_centered(s_wifi_key_label[i]);
    }

    make_taskbar(s_screen);
    s_wifi_keyboard_status = make_label(s_screen, "", 12, 279, 216, UI_TEXT);
    set_centered(s_wifi_keyboard_status);
    lv_obj_t *hint = make_label(s_screen, "上下移动 · 长按下返回", 12, 300, 216, UI_MUTED);
    set_centered(hint);
    update_wifi_keyboard(ssid, password_length, page, selected, status);
}

void build_wifi_connecting(const char *ssid, const char *detail, bool success = false) {
    reset_transient_objects(Page::WifiConnecting);
    make_label(s_screen, success ? "无线网络已连接" : "正在连接无线网络", 12, 37, 216, kText,
               &buddy_font_16);
    lv_obj_t *network = make_label(s_screen, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str(), 12, 102, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    s_pair_dot = make_box(s_screen, 108, 151, 24, 24, kAmber, LV_RADIUS_CIRCLE);
    if (success) lv_obj_add_flag(s_pair_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *message = make_label(s_screen, detail ? detail : "正在连接",
                                   12, 205, 216, kMuted);
    set_centered(message);
}

void timer_callback(lv_timer_t *timer) {
    // Audio and the sleep timer run independently of this visual timer.
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible ? 50 : 1000);
    if (!visible) return;
    s_animation += 17;
    if (s_page == Page::Config || s_page == Page::WifiConnecting) {
        if (s_pair_dot) {
            const uint8_t triangle = s_animation < 128 ? s_animation : 255 - s_animation;
            lv_obj_set_style_opa(s_pair_dot, 70 + triangle, 0);
        }
        return;
    }

    if(s_volume_until && (int32_t)(lv_tick_get()-s_volume_until)>=0){s_volume_until=0;apply_sleep_timer();}
    // The audio meter needs 20 Hz; clock and sleep labels only need 1 Hz.
    const uint32_t now = lv_tick_get();
    if (now - s_clock_tick >= 1000) {
        s_clock_tick = now;
        if (s_clock) {
            // Keep the same timezone and unsynchronized placeholder as Home.
            char text[8];
            home_clock_format_time(text, sizeof(text));
            ui_pixel_label_set_text(s_clock, text);
        }
        apply_sleep_timer();
    }

    if (++s_battery_ticks >= 100) {
        s_battery_ticks = 0;
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    }

    const uint32_t generation = s_level_generation.load(std::memory_order_acquire);
    if (generation != s_last_level_generation) {
        s_last_level_generation = generation;
        for (std::size_t i = 0; i < kMeterCount; ++i) {
            if (!s_meter[i]) continue;
            const int height = 2 + s_levels[i] * 19 / 100;
            lv_obj_set_pos(s_meter[i], 2 + static_cast<int>(i) * 12, 22 - height);
            lv_obj_set_size(s_meter[i], 8, height);
            const uint32_t color = s_levels[i] > 85 ? kRed :
                                   s_levels[i] > 62 ? kAmber : kGreen;
            ui_pixel_bg_color(s_meter[i], color);
            // Silent bars vanish so the idle meter is not another rule above the footer.
            lv_obj_set_style_bg_opa(s_meter[i], s_levels[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

}  // namespace

bool radio_ui_init() {
    if (s_screen) return true;
    if (!bsp_lvgl_lock(1000)) return false;
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    // start() 紧接着会建立目标页面。这里不预建再清空，避免首次进入 Radio
    // 重复分配整套 LVGL 对象并把启动时间翻倍。
    s_ui_timer = lv_timer_create(timer_callback, 50, nullptr);
    if (s_ui_timer) lv_timer_pause(s_ui_timer);
    bsp_lvgl_unlock();
    return true;
}

void radio_ui_show_main() {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_main();
    lv_screen_load(s_screen);
    if (s_ui_timer) lv_timer_resume(s_ui_timer);
    bsp_lvgl_unlock();
}

void radio_ui_hide() {
    if (!bsp_lvgl_lock(500)) return;
    if (s_ui_timer) lv_timer_pause(s_ui_timer);
    reset_transient_objects(Page::Main);
    bsp_lvgl_unlock();
}

void radio_ui_show_config(const char *ssid, const char *url) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_config(ssid, url);
    bsp_lvgl_unlock();
}

void radio_ui_deinit() {
    if (!bsp_lvgl_lock(-1)) return;
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = nullptr; }
    if (s_screen) { reset_transient_objects(Page::Main); lv_obj_delete(s_screen); s_screen = nullptr; }
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_list(const WifiNetworkView *networks, std::size_t count,
                             std::size_t selected, bool scanning,
                             const char *setup_ap_ssid, bool can_cancel) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiList && s_wifi_state) {
        update_wifi_list(networks, count, selected, scanning, can_cancel);
    } else {
        build_wifi_list(networks, count, selected, scanning, setup_ap_ssid,
                        can_cancel);
        lv_screen_load(s_screen);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_keyboard(const char *ssid, std::size_t password_length,
                                 WifiKeyboardPage page, std::size_t selected,
                                 const char *status) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiKeyboard && s_wifi_password_mask) {
        update_wifi_keyboard(ssid, password_length, page, selected, status);
    } else {
        build_wifi_keyboard(ssid, password_length, page, selected, status);
        lv_screen_load(s_screen);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_connecting(const char *ssid, const char *detail) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_connecting(ssid, detail);
    lv_screen_load(s_screen);
    if (s_ui_timer) lv_timer_resume(s_ui_timer);
    bsp_lvgl_unlock();
}

const char *radio_ui_wifi_key(WifiKeyboardPage page, std::size_t index) {
    return passport::wifi_keyboard::key(page, index);
}

void radio_ui_show_wifi_success(const char *ssid) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_connecting(ssid, "连接成功 · 确定返回", true);
    lv_screen_load(s_screen);
    if (s_ui_timer) lv_timer_pause(s_ui_timer);
    bsp_lvgl_unlock();
}

std::size_t radio_ui_wifi_key_count(WifiKeyboardPage page) {
    return passport::wifi_keyboard::page(page).count;
}

void radio_ui_set_network(bool connected, const char *detail) {
    s_network_connected = connected;
    if (detail) std::snprintf(s_network_detail, sizeof(s_network_detail), "%s", detail);
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_network();
    apply_playback();
    apply_sleep_timer();
    bsp_lvgl_unlock();
}

void radio_ui_set_location(const char *location) {
    std::snprintf(s_location_text, sizeof(s_location_text), "%s",
                  location && location[0] ? location : "NATIONAL");
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    if (s_location) apply_station();
    bsp_lvgl_unlock();
}

void radio_ui_set_station(std::size_t index, std::size_t count,
                          const RadioStation &station) {
    s_station_index = index;
    s_station_count = std::max<std::size_t>(count, 1);
    s_station_frequency = station.frequency_decihz;
    std::snprintf(s_station_name_text, sizeof(s_station_name_text), "%s", station.name);
    std::snprintf(s_station_description_text, sizeof(s_station_description_text), "%s",
                  station.description);
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_station();
    bsp_lvgl_unlock();
}

void radio_ui_set_playback(RadioPlaybackState state, const char *detail) {
    s_playback_state = state;
    std::snprintf(s_playback_detail, sizeof(s_playback_detail), "%s",
                  detail && detail[0] ? detail : playback_text());
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_playback();
    apply_sleep_timer();
    bsp_lvgl_unlock();
}

void radio_ui_set_audio_levels(const uint8_t *levels, std::size_t count) {
    if (!levels) return;
    const std::size_t copy_count = std::min(count, kMeterCount);
    std::memcpy(s_levels, levels, copy_count);
    if (copy_count < kMeterCount) {
        std::memset(s_levels + copy_count, 0, kMeterCount - copy_count);
    }
    s_level_generation.fetch_add(1, std::memory_order_release);
}

void radio_ui_show_settings(RadioSettingsPage page, std::size_t selected,
                            uint8_t volume) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::Settings && s_settings_page == page &&
        page != RadioSettingsPage::Volume &&
        page != RadioSettingsPage::Display && s_settings_row_count > 0) {
        s_settings_selected = selected % s_settings_row_count;
        apply_settings_selection();
    } else {
        build_settings(page, selected, volume);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_saved_wifi(const char *const *ssids, std::size_t count,
                              std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_saved_wifi(ssids, count, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_action(const char *ssid, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_action(ssid, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_delete_confirm(ssid, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_easter_egg(const char *message) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::EasterEgg && s_easter_message) {
        lv_label_set_text(s_easter_message, message ? message : "正在加载彩蛋");
        lv_obj_remove_flag(s_easter_message, LV_OBJ_FLAG_HIDDEN);
    } else {
        build_easter_egg(message);
    }
    bsp_lvgl_unlock();
}

bool radio_ui_show_easter_egg_frame(const uint8_t *jpeg, std::size_t size,
                                    uint16_t width, uint16_t height) {
    if (!jpeg || !size || !s_screen || !width || !height ||
        kVideoWidth % width != 0 || kVideoHeight % height != 0) {
        ESP_LOGE("radio_ui", "Video frame rejected: jpeg=%p size=%u %ux%u screen=%p",
                 jpeg, static_cast<unsigned>(size),
                 static_cast<unsigned>(width), static_cast<unsigned>(height),
                 s_screen);
        return false;
    }
    const uint16_t scale_x = kVideoWidth / width;
    const uint16_t scale_y = kVideoHeight / height;
    if (scale_x != scale_y || scale_x == 0 || scale_x > kMaxVideoScale) {
        ESP_LOGE("radio_ui", "Video scale invalid %ux%u", scale_x, scale_y);
        return false;
    }
    if (!ensure_direct_video_buffers()) {
        ESP_LOGE("radio_ui", "Video buffers unavailable: work=%p stripe=%p",
                 s_jpeg_work, s_video_stripe);
        return false;
    }
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGE("radio_ui", "LVGL lock timeout for video frame");
        return false;
    }
    if (s_page != Page::EasterEgg) {
        ESP_LOGE("radio_ui", "Video frame page mismatch page=%d", static_cast<int>(s_page));
        bsp_lvgl_unlock();
        return false;
    }

    if (s_video_color_pending) {
        if (!drain_video_color_transfers()) {
            bsp_lvgl_unlock();
            return false;
        }
        s_video_color_pending = false;
    }

    DirectJpegContext context = {
        .data = jpeg,
        .size = size,
        .position = 0,
        .source_width = width,
        .source_height = height,
        .scale = static_cast<uint8_t>(scale_x),
        .failed = false,
    };
    JDEC decoder = {};
    JRESULT result = jd_prepare(&decoder, direct_jpeg_input, s_jpeg_work,
                                kJpegWorkSize, &context);
    if (result == JDR_OK && decoder.width == width && decoder.height == height) {
        result = jd_decomp(&decoder, direct_jpeg_output, 0);
    } else {
        ESP_LOGE("radio_ui", "JPEG prepare failed %d (%ux%u expected %ux%u)",
                 result, decoder.width, decoder.height, width, height);
    }
    if (result != JDR_OK || context.failed) {
        ESP_LOGE("radio_ui", "JPEG decomp failed %d context_failed=%d",
                 result, context.failed);
    }
    bsp_lvgl_unlock();
    return result == JDR_OK && !context.failed;
}

void radio_ui_finish_easter_egg() {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    release_direct_video_buffers();
    build_main();
    bsp_lvgl_unlock();
}

void radio_ui_set_volume(uint8_t volume) {
    s_volume = volume;
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_volume();
    s_volume_until = lv_tick_get() + 1200;
    s_last_sleep_seconds = UINT32_MAX;
    apply_sleep_timer();
    bsp_lvgl_unlock();
}
