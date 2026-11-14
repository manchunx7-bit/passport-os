// NetEase music screens: message, QR login, playlist/track lists, player.
#include "apps/netease/netease_ui.h"

#include "app_fonts.h"
#include "apps/netease/netease_lyrics.h"
#include "apps/netease/netease_model.h"
#include "apps/netease/netease_player.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "display_text.h"
#include "launcher/app_registry.h"
#include "ssid_text.h"
#include "ui_pixel.h"
#include "wifi_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace netease {

namespace {

constexpr int kRowY = 56, kRowH = 27, kRowStep = 29;

lv_obj_t *s_screen = nullptr;
lv_timer_t *s_timer = nullptr;
const Lyrics *s_lyrics = nullptr;
lv_obj_t *s_wifi_bars[3]{};
ui_pixel_battery_t s_battery{};
uint32_t s_status_at = 0;
lv_obj_t *s_footer[2]{};

lv_obj_t *s_list_group, *s_message_group, *s_login_group, *s_player_group;
lv_obj_t *s_sub;
lv_obj_t *s_rows[kVisibleRows], *s_row_text[kVisibleRows];
lv_obj_t *s_message;
lv_obj_t *s_qr = nullptr, *s_login_hint, *s_login_status;
uint32_t s_qr_hash = 0;
lv_obj_t *s_title, *s_artist, *s_lyric[3], *s_bar_fill, *s_time, *s_state, *s_note;
uint32_t s_seen_revision = UINT32_MAX;
uint32_t s_seen_second = UINT32_MAX;
Page s_seen_page = Page::Message;
lv_obj_t *s_meter = nullptr;
uint8_t s_meter_value[kMeterBars]{};
uint32_t s_text_hash[16]{};

// Only the visible values cross the mutex; never copy the 7.6 KiB model or
// touch fonts/LVGL while holding the audio task's state lock.
struct Snapshot {
    Page page;
    char subtitle[96]{};
    char content[176]{};
    char status[48]{};
    char foot[64]{};
    char rows[kVisibleRows][128]{};
    uint8_t row_count = 0, selected = 0;
    int8_t playing = -1;
    Song song{};
    char lyrics[3][Lyrics::kLineBytes + 1]{};
    char note[40]{};
    uint32_t elapsed = 0;
};
static_assert(sizeof(Snapshot) <= 2048, "UI snapshot must fit the LVGL stack");

// Song names may hold any Unicode: drop emoji, fold full-width, '?' for glyphs
// the 16 px font lacks.
std::string shown(const char *text) {
    return passport::ssid_display(passport::fold_display_text(text), &buddy_font_16);
}

lv_obj_t *group(lv_obj_t *parent) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_pos(g, 0, 27);
    lv_obj_set_size(g, 240, 244);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    return g;
}

lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_text_align_t align,
               lv_label_long_mode_t mode = LV_LABEL_LONG_MODE_DOTS) {
    lv_obj_t *l = ui_pixel_label(parent, "", &buddy_font_16, color);
    lv_obj_set_pos(l, x, y - 27);  // y is in screen coordinates
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y - 27);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

void show_only(lv_obj_t *visible) {
    for (lv_obj_t *g : {s_list_group, s_message_group, s_login_group, s_player_group}) {
        if (g == visible) lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    }
}

void footer(const char *first, const char *second) {
    ui_pixel_label_set_text(s_footer[0], first);
    ui_pixel_label_set_text(s_footer[1], second);
}

void status_bar(uint32_t now) {
    if (now - s_status_at < 5000 && s_status_at) return;
    s_status_at = now;
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    auto &wifi = WifiManager::GetInstance();
    const bool ok = wifi.IsConnected();
    const int rssi = ok ? wifi.GetRssi() : -127;
    ui_pixel_wifi_bars_set(s_wifi_bars, !ok ? 0 : rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1, ok ? UI_TEXT : UI_TEXT_DIM);
}

void shown_text(lv_obj_t *label, const char *raw, size_t slot) {
    uint32_t hash = 2166136261u;
    for (const char *p = raw; *p; ++p) hash = (hash ^ uint8_t(*p)) * 16777619u;
    if (s_text_hash[slot] == hash) return;
    s_text_hash[slot] = hash;
    ui_pixel_label_set_text(label, shown(raw).c_str());
}

void render_rows(const Snapshot &v) {
    for (size_t r = 0; r < kVisibleRows; ++r) {
        if (r >= v.row_count) { lv_obj_add_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
        const bool active = r == v.selected;
        ui_pixel_bg_color(s_rows[r], active ? UI_SURFACE_ALT : UI_SURFACE);
        const uint32_t border = active ? UI_AMBER : UI_BORDER;
        if (!lv_color_eq(lv_obj_get_style_border_color(s_rows[r], LV_PART_MAIN), lv_color_hex(border)))
            lv_obj_set_style_border_color(s_rows[r], lv_color_hex(border), 0);
        shown_text(s_row_text[r], v.rows[r], r);
        ui_pixel_text_color(s_row_text[r], int(r) == v.playing ? UI_ACCENT : active ? UI_TEXT : UI_TEXT_DIM);
    }
}

void snapshot(Model &m, Snapshot &v, uint32_t now) {
    v.page = m.page_id;
    if (m.page_id == Page::Player) {
        v.song = m.now;
        v.elapsed = player_elapsed_ms();
        if (m.volume_notice && static_cast<int32_t>(now - m.volume_until_ms) > 0) m.volume_notice = false;
        if (m.volume_notice) std::snprintf(v.status, sizeof(v.status), "音量 %u%%", unsigned(bsp_audio_get_master_volume()));
        else std::snprintf(v.status, sizeof(v.status), "%s", m.session ? (player_buffering() ? "缓冲中" : "播放中") : m.paused ? "已暂停" : "准备中");
        std::snprintf(v.note, sizeof(v.note), "%s", m.player_note);
        if (m.lyrics_ready && s_lyrics->count()) {
            const int cur = s_lyrics->line_at(v.elapsed + 300);
            // LRC often inserts blank timestamps between phrases. Keep the last
            // sung line in focus instead of flashing an empty lyric panel.
            int focus = cur;
            for (; focus >= 0; --focus) {
                s_lyrics->text(focus, v.lyrics[1], sizeof(v.lyrics[1]));
                if (v.lyrics[1][0]) break;
            }
            for (int previous = focus - 1; previous >= 0; --previous) {
                s_lyrics->text(previous, v.lyrics[0], sizeof(v.lyrics[0]));
                if (v.lyrics[0][0]) break;
            }
            for (size_t next = cur + 1; next < s_lyrics->count(); ++next) {
                s_lyrics->text(next, v.lyrics[2], sizeof(v.lyrics[2]));
                if (v.lyrics[2][0]) break;
            }
        } else if (m.lyrics_ready) std::snprintf(v.lyrics[1], sizeof(v.lyrics[1]), "纯音乐或暂无歌词");
        return;
    }
    if (m.page_id == Page::Login) {
        std::snprintf(v.content, sizeof(v.content), "%s", m.qr_url);
        std::snprintf(v.status, sizeof(v.status), "%s", m.login_status);
        return;
    }
    if (m.page_id == Page::Message) {
        std::snprintf(v.content, sizeof(v.content), "%s", m.message);
        std::snprintf(v.foot, sizeof(v.foot), "%s", m.can_retry ? "确定重试" : m.list_count ? "确定返回歌单" : "");
        return;
    }
    const bool home = m.page_id == Page::Lists;
    const size_t total = home ? m.list_count + 1u : m.track_count + 1u;
    const size_t selected = home ? m.list_selected : m.track_selected;
    const size_t first = total <= kVisibleRows ? 0 : std::min(selected > 2 ? selected - 2 : size_t(0), total - kVisibleRows);
    v.row_count = static_cast<uint8_t>(std::min(kVisibleRows, total));
    v.selected = static_cast<uint8_t>(selected - first);
    if (home) std::snprintf(v.subtitle, sizeof(v.subtitle), "音乐首页 · %s", m.logged_in ? m.nickname : "未登录");
    else std::snprintf(v.subtitle, sizeof(v.subtitle), "%s %u/%u", m.open_list >= 0 ? m.lists[m.open_list].name : "", unsigned(m.track_selected), unsigned(m.track_count));
    for (size_t r = 0; r < v.row_count; ++r) {
        const size_t index = first + r;
        if (home) {
            if (index < m.list_count) std::snprintf(v.rows[r], sizeof(v.rows[r]), "%s%s", m.lists[index].name,
                !m.logged_in && m.lists[index].kind != PlaylistKind::Playlist ? " · 登录" : "");
            else std::snprintf(v.rows[r], sizeof(v.rows[r]), "%s", m.logged_in ? "退出登录" : "扫码登录");
        } else if (!index) std::snprintf(v.rows[r], sizeof(v.rows[r]), "返回歌单");
        else {
            const size_t song = index - 1;
            if (int(song) == m.now_index) v.playing = static_cast<int8_t>(r);
            if (!m.page_count || song < m.page_start || song >= size_t(m.page_start) + m.page_count)
                std::snprintf(v.rows[r], sizeof(v.rows[r]), "…");
            else {
                const Song &t = m.page[song - m.page_start];
                std::snprintf(v.rows[r], sizeof(v.rows[r]), "%u %s%s%s", unsigned(index), t.name[0] ? t.name : "未知歌曲", t.artist[0] ? " - " : "", t.artist);
            }
        }
    }
}

void clock_text(char *out, size_t n, uint32_t ms) {
    const unsigned s = ms / 1000;
    std::snprintf(out, n, "%02u:%02u", s / 60, s % 60);
}

void render_player(const Snapshot &v) {
    shown_text(s_title, v.song.name[0] ? v.song.name : "正在准备", 8);
    shown_text(s_artist, v.song.artist, 9);
    char a[12], b[12], line[32];
    clock_text(a, sizeof(a), v.elapsed);
    clock_text(b, sizeof(b), v.song.duration_ms);
    std::snprintf(line, sizeof(line), "%s / %s", a, b);
    ui_pixel_label_set_text(s_time, line);
    const int width = v.song.duration_ms ? static_cast<int>(216ULL * std::min(v.elapsed, v.song.duration_ms) / v.song.duration_ms) : 0;
    lv_obj_set_width(s_bar_fill, std::max(1, width));
    ui_pixel_label_set_text(s_state, v.status);
    shown_text(s_note, v.note, 13);
    for (int i = 0; i < 3; ++i) shown_text(s_lyric[i], v.lyrics[i], 10 + i);
    if (v.note[0]) lv_obj_add_flag(s_lyric[2], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_lyric[2], LV_OBJ_FLAG_HIDDEN);
    footer("上下切歌 · 确定暂停/继续", "长按 上下音量 确定返回");
}

void draw_meter(lv_event_t *event) {
    lv_area_t area;
    lv_obj_get_coords(s_meter, &area);
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_opa = LV_OPA_COVER;
    for (size_t i = 0; i < kMeterBars; ++i) {
        const uint8_t value = s_meter_value[i];
        const int height = 2 + value * 20 / 100;
        ink.bg_color = lv_color_hex(value > 85 ? UI_RED : value > 62 ? UI_AMBER : value ? UI_GRASS : UI_BORDER);
        const int x = area.x1 + 2 + int(i) * 12;
        const lv_area_t bar = {x, area.y2 - height + 1, x + 7, area.y2};
        lv_draw_rect(lv_event_get_layer(event), &ink, &bar);
    }
}

void render_meter() {
    if (s_seen_page != Page::Player) return;
    uint8_t levels[kMeterBars];
    player_audio_levels(levels);
    const bool silent = player_buffering();
    bool changed = false;
    for (size_t i = 0; i < kMeterBars; ++i) {
        const auto target = silent ? 0 : levels[i];
        const auto value = static_cast<uint8_t>(std::max(int(target), int(s_meter_value[i]) - 8));
        changed |= value != s_meter_value[i];
        s_meter_value[i] = value;
    }
    // One 216x22 region; no per-bar layout, style churn or object allocation.
    if (changed) lv_obj_invalidate(s_meter);
}

void render_login(const Snapshot &v) {
    uint32_t hash = 2166136261u;
    for (const char *p = v.content; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
    if (v.content[0] && (!s_qr || hash != s_qr_hash)) {
        if (!s_qr) {
            s_qr = lv_qrcode_create(s_login_group);
            lv_qrcode_set_size(s_qr, 136);
            // Black on white for any theme: scanners need the contrast.
            lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
            lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
            lv_qrcode_set_quiet_zone(s_qr, true);
            lv_obj_set_pos(s_qr, (240 - 136) / 2, 36 - 27);
        }
        if (lv_qrcode_update(s_qr, v.content, std::strlen(v.content)) == LV_RESULT_OK) s_qr_hash = hash;
    }
    ui_pixel_label_set_text(s_login_status, v.status);
    footer("确定跳过，先听排行榜", "长按确定返回");
}

void drop_qr() {
    if (!s_qr) return;
    lv_obj_delete(s_qr);
    s_qr = nullptr;
    s_qr_hash = 0;
}

void render() {
    if (!s_screen || !app_registry_screen_is_on()) return;
    const uint32_t now = lv_tick_get();
    status_bar(now);
    if (!model_lock(5)) return;
    Model &m = model();
    const uint32_t second = now / 250;
    const bool changed = m.revision != s_seen_revision || m.page_id != s_seen_page ||
                         (m.page_id == Page::Player && second != s_seen_second);
    if (!changed) { model_unlock(); render_meter(); return; }
    const bool page_changed = m.page_id != s_seen_page || s_seen_revision == UINT32_MAX;
    s_seen_revision = m.revision;
    s_seen_second = second;
    s_seen_page = m.page_id;
    Snapshot v;
    snapshot(m, v, now);
    model_unlock();
    if (page_changed) {
        if (v.page != Page::Login) drop_qr();
        show_only(v.page == Page::Player ? s_player_group : v.page == Page::Login ? s_login_group :
                  v.page == Page::Message ? s_message_group : s_list_group);
    }
    switch (v.page) {
    case Page::Message:
        ui_pixel_label_set_text(s_message, v.content);
        footer(v.foot, "长按确定返回");
        break;
    case Page::Login: render_login(v); break;
    case Page::Lists:
    case Page::Tracks:
        shown_text(s_sub, v.subtitle, 7);
        render_rows(v);
        if (v.page == Page::Lists) footer("上下选择 · 确定打开", "长按确定退出音乐");
        else footer("确定播放 · 长按下键翻页", "长按确定返回音乐首页");
        break;
    case Page::Player: render_player(v); break;
    }
    render_meter();
}

void tick(lv_timer_t *timer) {
    lv_timer_set_period(timer, app_registry_screen_is_on() ? 50 : 1000);
    render();
}

void build_common(const char *title) {
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, title, &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    s_footer[0] = ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
    s_footer[1] = lv_obj_get_child(s_screen, lv_obj_get_index(s_footer[0]) + 1);
    s_status_at = 0;
}

}  // namespace

void ui_create(const Lyrics *lyrics) {
    if (!bsp_lvgl_lock(1000)) return;
    s_lyrics = lyrics;
    build_common("音乐");

    s_list_group = group(s_screen);
    s_sub = text(s_list_group, 12, 32, 216, 20, UI_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    for (size_t r = 0; r < kVisibleRows; ++r) {
        s_rows[r] = box(s_list_group, 12, kRowY + static_cast<int>(r) * kRowStep, 216, kRowH, UI_SURFACE);
        lv_obj_set_style_border_width(s_rows[r], 1, 0);
        s_row_text[r] = ui_pixel_label(s_rows[r], "", &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(s_row_text[r], 8, 3);
        lv_obj_set_size(s_row_text[r], 200, 20);
        lv_label_set_long_mode(s_row_text[r], LV_LABEL_LONG_MODE_DOTS);
    }

    s_message_group = group(s_screen);
    s_message = text(s_message_group, 12, 120, 216, 64, UI_TEXT, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_WRAP);

    s_login_group = group(s_screen);
    s_login_hint = text(s_login_group, 12, 182, 216, 20, UI_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_login_hint, "用网易云音乐 App 扫码登录");
    s_login_status = text(s_login_group, 12, 206, 216, 20, UI_ACCENT, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *privacy = text(s_login_group, 12, 236, 216, 20, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(privacy, "登录信息只保存在本机");

    s_player_group = group(s_screen);
    s_title = text(s_player_group, 12, 38, 216, 22, UI_ACCENT, LV_TEXT_ALIGN_CENTER);
    s_artist = text(s_player_group, 12, 64, 216, 20, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    box(s_player_group, 108, 88, 24, 1, UI_BORDER);
    s_lyric[0] = text(s_player_group, 12, 100, 216, 20, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    box(s_player_group, 12, 124, 216, 44, UI_SURFACE);
    s_lyric[1] = text(s_player_group, 18, 126, 204, 40, UI_TEXT, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(s_lyric[1], LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_lyric[1], 40, 0);
    lv_obj_align(s_lyric[1], LV_ALIGN_CENTER, 0, -3);
    s_lyric[2] = text(s_player_group, 12, 176, 216, 20, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    s_note = text(s_player_group, 12, 176, 216, 20, UI_WARNING, LV_TEXT_ALIGN_CENTER);
    s_meter = box(s_player_group, 12, 204, 216, 22, UI_BG);
    std::memset(s_meter_value, 0, sizeof(s_meter_value));
    lv_obj_add_event_cb(s_meter, draw_meter, LV_EVENT_DRAW_MAIN, nullptr);
    box(s_player_group, 12, 238, 216, 3, UI_BORDER);
    s_bar_fill = box(s_player_group, 12, 238, 1, 3, UI_ACCENT);
    s_time = text(s_player_group, 12, 248, 130, 20, UI_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_14, 0);
    s_state = text(s_player_group, 142, 246, 86, 20, UI_TEXT, LV_TEXT_ALIGN_RIGHT);
    std::memset(s_text_hash, 0, sizeof(s_text_hash));

    s_seen_revision = UINT32_MAX;
    s_seen_page = Page::Message;
    render();
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 50, nullptr);
    bsp_lvgl_unlock();
}

void ui_show_unavailable(const char *message) {
    if (!bsp_lvgl_lock(1000)) return;
    if (!s_screen) {
        build_common("音乐");
        lv_obj_t *l = ui_pixel_label(s_screen, message, &buddy_font_16, UI_TEXT);
        lv_obj_set_pos(l, 12, 140);
        lv_obj_set_size(l, 216, 44);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        lv_screen_load(s_screen);
    }
    bsp_lvgl_unlock();
}

void ui_destroy() {
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_qr = nullptr;  // child of the screen
    s_qr_hash = 0;
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    for (auto &bar : s_wifi_bars) bar = nullptr;
    s_battery = {};
    s_lyrics = nullptr;
    bsp_lvgl_unlock();
}

}  // namespace netease
