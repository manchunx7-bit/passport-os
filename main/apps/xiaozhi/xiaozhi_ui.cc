// Xiaozhi: shared terminal conversation view and bounded UTF-8 replies.
#include "apps/xiaozhi/xiaozhi_ui.h"
#include "apps/xiaozhi/xiaozhi_face.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"
#include "ui_palette.h"
#include "display_text.h"
#include "utf8_tail.h"
#include "app_fonts.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include <esp_log.h>
#include <cstdio>

namespace xiaozhi {

namespace {

// 状态点颜色:与整机状态色语义一致(墨/纸/草/黄/红/天蓝)
#define kTextMain (UI_TEXT)
#define kTextMuted (UI_SUB)
#define kUserBlue (UI_SKY_DARK)
#define kGreenAccent (UI_GRASS_DARK)
#define kAmberAccent (UI_WARNING)
#define kRedAccent (UI_RED)

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_status_badge = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_msg_container = nullptr;
lv_obj_t *s_user_text_label = nullptr;
lv_obj_t *s_ai_text_label = nullptr;
lv_obj_t *s_hint_label = nullptr;
lv_obj_t *s_companion = nullptr;
State s_face_state = State::Idle;
uint32_t s_badge_color = 0;
face::Animator s_face_animator;
face::Pose s_face_pose{};
face::Waveform s_waveform;
lv_timer_t *s_face_timer = nullptr;
lv_timer_t *s_volume_timer = nullptr;
lv_timer_t *s_status_timer = nullptr;
ui_pixel_battery_t s_battery{};
lv_obj_t *s_wifi_bars[3]{};
const char *s_default_hint = "确定说话 · 上下音量";

// 文本内容也只在持锁时读写，避免与 lv_label 生命周期脱节。
std::string s_user_text;
std::string s_ai_reply_buffer;
bool s_text_expanded = false;

constexpr size_t kMaxTextBytes = 1200;  // up to 400 three-byte Han characters
constexpr int kTextLineSpace = 4;

int text_height_for_lines(int lines) {
    return lines * buddy_font_16.line_height + (lines - 1) * kTextLineSpace;
}

void refresh_status_bar() {
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    auto &wifi = WifiManager::GetInstance();
    const bool connected = wifi.IsConnected();
    const int rssi = connected ? wifi.GetRssi() : -127;
    const int bars = !connected ? 0 : rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1;
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, connected ? UI_TEXT : UI_TEXT_DIM);
}

face::Activity face_activity(State state) {
    switch (state) {
    case State::Disconnected: return face::Activity::Offline;
    case State::Connecting:   return face::Activity::Connecting;
    case State::Listening:    return face::Activity::Listening;
    case State::Thinking:     return face::Activity::Thinking;
    case State::Speaking:     return face::Activity::Speaking;
    case State::Error:        return face::Activity::Error;
    default:                  return face::Activity::Idle;
    }
}

face::Palette face_palette() {
    const ui_theme_t theme = ui_theme_get();
    const bool classic = theme == UI_THEME_CLASSIC;
    face::Palette p{};
    p.shell = UI_SURFACE;
    p.edge = UI_BORDER;
    // The light theme gets a dark visor so the eyes still glow.
    p.screen = theme == UI_THEME_PLAYFUL ? UI_TEXT : UI_BG;
    p.eye = s_face_state == State::Error ? UI_DANGER : UI_ACCENT;
    p.glint = classic ? UI_TEXT : 0xFFFFFF;
    p.blush = classic ? UI_ACCENT : theme == UI_THEME_NEON ? UI_SECONDARY : UI_DANGER;
    p.tear = classic ? UI_TEXT : theme == UI_THEME_PLAYFUL ? UI_SECONDARY : 0x8FC7EF;
    p.warn = UI_WARNING;
    return p;
}

void draw_companion(lv_event_t *event) {
    lv_area_t area;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(event)), &area);
    face::draw_big(lv_event_get_layer(event), area, s_face_pose, face_palette());
}

void draw_badge(lv_event_t *event) {
    lv_area_t area;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(event)), &area);
    face::draw_mini(lv_event_get_layer(event), area, s_face_pose, face_palette(), s_badge_color);
}

// Runs inside lv_timer_handler (LVGL lock held). Redraw only when the pose
// changed, and not at all while the panel sleeps.
void face_tick(lv_timer_t *timer) {
    // Like the radio and music timers: idle at 1 Hz while the panel is off.
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible ? face::kFramePeriodMs : 1000);
    if (!s_screen || !visible) return;
    face::Pose pose = s_face_animator.step(face_activity(s_face_state));
    const bool audio_active = s_face_state == State::Listening || s_face_state == State::Speaking;
    if (audio_active) s_waveform.step(AudioService::VisualLevel());
    else s_waveform.clear();
    for (int i = 0; i < face::kWaveBars; ++i) pose.bars[i] = s_waveform.bars[i];
    if (pose == s_face_pose) return;
    s_face_pose = pose;
    if (s_companion && !lv_obj_has_flag(s_companion, LV_OBJ_FLAG_HIDDEN)) lv_obj_invalidate(s_companion);
    if (s_status_badge) lv_obj_invalidate(s_status_badge);
}

void set_badge_color(uint32_t color) {
    s_badge_color = color;
    if (s_status_badge) lv_obj_invalidate(s_status_badge);
}

void show_companion(bool visible) {
    if (!s_companion) return;
    if (visible) {
        lv_obj_remove_flag(s_companion, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_msg_container, 12, 212);
        lv_obj_set_height(s_msg_container, text_height_for_lines(2));
    } else {
        lv_obj_add_flag(s_companion, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_msg_container, 12, 76);
        lv_obj_set_height(s_msg_container, text_height_for_lines(8));
    }
}

bool companion_needed() {
    return !s_text_expanded && (s_face_state == State::Listening ||
           s_face_state == State::Thinking || s_face_state == State::Speaking ||
           (s_user_text.empty() && s_ai_reply_buffer.empty()));
}

void conversation_layout() {
    lv_obj_set_flex_align(s_msg_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_START);
    lv_obj_set_style_text_align(s_ai_text_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_update_layout(s_msg_container);
    // Measure the wrapped labels, including the gap between user and AI.
    // More than two lines gets the full reading area even while speaking.
    const bool have_ai = !lv_obj_has_flag(s_ai_text_label, LV_OBJ_FLAG_HIDDEN);
    int content_height = have_ai ? lv_obj_get_height(s_ai_text_label) : 0;
    if (!lv_obj_has_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN)) {
        content_height += lv_obj_get_height(s_user_text_label) + (have_ai ? kTextLineSpace : 0);
    }
    s_text_expanded = content_height > text_height_for_lines(2);
    show_companion(companion_needed());
}

// 滚动到消息区底部，保证最新一轮对话可见
void scroll_to_bottom_locked() {
    if (s_msg_container) {
        lv_obj_update_layout(s_msg_container);
        lv_obj_scroll_to_y(s_msg_container, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// 所有"改文案"的操作都走这里：先加锁、再加一次指针校验。
// 返回 false 表示控件已销毁（应用正在退出），调用方静默丢弃即可。
bool begin_update(uint32_t timeout_ms) {
    if (!bsp_lvgl_lock(timeout_ms)) return false;
    if (s_screen == nullptr) {                      // 加锁期间被 ui_hide 销毁
        bsp_lvgl_unlock();
        return false;
    }
    return true;
}

} // namespace

void ui_init() {
    if (!bsp_lvgl_lock(1000)) return;

    if (s_volume_timer) {
        lv_timer_delete(s_volume_timer);
        s_volume_timer = nullptr;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = nullptr;
    }
    if (s_face_timer) {
        lv_timer_delete(s_face_timer);
        s_face_timer = nullptr;
    }

    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    s_status_badge = nullptr;
    s_status_label = nullptr;
    s_msg_container = nullptr;
    s_user_text_label = nullptr;
    s_ai_text_label = nullptr;
    s_hint_label = nullptr;
    s_companion = nullptr;
    s_face_state = State::Idle;
    s_user_text.clear();
    s_ai_reply_buffer.clear();
    s_text_expanded = false;

    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    auto *header = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(header, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    auto *title = ui_pixel_label(header, "小智 AI", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(header, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    refresh_status_bar();

    lv_obj_t *top_bar = lv_obj_create(s_screen);
    lv_obj_set_size(top_bar, 216, 28);
    lv_obj_set_pos(top_bar, 12, 42);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_pad_hor(top_bar, 0, 0);
    lv_obj_set_style_pad_ver(top_bar, 0, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Mini face keeps the expression visible once the conversation hides the big one.
    s_status_badge = lv_obj_create(top_bar);
    lv_obj_remove_style_all(s_status_badge);
    lv_obj_set_size(s_status_badge, face::kMiniW, face::kMiniH);
    lv_obj_align(s_status_badge, LV_ALIGN_LEFT_MID, 0, -1);
    lv_obj_remove_flag(s_status_badge, LV_OBJ_FLAG_SCROLLABLE);
    s_badge_color = kAmberAccent;
    lv_obj_add_event_cb(s_status_badge, draw_badge, LV_EVENT_DRAW_MAIN, nullptr);

    s_status_label = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_status_label, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(kTextMain), 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_status_label, 216 - face::kMiniW - 8, 20);
    lv_label_set_text(s_status_label, "准备中...");
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, face::kMiniW + 8, 0);

    // Keep the conversation open and reserve the shared two-line key footer.
    s_msg_container = lv_obj_create(s_screen);
    lv_obj_set_size(s_msg_container, 216, 186);
    lv_obj_set_pos(s_msg_container, 12, 78);
    lv_obj_set_style_bg_color(s_msg_container, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_msg_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_msg_container, 0, 0);
    lv_obj_set_style_border_width(s_msg_container, 0, 0);
    lv_obj_set_style_border_color(s_msg_container, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_pad_all(s_msg_container, 0, 0);
    // Match the gap between labels to the gap between wrapped lines, so
    // bottom-follow always leaves complete rows at the top and bottom.
    lv_obj_set_style_pad_row(s_msg_container, kTextLineSpace, 0);
    lv_obj_set_style_text_line_space(s_msg_container, kTextLineSpace, 0);
    lv_obj_set_flex_flow(s_msg_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_msg_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_msg_container, LV_SCROLLBAR_MODE_AUTO);

    s_user_text_label = lv_label_create(s_msg_container);
    lv_obj_set_width(s_user_text_label, 216);
    lv_obj_set_height(s_user_text_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_user_text_label, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_user_text_label, lv_color_hex(kUserBlue), 0);
    lv_label_set_long_mode(s_user_text_label, LV_LABEL_LONG_WRAP);
    s_user_text.clear();
    lv_label_set_text(s_user_text_label, s_user_text.c_str());
    lv_obj_add_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN);

    s_ai_text_label = lv_label_create(s_msg_container);
    lv_obj_set_width(s_ai_text_label, 216);
    lv_obj_set_height(s_ai_text_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_ai_text_label, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_ai_text_label, lv_color_hex(kTextMain), 0);
    lv_label_set_long_mode(s_ai_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ai_text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ai_text_label, "确定开始说话\n再按确定结束");

    s_companion = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_companion);
    lv_obj_set_pos(s_companion, (240 - face::kBigW) / 2, 88);
    lv_obj_set_size(s_companion, face::kBigW, face::kBigH);
    lv_obj_remove_flag(s_companion, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_companion, draw_companion, LV_EVENT_DRAW_MAIN, nullptr);
    show_companion(true);

    s_default_hint = "确定说话 · 上下音量";
    s_hint_label = ui_pixel_footer(s_screen, s_default_hint, "长按确定返回", &buddy_font_16);
    s_status_timer = lv_timer_create([](lv_timer_t *) {
        if (s_screen && app_registry_screen_is_on()) refresh_status_bar();
    }, 5000, nullptr);
    s_face_animator = face::Animator{};
    s_waveform.clear();
    s_face_pose = s_face_animator.step(face_activity(s_face_state));
    s_face_timer = lv_timer_create(face_tick, face::kFramePeriodMs, nullptr);

    bsp_lvgl_unlock();
}

void ui_show() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen == nullptr) {
        bsp_lvgl_unlock();
        ui_init();
        if (!bsp_lvgl_lock(1000)) return;
    }
    if (s_screen) lv_screen_load(s_screen);
    bsp_lvgl_unlock();
}

void ui_hide() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_volume_timer) {
        lv_timer_delete(s_volume_timer);
        s_volume_timer = nullptr;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = nullptr;
    }
    if (s_face_timer) {
        lv_timer_delete(s_face_timer);
        s_face_timer = nullptr;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
        s_status_badge = nullptr;
        s_status_label = nullptr;
        s_msg_container = nullptr;
        s_user_text_label = nullptr;
        s_ai_text_label = nullptr;
        s_hint_label = nullptr;
        s_companion = nullptr;
        s_battery = {};
        for (auto &bar : s_wifi_bars) bar = nullptr;
    }
    std::string().swap(s_user_text);
    std::string().swap(s_ai_reply_buffer);
    bsp_lvgl_unlock();
}

void ui_set_state(State state) {
    if (!begin_update(200)) return;
    if (s_face_state != state) s_waveform.clear();
    s_face_state = state;
    conversation_layout();
    scroll_to_bottom_locked();
    if (s_companion) lv_obj_invalidate(s_companion);  // palette may change (error eyes)
    s_default_hint = state == State::Listening ? "确定结束 · 上下音量" :
                     state == State::Speaking || state == State::Thinking ?
                     "确定打断 · 上下音量" : "确定说话 · 上下音量";
    ui_pixel_label_set_text(s_hint_label, s_default_hint);
    ui_pixel_text_color(s_status_label,
        state == State::Error || state == State::Disconnected ? UI_DANGER :
        state == State::Connecting || state == State::Thinking ? UI_WARNING : UI_TEXT);

    switch (state) {
    case State::Disconnected:
        set_badge_color(kRedAccent);
        if (!WifiManager::GetInstance().IsConnected()) {
            lv_label_set_text(s_status_label, "Wi-Fi 未连接，请配网");
        } else {
            lv_label_set_text(s_status_label, "未连接 · 确定重试");
        }
        break;
    case State::Connecting:
        set_badge_color(kAmberAccent);
        lv_label_set_text(s_status_label, "正在连接小智");
        break;
    case State::Idle:
        set_badge_color(kGreenAccent);
        lv_label_set_text(s_status_label, "准备就绪");
        break;
    case State::Listening:
        set_badge_color(kUserBlue);
        lv_label_set_text(s_status_label, "正在聆听");
        break;
    case State::Thinking:
        set_badge_color(kAmberAccent);
        lv_label_set_text(s_status_label, "正在思考");
        break;
    case State::Speaking:
        set_badge_color(kGreenAccent);
        lv_label_set_text(s_status_label, "正在回复");
        break;
    case State::Error:
        set_badge_color(kRedAccent);
        lv_label_set_text(s_status_label, "服务异常 · 确定重试");
        break;
    }

    bsp_lvgl_unlock();
}

void ui_set_emotion(const char *emotion) {
    if (!begin_update(200)) return;
    s_face_animator.set_mood(face::mood_from_emotion(emotion));
    bsp_lvgl_unlock();
}

void ui_set_status_text(const std::string &text) {
    if (!begin_update(200)) return;
    lv_label_set_text(s_status_label, text.c_str());
    bsp_lvgl_unlock();
}

void ui_set_user_text(const std::string &text) {
    if (!begin_update(200)) return;
    lv_obj_remove_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN);
    s_user_text = "你说: ";
    passport::append_utf8_tail(s_user_text, passport::fold_display_text(text), kMaxTextBytes);
    lv_label_set_text(s_user_text_label, s_user_text.c_str());
    conversation_layout();
    scroll_to_bottom_locked();
    bsp_lvgl_unlock();
}

void ui_set_ai_text(const std::string &text) {
    if (!begin_update(200)) return;
    lv_obj_remove_flag(s_ai_text_label, LV_OBJ_FLAG_HIDDEN);
    const std::string folded = passport::fold_display_text(text);
    const auto tail = passport::utf8_tail(folded, kMaxTextBytes);
    s_ai_reply_buffer.assign(tail.data(), tail.size());
    std::string display = "小智: " + s_ai_reply_buffer;
    lv_label_set_text(s_ai_text_label, display.c_str());
    conversation_layout();
    scroll_to_bottom_locked();
    bsp_lvgl_unlock();
}

void ui_append_ai_text(const std::string &text) {
    if (!begin_update(200)) return;
    lv_obj_remove_flag(s_ai_text_label, LV_OBJ_FLAG_HIDDEN);

    passport::append_utf8_tail(s_ai_reply_buffer, passport::fold_display_text(text), kMaxTextBytes);
    std::string display = "小智: " + s_ai_reply_buffer;
    lv_label_set_text(s_ai_text_label, display.c_str());
    conversation_layout();
    scroll_to_bottom_locked();
    bsp_lvgl_unlock();
}

void ui_set_hint(const std::string &text) {
    if (!begin_update(200)) return;
    lv_label_set_text(s_hint_label, text.c_str());
    bsp_lvgl_unlock();
}

void ui_set_volume(unsigned percent) {
    if (!begin_update(200)) return;
    lv_label_set_text_fmt(s_hint_label, "音量 %u%%", percent);
    if (s_volume_timer) {
        lv_timer_reset(s_volume_timer);
    } else {
        s_volume_timer = lv_timer_create([](lv_timer_t *) {
            s_volume_timer = nullptr;
            if (s_hint_label) {
                lv_label_set_text(s_hint_label, s_default_hint);
            }
        }, 1200, nullptr);
        if (s_volume_timer) lv_timer_set_repeat_count(s_volume_timer, 1);
    }
    bsp_lvgl_unlock();
}

void ui_clear_text() {
    if (!begin_update(200)) return;
    s_ai_reply_buffer.clear();
    s_user_text.clear();
    lv_label_set_text(s_user_text_label, s_user_text.c_str());
    lv_obj_add_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_ai_text_label, "");
    lv_obj_add_flag(s_ai_text_label, LV_OBJ_FLAG_HIDDEN);
    s_text_expanded = false;
    show_companion(true);
    lv_obj_scroll_to_y(s_msg_container, 0, LV_ANIM_OFF);
    bsp_lvgl_unlock();
}

} // namespace xiaozhi
