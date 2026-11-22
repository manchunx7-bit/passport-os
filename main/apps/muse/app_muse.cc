#include "apps/app_base.h"
#include "launcher/app_registry.h"
#include "passport_muse.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "muse_avatar_frames.h"
#include <cstdio>
#include <cstring>

namespace {
lv_obj_t *s_screen, *s_name, *s_status, *s_detail, *s_reply, *s_action, *s_avatar;
uint32_t s_reply_revision;
size_t s_page_at, s_next_page;
unsigned s_page_number, s_page_count;
bool s_page_dirty, s_showing_reply;
lv_timer_t *s_timer;
ui_pixel_battery_t s_battery{};
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
passport_muse_stage_t s_stage;
bool s_started;
uint32_t s_stage_at, s_layout_at, s_happy_until, s_battery_at, s_clock_at;
int s_frame, s_layout, s_from_size, s_from_y, s_from_status, s_size, s_y, s_status_y;
uint16_t s_level;
uint32_t s_repair_armed_until;
static const lv_image_colorkey_t kTransparent = {{0,0,0},{0,0,0}};

lv_obj_t *label(const char *text, int y, int height, uint32_t color) {
    auto *o = ui_pixel_label(s_screen, text, &buddy_font_16, color);
    lv_obj_set_pos(o, 12, y); lv_obj_set_size(o, 216, height);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}
void visible(lv_obj_t *o, bool show) {
    if (show == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
int mix(int from, int to, uint32_t elapsed) {
    if (elapsed >= 280) return to;
    // Integer ease-out: no LVGL animation objects or off-screen image layers.
    int remain = 280 - (int)elapsed;
    return to + (from - to) * remain * remain / (280 * 280);
}
void composition(int layout, uint32_t now) {
    const int target_size = layout == 2 ? 96 : layout == 1 ? 128 : 192;
    const int target_y = layout == 2 ? 28 : layout == 1 ? 48 : 42;
    const int target_status = layout == 2 ? 126 : layout == 1 ? 189 : 228;
    if (layout != s_layout) {
        s_layout = layout; s_layout_at = now;
        s_from_size = s_size; s_from_y = s_y; s_from_status = s_status_y;
    }
    uint32_t age = now - s_layout_at;
    int size = mix(s_from_size, target_size, age);
    int y = mix(s_from_y, target_y, age);
    int status_y = mix(s_from_status, target_status, age);
    if (size != s_size) {
        lv_image_set_scale(s_avatar, size * 256 / 64);
        s_size = size;
    }
    if (y != s_y || lv_obj_get_x(s_avatar) != (240-size)/2) {
        lv_obj_set_pos(s_avatar, (240-size)/2, y); s_y = y;
    }
    if (status_y != s_status_y) { lv_obj_set_y(s_status, status_y); s_status_y = status_y; }
    visible(s_reply, layout == 2 && age >= 280);
    visible(s_detail, layout != 2 && age >= 280);
    if (layout != 2) {
        int dy = layout == 1 ? 216 : 252;
        if (lv_obj_get_y(s_detail) != dy) {
            lv_obj_set_y(s_detail,dy); lv_obj_set_height(s_detail,layout == 1 ? 44 : 20);
        }
    }
}
bool pairing_problem(const char *detail) {
    return std::strstr(detail, "配对") || std::strstr(detail, "重新添加");
}
// The footer must not contradict the detail: pairing problems are fixed by
// pairing again, and memory/Bluetooth leftovers only clear after leaving.
// Network errors never suggest pairing: it wipes the account link, and
// pairing again away from home needs the phone and its Bluetooth.
const char *error_action(const char *detail) {
    if (pairing_problem(detail)) return "长按下键重新配对";
    if (std::strstr(detail, "重启")) return "请重启设备后再连接";
    if (std::strstr(detail, "返回") || std::strstr(detail, "重新进入")) return "返回后重新打开 Muse";
    return "确定重试";
}
int avatar_frame(const passport_muse_snapshot_t &state, uint32_t now) {
    uint32_t age = now - s_stage_at;
    if (state.stage == PASSPORT_MUSE_ERROR) return 28 + (age / 700) % 2;
    if (state.stage == PASSPORT_MUSE_LISTENING) {
        s_level = (uint16_t)((s_level * 2u + state.level) / 3u);
        unsigned band = s_level > 9000 ? 3 : s_level > 4000 ? 2 : s_level > 900 ? 1 : 0;
        return 10 + band * 2 + (age / 160) % 2;
    }
    if (state.stage == PASSPORT_MUSE_WORKING) return 18 + (age / 240) % 6;
    if (state.stage == PASSPORT_MUSE_CONNECTING || state.stage == PASSPORT_MUSE_PHONE_SETUP)
        return age < 800 ? 6 + age / 200 : 18 + (age / 350) % 6;
    if ((int32_t)(s_happy_until - now) > 0) return 24 + (now / 160) % 4;
    uint32_t cycle = age % 4400;
    if (cycle >= 3840 && cycle < 3960) return 5;
    static const uint8_t sway[] = {0,1,2,3,4,3,2,1};
    return sway[(cycle / 480) % 8];
}
void refresh() {
    if (!s_screen || !app_registry_screen_is_on()) return;
    uint32_t now = lv_tick_get();
    passport_muse_snapshot_t state; passport_muse_snapshot(&state);
    bool changed = !s_started || state.stage != s_stage;
    if (changed) { s_stage = state.stage; s_stage_at = now; s_clock_at = now - 1000; s_level = 0; }
    ui_pixel_label_set_text(s_name, std::strcmp(state.name, "Muse") == 0 ? "" : state.name);
    const char *status = "连接你的 Muse", *action = "手机设置页添加 Muse", *detail = "在手机设置页填写 SDK Token";
    bool guidance = true;
    switch (state.stage) {
        case PASSPORT_MUSE_NEEDS_TOKEN: break;
        case PASSPORT_MUSE_PAIRING: status = "等待配对"; action = "在手机添加设备"; break;
        case PASSPORT_MUSE_CONFIRM: status = "是你的手机吗"; detail = "确认后即可连接 Muse"; action = "按确定键允许配对"; break;
        case PASSPORT_MUSE_PHONE_SETUP: status = "就快好了"; detail = "在手机完成联网设置"; action = "在手机继续操作"; break;
        case PASSPORT_MUSE_CONNECTING:
            status = "正在连接"; action = "正在连接你的 Muse"; guidance = false;
            // Reconnect reasons ("内存不足，正在重连" ...) explain a long wait.
            detail = std::strcmp(state.detail, action) == 0 ? "" : state.detail; break;
        case PASSPORT_MUSE_READY: status = "我准备好了"; detail = ""; action = "按住上键说话 · 松开发送"; guidance = false; break;
        case PASSPORT_MUSE_LISTENING: status = "我在听"; action = "松开上键发送"; detail = ""; guidance = false; break;
        case PASSPORT_MUSE_WORKING: status = "正在思考"; action = "下键停止等待"; detail = ""; guidance = false; break;
        case PASSPORT_MUSE_REPLY: status = "收到回复"; action = "按住上键继续说话"; detail = ""; guidance = false; break;
        case PASSPORT_MUSE_ERROR:
            status = "连接遇到问题"; detail = state.detail;
            action = (int32_t)(s_repair_armed_until - now) > 0 ? "再次长按下键清除配对" : error_action(state.detail);
            break;
    }
    bool showing_reply = state.reply_is_answer && state.reply[0];
    if (showing_reply && (state.reply_revision != s_reply_revision || s_page_dirty)) {
        char page[256];
        if (state.reply_revision != s_reply_revision) {
            s_reply_revision = state.reply_revision; s_happy_until = now + 960;
            s_page_at = 0; s_page_number = 1; s_page_count = 0;
            size_t at = 0, next;
            do {
                next = muse_hatch_reply_page(state.reply, at, page, sizeof(page));
                s_page_count++; if (next <= at) break; at = next;
            } while (state.reply[at]);
        }
        s_next_page = muse_hatch_reply_page(state.reply, s_page_at, page, sizeof(page));
        ui_pixel_label_set_text(s_reply, page); s_page_dirty = false;
        app_registry_keep_awake();
    }
    char reply_status[64];
    if (showing_reply) {
        const char *heading = state.stage == PASSPORT_MUSE_WORKING || state.stage == PASSPORT_MUSE_CONNECTING ? "恢复同步" :
                              state.stage == PASSPORT_MUSE_ERROR ? "同步中断" : "收到回复";
        snprintf(reply_status, sizeof(reply_status), "%s %u/%u", heading, s_page_number, s_page_count);
        status = reply_status;
        action = state.stage == PASSPORT_MUSE_ERROR ? "确定重连 · 上键继续说话" :
                 s_page_count > 1 ? "确定翻页 · 上键继续说话" : "按住上键继续说话";
    }
    ui_pixel_label_set_text(s_status, status);
    ui_pixel_text_color(s_status, state.stage == PASSPORT_MUSE_ERROR ? UI_DANGER : UI_TEXT);
    char pairing_hint[96];
    if (state.stage == PASSPORT_MUSE_PAIRING) {
        const char *tail = strrchr(state.device_name, '-');
        snprintf(pairing_hint, sizeof(pairing_hint), "手机 Muse / 设置 / 设备\n选择尾号 %s", tail ? tail + 1 : state.device_name);
        detail = pairing_hint;
    }
    if (!showing_reply && (changed || now - s_clock_at >= 1000)) {
        char clock[48];
        unsigned seconds = (now - s_stage_at) / 1000;
        if (state.stage == PASSPORT_MUSE_LISTENING || state.stage == PASSPORT_MUSE_WORKING) {
            snprintf(clock,sizeof(clock),"%s %02u:%02u",state.stage == PASSPORT_MUSE_LISTENING ? "录音" : "等待",seconds/60,seconds%60);
            detail = clock;
        }
        ui_pixel_label_set_text(s_detail, detail); s_clock_at = now;
    }
    ui_pixel_label_set_text(s_action, action);
    int layout = showing_reply ? 2 : guidance ? 1 : 0;
    if (!s_started) {
        s_size = s_from_size = layout == 2 ? 96 : layout == 1 ? 128 : 192;
        s_y = s_from_y = layout == 2 ? 28 : layout == 1 ? 48 : 42;
        s_status_y = s_from_status = layout == 2 ? 126 : layout == 1 ? 189 : 228;
        s_layout = layout; s_layout_at = now - 280;
        lv_image_set_scale(s_avatar,s_size*256/64);lv_obj_set_pos(s_avatar,(240-s_size)/2,s_y);
        lv_obj_set_y(s_status,s_status_y);s_started = true;
    }
    composition(layout,now); s_showing_reply = showing_reply;
    int frame = avatar_frame(state,now);
    if (frame != s_frame) { lv_image_set_src(s_avatar,&muse_avatar_frames[frame]);s_frame = frame; }
    if (state.stage == PASSPORT_MUSE_LISTENING || state.stage == PASSPORT_MUSE_WORKING) app_registry_keep_awake();
    if (now - s_battery_at >= 5000) {
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
        s_battery_at = now;
    }
}
void tick(lv_timer_t *timer) {
    lv_timer_set_period(timer, app_registry_screen_is_on() ? 50 : 1000);
    refresh();
}
void init() {}
void start() {
    passport_muse_start();
    if (!bsp_lvgl_lock(1000)) { passport_muse_stop(); return; }
    s_screen = lv_obj_create(nullptr); ui_pixel_background(s_screen);
    auto *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    auto *title = ui_pixel_label(top, "Muse", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_name = ui_pixel_label(top, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_pos(s_name, 100, 4); lv_obj_set_size(s_name, UI_STATUS_CONTENT_RIGHT - 100, 20);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    s_avatar = lv_image_create(s_screen);
    lv_image_set_src(s_avatar,&muse_avatar_frames[0]);
    lv_image_set_pivot(s_avatar,0,0);lv_image_set_antialias(s_avatar,false);
    lv_obj_set_style_image_colorkey(s_avatar,&kTransparent,0);
    s_status = label("",228,24,UI_TEXT);
    s_detail = label("",252,20,UI_TEXT_DIM);
    s_reply = label("",154,108,UI_TEXT);
    lv_obj_set_style_text_align(s_reply,LV_TEXT_ALIGN_LEFT,0);
    lv_obj_set_style_text_line_space(s_reply,2,0);
    s_action = ui_pixel_footer(s_screen,"","长按确定返回",&buddy_font_16);
    s_reply_revision=0;s_page_at=s_next_page=0;s_page_number=s_page_count=1;s_page_dirty=true;
    s_started=false;s_showing_reply=false;s_frame=-1;s_layout=-1;s_happy_until=0;s_battery_at=lv_tick_get()-5000;
    refresh();lv_screen_load(s_screen);s_timer=lv_timer_create(tick,50,nullptr);
    bsp_lvgl_unlock();
}
void stop() {
    if (bsp_lvgl_lock(-1)) {
        if (s_timer) {lv_timer_delete(s_timer);s_timer=nullptr;}
        bsp_lvgl_unlock();
    }
    passport_muse_stop();
    if (bsp_lvgl_lock(-1)) {
        if (s_screen) {lv_obj_delete(s_screen);s_screen=nullptr;}
        s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
        bsp_lvgl_unlock();
    }
}
void key(uint8_t button, app_btn_event_t event, uint16_t) {
    if (button == BSP_BTN_UP) {
        if (event == BTN_EVT_PRESS) passport_muse_key(true, false);
        if (event == BTN_EVT_RELEASE) passport_muse_key(false, false);
    }
    if (button == BSP_BTN_DOWN && event == BTN_EVT_CLICK) passport_muse_key(false, true);
    if (button == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        passport_muse_snapshot_t state; passport_muse_snapshot(&state);
        uint32_t now = lv_tick_get();
        // Clearing a working pairing by accident is costly outside: unless the
        // error is about pairing, ask for a second long press within 4 s.
        if (state.stage == PASSPORT_MUSE_ERROR && !pairing_problem(state.detail) &&
            (int32_t)(s_repair_armed_until - now) <= 0) {
            s_repair_armed_until = now + 4000;
            return;
        }
        s_repair_armed_until = 0;
        passport_muse_repair();
    }
    if (button == BSP_BTN_OK && event == BTN_EVT_CLICK) {
        passport_muse_snapshot_t state; passport_muse_snapshot(&state);
        if (state.reply_is_answer && state.reply[0] && state.stage != PASSPORT_MUSE_ERROR) {
            if (state.reply_revision != s_reply_revision) { s_page_at = 0; s_page_number = 1; }
            else if (s_next_page < sizeof(state.reply) && state.reply[s_next_page]) { s_page_at = s_next_page; s_page_number++; }
            else { s_page_at = 0; s_page_number = 1; }
            s_page_dirty = true;
        } else passport_muse_confirm();
    }
}
}
extern const passport_app_t g_muse_app = {
    .id = APP_ID_MUSE, .name = "Muse", .en_name = "MUSE", .desc = "语音交给你的 Muse",
    .tag = "需代理", .theme_color = 0x8DA7B0,
    .init = init, .start = start, .stop = stop, .on_key = key,
    .key = "muse", .in_launcher = true, .openable = true, .toggleable = true,
    .default_enabled = true, .streaming = true,
    .heap_largest_kb = 20, .heap_free_kb = 44,
};
