#include "podcast_catalog.h"
#include "apps/app_base.h"
#include "apps/radio/radio_player.h"
#include "launcher/app_registry.h"
#include "app_fonts.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "display_text.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ssid_text.h"
#include "ui_pixel.h"
#include "voice_intent.h"
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <string>

namespace podcast {
namespace {

enum class View { Menu, Programs, Episodes, Player };
enum class Feed : uint8_t { Hot, Recommended, Business, Story, Crosstalk, Search, Resume };

constexpr std::size_t kMenuSlots = 6;
constexpr std::size_t kListSlots = 3;
// The top bit distinguishes the 32 kbps file without changing the stored layout.
// Earlier builds stored ordinary offsets with this bit clear.
constexpr uint32_t kBackupBookmark = uint32_t{1} << 31;

struct Bookmark {
    uint32_t episode;
    uint32_t page;
    uint32_t offset;
};

struct Recent {
    uint32_t program;
    uint32_t episode;
    uint32_t page;
    uint32_t duration;
    char title[96];
};

struct Request {
    uint32_t program = 0;
    unsigned page = 1;
    Discovery discovery = Discovery::Hot;
    char query[voice_intent::kNameBytes]{};
    Page result{};
    ProgramList programs{};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
};

struct Session {
    Page page_data{};
    Bookmark bookmark{};
    Recent recent{};
    ProgramList programs{};
    View view = View::Menu;
    Feed feed = Feed::Hot;
    unsigned menu = 0;
    unsigned program = 0;
    char query[voice_intent::kNameBytes]{};
    bool autoplay = false;
    bool latest = false;
    unsigned page = 1;
    unsigned selected = 0;
    bool player_ready = false;
    bool returning = false;
    bool online = false;
    bool started = false;
    bool backup_active = false;
    bool volume_notice = false;
    uint32_t volume_until_ms = 0;
    uint32_t initial_offset = 0;
    int64_t save_at = 0;
    int64_t stop_deadline = 0;
    const char *message = nullptr;
};

Session *session;
Request *request;
std::atomic<bool> exiting{false};

lv_obj_t *screen = nullptr;
lv_timer_t *timer = nullptr;
lv_obj_t *wifi_bars[3]{};
ui_pixel_battery_t battery{};
uint32_t status_at = 0;
uint32_t last_refresh_ms = 0;
lv_obj_t *footer_primary = nullptr;
lv_obj_t *footer_secondary = nullptr;

lv_obj_t *menu_group = nullptr;
lv_obj_t *list_group = nullptr;
lv_obj_t *player_group = nullptr;

lv_obj_t *menu_count_label = nullptr;
lv_obj_t *menu_rows[kMenuSlots]{};
lv_obj_t *menu_bars[kMenuSlots]{};
lv_obj_t *menu_labels[kMenuSlots]{};

lv_obj_t *list_heading = nullptr;
lv_obj_t *list_counter = nullptr;
lv_obj_t *list_cards[kListSlots]{};
lv_obj_t *list_bars[kListSlots]{};
lv_obj_t *list_titles[kListSlots]{};
lv_obj_t *list_metas[kListSlots]{};
lv_obj_t *list_empty = nullptr;

lv_obj_t *player_program = nullptr;
lv_obj_t *player_status = nullptr;
lv_obj_t *player_title = nullptr;
lv_obj_t *player_note = nullptr;
lv_obj_t *player_meter = nullptr;
lv_obj_t *player_bar_fill = nullptr;
lv_obj_t *player_time = nullptr;
lv_obj_t *player_quality = nullptr;
uint8_t meter_value[kEpisodeMeterBars]{};

std::string shown(const char *raw) {
    return passport::ssid_display(passport::fold_display_text(raw ? raw : ""), &buddy_font_16);
}

void set_border_color(lv_obj_t *obj, uint32_t hex) {
    const lv_color_t next = lv_color_hex(hex);
    if (!lv_color_eq(lv_obj_get_style_border_color(obj, LV_PART_MAIN), next)) {
        lv_obj_set_style_border_color(obj, next, 0);
    }
}

void set_visible(lv_obj_t *obj, bool visible) {
    if (!obj) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *make_group(lv_obj_t *parent) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_pos(g, 0, 27);
    lv_obj_set_size(g, 240, 244);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    return g;
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y - 27);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                     uint32_t color, const lv_font_t *font = &buddy_font_16,
                     lv_text_align_t align = LV_TEXT_ALIGN_LEFT,
                     lv_label_long_mode_t mode = LV_LABEL_LONG_DOT) {
    lv_obj_t *l = ui_pixel_label(parent, text, font, color);
    lv_obj_set_pos(l, x, y - 27);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

Bookmark load_bookmark(uint32_t program) {
    Bookmark value{};
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READONLY, &handle) != ESP_OK) return value;
    char key[12];
    std::snprintf(key, sizeof(key), "%lu", static_cast<unsigned long>(program));
    std::size_t size = sizeof(value);
    if (nvs_get_blob(handle, key, &value, &size) != ESP_OK || size != sizeof(value) ||
        !value.episode || !value.page || value.page > 100000) value = {};
    nvs_close(handle);
    return value;
}

Recent load_recent() {
    Recent value{};
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READONLY, &handle) != ESP_OK) return value;
    std::size_t size = sizeof(value);
    if (nvs_get_blob(handle, "recent", &value, &size) != ESP_OK || size != sizeof(value) ||
        !value.program || !value.title[0]) {
        value = {};
    } else {
        value.title[sizeof(value.title) - 1] = '\0';
    }
    nvs_close(handle);
    return value;
}

void save_recent(Session &v, const Recent &next) {
    if (!next.program || !next.title[0]) return;
    if (next.program == v.recent.program && next.episode == v.recent.episode &&
        next.page == v.recent.page && next.duration == v.recent.duration &&
        std::strcmp(next.title, v.recent.title) == 0) {
        return;
    }
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, "recent", &next, sizeof(next)) == ESP_OK && nvs_commit(handle) == ESP_OK) {
        v.recent = next;
    }
    nvs_close(handle);
}

void save_progress(Session &v, EpisodePlayback playback) {
    v.save_at = esp_timer_get_time();
    if (!v.started || !v.page_data.count || v.selected >= v.page_data.count) return;
    const auto &ep = v.page_data.episodes[v.selected];
    if (playback.state == RadioPlaybackState::Finished) playback.offset = 0;
    if (playback.offset >= kBackupBookmark) return;
    const Bookmark next{ep.id, v.page,
                        playback.offset | (v.backup_active ? kBackupBookmark : 0)};
    if (next.episode != v.bookmark.episode || next.page != v.bookmark.page ||
        next.offset != v.bookmark.offset) {
        nvs_handle_t handle;
        if (nvs_open("podcast", NVS_READWRITE, &handle) == ESP_OK) {
            char key[12];
            std::snprintf(key, sizeof(key), "%lu", static_cast<unsigned long>(v.programs.items[v.program].id));
            if (nvs_set_blob(handle, key, &next, sizeof(next)) == ESP_OK && nvs_commit(handle) == ESP_OK) {
                v.bookmark = next;
            }
            nvs_close(handle);
        }
    }
    Recent recent{v.programs.items[v.program].id, ep.id, v.page, ep.duration, {}};
    std::snprintf(recent.title, sizeof(recent.title), "%s", v.programs.items[v.program].title);
    save_recent(v, recent);
}

void fetch_task(void *arg) {
    auto &job = *static_cast<Request *>(arg);
    job.ok = job.program ? fetch_page(job.program, job.page, job.cancel, job.result)
                         : discover(job.discovery, job.query, job.cancel, job.programs);
    job.done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

void play(Session &v);

bool fetch(Session &v, unsigned page = 1) {
    if (request) return false;
    if (!WifiManager::GetInstance().IsConnected()) {
        v.message = "请先连接 Wi-Fi";
        return false;
    }
    request = new (std::nothrow) Request;
    if (request) {
        if (v.view == View::Episodes) {
            request->program = v.programs.items[v.program].id;
            request->page = page;
        } else {
            switch (v.feed) {
                case Feed::Hot: request->discovery = Discovery::Hot; break;
                case Feed::Recommended: request->discovery = Discovery::Recommended; break;
                case Feed::Business: request->discovery = Discovery::Business; break;
                case Feed::Story: request->discovery = Discovery::Story; break;
                case Feed::Crosstalk: request->discovery = Discovery::Crosstalk; break;
                default: request->discovery = Discovery::Search; break;
            }
            std::memcpy(request->query, v.query, sizeof(request->query));
        }
    }
    if (!request || xTaskCreate(fetch_task, "podcast_list", 6144, request, 4, nullptr) != pdPASS) {
        delete request;
        request = nullptr;
        v.message = "内存不足，请重试";
        return false;
    }
    v.message = v.view == View::Episodes ? "正在获取单集" : "正在获取播客";
    return true;
}

void open_program(Session &v) {
    v.view = View::Episodes;
    v.bookmark = load_bookmark(v.programs.items[v.program].id);
    v.page_data = {};
    v.selected = 0;
    v.page = (!v.latest && v.bookmark.page) ? v.bookmark.page : 1;
    if (!fetch(v, v.page)) v.autoplay = false;
}

void open_feed(Session &v, Feed feed) {
    v.view = View::Programs;
    v.feed = feed;
    v.program = 0;
    v.programs = {};
    v.message = nullptr;
    if (!fetch(v)) v.autoplay = false;
}

void open_resume(Session &v) {
    if (!v.recent.program) return;
    v.feed = Feed::Resume;
    v.program = 0;
    v.programs = {};
    v.programs.count = 1;
    v.programs.items[0].id = v.recent.program;
    std::snprintf(v.programs.items[0].title, sizeof(v.programs.items[0].title), "%s", v.recent.title);
    v.autoplay = true;
    v.latest = false;
    open_program(v);
}

unsigned menu_count(const Session &v) {
    return v.recent.program ? 6u : 5u;
}

Feed menu_feed(const Session &v, unsigned index) {
    const unsigned base = v.recent.program ? (index ? index - 1 : 0) : index;
    switch (base) {
        case 0: return Feed::Hot;
        case 1: return Feed::Recommended;
        case 2: return Feed::Business;
        case 3: return Feed::Story;
        default: return Feed::Crosstalk;
    }
}

bool finish_fetch(Session &v) {
    if (!request || !request->done.load(std::memory_order_acquire)) return false;
    // Clear the old request before actions that might queue a follow-up page.
    auto *done = request;
    request = nullptr;
    if (done->cancel.load()) { delete done; return false; }
    if (v.view == View::Programs && !done->program) {
        if (!done->ok) v.message = "节目获取失败，按确定重试";
        else {
            v.programs = done->programs;
            v.program = 0;
            v.message = v.programs.count ? nullptr : "没有找到节目";
        }
        delete done;
        if (!v.autoplay || !v.programs.count) { v.autoplay = false; return true; }
        if (v.feed == Feed::Search) {
            const auto found = match_program(v.programs, v.query);
            if (found == v.programs.count) {
                v.autoplay = false;
                v.message = "未找到同名节目，请选择";
                return true;
            }
            v.program = found;
        }
        open_program(v);
        return true;
    }
    if (v.view != View::Episodes || !done->program) { delete done; return true; }
    const auto page = done->page;
    const bool fetched = done->ok;
    const bool has_episodes = fetched && done->result.count != 0;
    if (has_episodes) {
        v.page = page;
        v.page_data = done->result;
        v.selected = 0;
        v.message = nullptr;
    } else v.message = fetched ? "此页没有可显示的单集" : "列表获取失败，按确定重试";
    delete done;

    unsigned bookmarked = v.page_data.count;
    if (has_episodes && !v.latest) {
        for (unsigned i = 0; i < v.page_data.count; ++i) {
            if (v.page_data.episodes[i].id == v.bookmark.episode) bookmarked = v.selected = i;
        }
    }
    if (!v.autoplay) return true;
    if (fetched && bookmarked == v.page_data.count && page > 1) {
        v.page = 1;
        if (fetch(v, 1)) return true;
    }
    if (has_episodes) {
        for (unsigned i = 0; i < v.page_data.count; ++i) {
            const auto index = (v.selected + i) % v.page_data.count;
            const auto &episode = v.page_data.episodes[index];
            if (episode.paid || !*episode.url) continue;
            v.autoplay = false;
            v.selected = index;
            v.view = View::Player;
            v.started = false;
            v.volume_notice = false;
            std::memset(meter_value, 0, sizeof(meter_value));
            play(v);
            return true;
        }
    }
    // Try the next few recommendations when the first has no playable episode.
    if (v.feed != Feed::Search && v.feed != Feed::Resume &&
        v.program + 1 < std::min(v.programs.count, 3u)) {
        ++v.program;
        open_program(v);
        return true;
    }
    v.autoplay = false;
    if (fetched) v.message = "暂无可播放单集";
    return true;
}

const char *status_text(RadioPlaybackState state) {
    switch (state) {
        case RadioPlaybackState::Stopped: return "已暂停";
        case RadioPlaybackState::Connecting: return "连接中";
        case RadioPlaybackState::Buffering: return "缓冲中";
        case RadioPlaybackState::Playing: return "播放中";
        case RadioPlaybackState::Reconnecting: return "重连中";
        case RadioPlaybackState::Error: return "播放失败";
        case RadioPlaybackState::Finished: return "已播完";
    }
    return "";
}

uint32_t status_color(RadioPlaybackState state) {
    switch (state) {
        case RadioPlaybackState::Playing: return ui_theme_is_studio() ? UI_SUCCESS : UI_ACCENT;
        case RadioPlaybackState::Connecting:
        case RadioPlaybackState::Buffering:
        case RadioPlaybackState::Reconnecting: return UI_WARNING;
        case RadioPlaybackState::Error: return UI_DANGER;
        default: return UI_TEXT_DIM;
    }
}

void format_plays(uint32_t plays, char *out, std::size_t capacity) {
    if (plays >= 100000000u) {
        std::snprintf(out, capacity, "%lu.%lu亿次播放",
                      static_cast<unsigned long>(plays / 100000000u),
                      static_cast<unsigned long>((plays / 10000000u) % 10u));
    } else if (plays >= 10000u) {
        std::snprintf(out, capacity, "%lu万次播放", static_cast<unsigned long>(plays / 10000u));
    } else if (plays > 0u) {
        std::snprintf(out, capacity, "%lu次播放", static_cast<unsigned long>(plays));
    } else {
        std::snprintf(out, capacity, "精选节目");
    }
}

void format_episode_meta(const Episode &ep, bool resume, char *out, std::size_t capacity) {
    char dur[24];
    if (ep.duration) {
        std::snprintf(dur, sizeof(dur), "%02u:%02u",
                      unsigned(ep.duration / 60), unsigned(ep.duration % 60));
    } else {
        std::snprintf(dur, sizeof(dur), "%s", ep.paid ? "付费单集" : "免费单集");
    }
    const char *badge = (ep.duration && ep.paid) ? " · 需付费" : resume ? " · 继续收听" : "";
    std::snprintf(out, capacity, "%s%s", dur, badge);
}

std::string feed_heading(const Session &v) {
    switch (v.feed) {
        case Feed::Hot: return "热门播客";
        case Feed::Recommended: return std::string("今日主题 · ") + recommended_theme_name();
        case Feed::Business: return "商业资讯";
        case Feed::Story: return "故事人文";
        case Feed::Crosstalk: return "相声评书";
        case Feed::Resume: return "继续收听";
        case Feed::Search: return shown(v.query);
    }
    return "播客";
}

void update_status_bar(bool force) {
    if (!screen) return;
    const uint32_t now = lv_tick_get();
    if (!force && status_at && now - status_at < 5000) return;
    status_at = now ? now : 1;
    ui_pixel_battery_set(&battery, bsp_battery_soc());
    auto &wifi = WifiManager::GetInstance();
    const bool ok = wifi.IsConnected();
    const int rssi = ok ? wifi.GetRssi() : -127;
    ui_pixel_wifi_bars_set(wifi_bars, !ok ? 0 : rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1,
                           ok ? UI_TEXT : UI_TEXT_DIM);
}

void draw_meter(lv_event_t *event) {
    lv_area_t area;
    lv_obj_get_coords(player_meter, &area);
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_opa = LV_OPA_COVER;
    const bool studio = ui_theme_is_studio();
    if (studio) ink.radius = 2;
    for (std::size_t i = 0; i < kEpisodeMeterBars; ++i) {
        const uint8_t value = meter_value[i];
        const int height = 2 + value * 22 / 100;
        ink.bg_color = lv_color_hex(studio
            ? ui_studio_meter_color(static_cast<unsigned>(i), static_cast<unsigned>(kEpisodeMeterBars), value)
            : (value > 85 ? UI_RED : value > 62 ? UI_AMBER : value ? UI_GRASS : UI_BORDER));
        const int x = area.x1 + 2 + static_cast<int>(i) * 12;
        const lv_area_t bar = {x, area.y2 - height + 1, x + 7, area.y2};
        lv_draw_rect(lv_event_get_layer(event), &ink, &bar);
    }
}

void render_meter(const Session &v) {
    if (v.view != View::Player || !player_meter) return;
    uint8_t levels[kEpisodeMeterBars]{};
    if (v.player_ready) radio_player_episode_levels(levels);
    bool changed = false;
    for (std::size_t i = 0; i < kEpisodeMeterBars; ++i) {
        const auto value = static_cast<uint8_t>(std::max(int(levels[i]), int(meter_value[i]) - 8));
        changed |= value != meter_value[i];
        meter_value[i] = value;
    }
    if (changed) lv_obj_invalidate(player_meter);
}

void set_footer(const char *first, const char *second) {
    if (footer_primary) ui_pixel_label_set_text(footer_primary, first);
    if (footer_secondary) ui_pixel_label_set_text(footer_secondary, second);
}

void show_group(lv_obj_t *active) {
    for (lv_obj_t *g : {menu_group, list_group, player_group}) set_visible(g, g == active);
}

void refresh() {
    if (!session) {
        show_group(list_group);
        ui_pixel_label_set_text(list_heading, "播客");
        ui_pixel_label_set_text(list_counter, "");
        for (std::size_t r = 0; r < kListSlots; ++r) set_visible(list_cards[r], false);
        set_visible(list_empty, true);
        ui_pixel_label_set_text(list_empty, "内存不足，请退出重试");
        set_footer("长按确定返回", "");
        return;
    }
    auto &v = *session;
    if (v.view == View::Menu) {
        show_group(menu_group);
        const unsigned count = menu_count(v);
        if (v.menu >= count) v.menu = count - 1;
        char counter[16];
        std::snprintf(counter, sizeof(counter), "%02u / %02u", v.menu + 1, count);
        ui_pixel_label_set_text(menu_count_label, counter);

        const std::string theme_line = std::string("今日主题 · ") + recommended_theme_name();
        const bool has_resume = v.recent.program != 0;
        const std::string resume_line = has_resume ? std::string("继续 · ") + shown(v.recent.title) : "";
        for (std::size_t i = 0; i < kMenuSlots; ++i) {
            if (i >= count) { set_visible(menu_rows[i], false); continue; }
            set_visible(menu_rows[i], true);
            const bool active = i == v.menu;
            const bool is_resume = has_resume && i == 0;
            ui_pixel_bg_color(menu_rows[i], active ? UI_SURFACE_ALT : UI_SURFACE);
            set_border_color(menu_rows[i], active ? (ui_theme_is_studio() ? UI_ACCENT : UI_AMBER) : UI_BORDER);
            set_visible(menu_bars[i], active);
            ui_pixel_text_color(menu_labels[i], active ? UI_TEXT : is_resume ? UI_ACCENT : UI_TEXT_DIM);

            const char *item_text = nullptr;
            if (is_resume) item_text = resume_line.c_str();
            else {
                const std::size_t base = has_resume ? i - 1 : i;
                static constexpr const char *kBaseLabels[]{"热门播客", nullptr, "商业资讯", "故事人文", "相声评书"};
                item_text = base == 1 ? theme_line.c_str() : kBaseLabels[base];
            }
            ui_pixel_label_set_text(menu_labels[i], item_text);
        }
        set_footer("上下选择 · 确定打开", "双击小智点播 · 长按返回");
        return;
    }

    if (v.view == View::Programs || v.view == View::Episodes) {
        show_group(list_group);
        const bool is_programs = v.view == View::Programs;
        if (is_programs) {
            ui_pixel_label_set_text(list_heading, feed_heading(v).c_str());
            if (v.programs.count) {
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), "%02u / %02u", v.program + 1, v.programs.count);
                ui_pixel_label_set_text(list_counter, cnt);
            } else {
                ui_pixel_label_set_text(list_counter, "");
            }
        } else {
            const std::string prog_title = v.program < v.programs.count
                ? shown(v.programs.items[v.program].title) : std::string("播客");
            ui_pixel_label_set_text(list_heading, prog_title.c_str());
            char cnt[24];
            std::snprintf(cnt, sizeof(cnt), "第 %u 页", v.page);
            ui_pixel_label_set_text(list_counter, cnt);
        }

        const unsigned total = is_programs ? v.programs.count : v.page_data.count;
        const unsigned first = is_programs
            ? (total <= kListSlots ? 0u : std::min(v.program > 1 ? v.program - 1 : 0u, total - unsigned(kListSlots)))
            : 0u;
        const unsigned visible = total > first ? std::min<unsigned>(total - first, kListSlots) : 0u;
        for (std::size_t r = 0; r < kListSlots; ++r) {
            if (r >= visible) { set_visible(list_cards[r], false); continue; }
            set_visible(list_cards[r], true);
            const unsigned idx = first + static_cast<unsigned>(r);
            const bool active = idx == (is_programs ? v.program : v.selected);
            ui_pixel_bg_color(list_cards[r], active ? UI_SURFACE_ALT : UI_SURFACE);
            set_border_color(list_cards[r], active ? (ui_theme_is_studio() ? UI_ACCENT : UI_AMBER) : UI_BORDER);
            set_visible(list_bars[r], active);
            ui_pixel_text_color(list_titles[r], active ? UI_TEXT : UI_TEXT_DIM);
            ui_pixel_text_color(list_metas[r], active ? UI_ACCENT : UI_TEXT_DIM);

            char meta[64];
            if (is_programs) {
                const auto &item = v.programs.items[idx];
                ui_pixel_label_set_text(list_titles[r], shown(item.title).c_str());
                format_plays(item.plays, meta, sizeof(meta));
                ui_pixel_label_set_text(list_metas[r], meta);
            } else {
                const auto &ep = v.page_data.episodes[idx];
                ui_pixel_label_set_text(list_titles[r], shown(ep.title).c_str());
                const bool is_resume = ep.id == v.bookmark.episode && (v.bookmark.offset & ~kBackupBookmark) != 0;
                format_episode_meta(ep, is_resume, meta, sizeof(meta));
                ui_pixel_label_set_text(list_metas[r], meta);
            }
        }
        set_visible(list_empty, visible == 0);
        if (visible == 0) {
            ui_pixel_label_set_text(list_empty, v.message ? v.message : "暂无内容");
        }
        if (is_programs) {
            set_footer(v.message ? v.message : "上下选择 · 确定选期", "长按确定返回");
        } else {
            set_footer(v.message ? v.message : "上下选期 · 长按上下翻页", "长按确定返回");
        }
        return;
    }

    show_group(player_group);
    const auto playback = v.player_ready ? radio_player_episode() : EpisodePlayback{};
    const auto &ep = v.page_data.episodes[v.selected];
    const std::string prog_title = v.program < v.programs.count
        ? shown(v.programs.items[v.program].title) : std::string("播客");
    ui_pixel_label_set_text(player_program, prog_title.c_str());
    ui_pixel_label_set_text(player_title, shown(ep.title).c_str());

    if (v.volume_notice && static_cast<int32_t>(lv_tick_get() - v.volume_until_ms) < 0) {
        char vol[24];
        std::snprintf(vol, sizeof(vol), "音量 %u%%", unsigned(radio_player_volume()));
        ui_pixel_label_set_text(player_status, vol);
        ui_pixel_text_color(player_status, UI_AMBER);
    } else {
        v.volume_notice = false;
        ui_pixel_label_set_text(player_status, status_text(playback.state));
        ui_pixel_text_color(player_status, status_color(playback.state));
    }

    char sleep_note[40];
    const char *note = v.message ? v.message
        : playback.state == RadioPlaybackState::Error ? "播放中断，按确定重试"
        : nullptr;
    uint32_t note_color = UI_WARNING;
    if (!note && v.stop_deadline) {
        const int64_t left = std::max<int64_t>(0, v.stop_deadline - esp_timer_get_time()) / 1000000;
        std::snprintf(sleep_note, sizeof(sleep_note), "定时 %02u:%02u 后暂停",
                      unsigned(left / 60), unsigned(left % 60));
        note = sleep_note;
        note_color = UI_ACCENT;
    }
    set_visible(player_note, note != nullptr);
    if (note) {
        ui_pixel_text_color(player_note, note_color);
        ui_pixel_label_set_text(player_note, note);
    }

    char time_buf[32];
    int bar_w = 1;
    if (playback.total && ep.duration) {
        const auto elapsed = std::min(ep.duration,
            static_cast<uint32_t>(uint64_t(playback.offset) * ep.duration / playback.total));
        std::snprintf(time_buf, sizeof(time_buf), "%02u:%02u / %02u:%02u",
                      unsigned(elapsed / 60), unsigned(elapsed % 60),
                      unsigned(ep.duration / 60), unsigned(ep.duration % 60));
        bar_w = std::max(1, static_cast<int>(216ULL * elapsed / ep.duration));
    } else if (ep.duration) {
        std::snprintf(time_buf, sizeof(time_buf), "--:-- / %02u:%02u",
                      unsigned(ep.duration / 60), unsigned(ep.duration % 60));
    } else {
        std::snprintf(time_buf, sizeof(time_buf), "00:00 / --:--");
    }
    lv_obj_set_width(player_bar_fill, bar_w);
    ui_pixel_label_set_text(player_time, time_buf);
    ui_pixel_label_set_text(player_quality, v.backup_active ? "32K" : "64K");
    set_footer("确定播放/暂停 · 上下音量", "长按确定返回单集");
}

void play(Session &v) {
    const auto &ep = v.page_data.episodes[v.selected];
    if (ep.paid || !*ep.url) { v.message = ep.paid ? "此单集需要付费" : "此单集暂无音频"; return; }
    const bool online = WifiManager::GetInstance().IsConnected();
    if (!online) { v.message = "请先连接 Wi-Fi"; return; }
    if (!v.player_ready) {
        v.player_ready = radio_player_init(true);
        if (!v.player_ready) { v.message = "音频启动失败，按确定重试"; return; }
        v.online = false;
    }
    if (!v.online) {
        v.online = true;
        radio_player_set_network(true);
    }
    if (!radio_player_wait_idle(0)) { v.message = "音频正在暂停，请稍后重试"; return; }
    constexpr uint32_t kRewindBytes = 16000;
    const bool resume_episode = !v.latest && v.bookmark.episode == ep.id;
    const uint32_t stored = resume_episode ? v.bookmark.offset : 0;
    v.backup_active = (stored & kBackupBookmark) && *ep.backup_url;
    const uint32_t at = bool(stored & kBackupBookmark) == v.backup_active ? stored & ~kBackupBookmark : 0;
    const uint32_t resume = at > kRewindBytes ? at - kRewindBytes : 0;
    if (radio_player_play_episode(v.backup_active ? ep.backup_url : ep.url, resume)) {
        v.initial_offset = resume;
        v.latest = false;
        v.started = true;
        v.save_at = esp_timer_get_time();
        v.message = nullptr;
        Recent recent{v.programs.items[v.program].id, ep.id, v.page, ep.duration, {}};
        std::snprintf(recent.title, sizeof(recent.title), "%s", v.programs.items[v.program].title);
        save_recent(v, recent);
    } else v.message = "音频启动失败，按确定重试";
}

void tick(lv_timer_t *t) {
    if (!session || exiting.load()) return;
    auto &v = *session;
    const bool screen_on = app_registry_screen_is_on();
    lv_timer_set_period(t, !screen_on ? 1000 : v.view == View::Player ? 50 : 250);

    bool dirty = finish_fetch(v);
    if (v.player_ready) {
        const bool online = WifiManager::GetInstance().IsConnected();
        if (v.online != online) {
            v.online = online;
            radio_player_set_network(online);
            dirty = true;
        }
    }
    if (v.stop_deadline && esp_timer_get_time() >= v.stop_deadline) {
        v.stop_deadline = 0;
        if (v.player_ready && radio_player_is_playing()) {
            radio_player_set_playing(false);
            if (v.started) save_progress(v, radio_player_episode());
            v.message = "定时已到，已暂停";
            dirty = true;
        }
    }
    if (v.returning && radio_player_deinit(0)) {
        save_progress(v, radio_player_episode());
        v.player_ready = v.returning = v.started = false;
        v.volume_notice = false;
        v.stop_deadline = 0;
        std::memset(meter_value, 0, sizeof(meter_value));
        v.view = View::Episodes;
        v.message = nullptr;
        dirty = true;
    } else if (v.view == View::Player && v.started) {
        const auto playback = radio_player_episode();
        const bool idle = !radio_player_is_playing() && radio_player_wait_idle(0);
        const auto &episode = v.page_data.episodes[v.selected];
        if (idle && playback.state == RadioPlaybackState::Error && playback.offset <= v.initial_offset &&
            !v.backup_active && *episode.backup_url && v.online &&
            radio_player_play_episode(episode.backup_url, 0)) {
            v.backup_active = true;
            v.initial_offset = 0;
            v.save_at = esp_timer_get_time();
            v.message = "正在连接备用音源";
            dirty = true;
        } else {
            if ((playback.state == RadioPlaybackState::Playing || playback.state == RadioPlaybackState::Error) &&
                v.message != nullptr) {
                v.message = nullptr;
                dirty = true;
            }
            if (idle || esp_timer_get_time() - v.save_at >= 60000000) save_progress(v, playback);
        }
    }
    if (!screen_on) return;
    update_status_bar(false);
    if (v.view == View::Player) render_meter(v);
    const uint32_t now = lv_tick_get();
    if (dirty || !last_refresh_ms || now - last_refresh_ms >= 250) {
        last_refresh_ms = now ? now : 1;
        refresh();
    }
}

void build_ui() {
    screen = lv_obj_create(nullptr);
    ui_pixel_background(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = ui_pixel_top_bar(screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "播客", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);

    // 1. Home menu group
    menu_group = make_group(screen);
    make_label(menu_group, "喜马拉雅随身听", 12, 34, 144, 20, UI_TEXT_DIM);
    menu_count_label = make_label(menu_group, "", 160, 36, 68, 18, UI_TEXT_DIM,
                                  &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    const bool studio = ui_theme_is_studio();
    for (std::size_t i = 0; i < kMenuSlots; ++i) {
        const int y = 58 + static_cast<int>(i) * 33;
        menu_rows[i] = make_box(menu_group, 12, y, 216, 30, UI_SURFACE);
        lv_obj_set_style_border_width(menu_rows[i], 1, 0);
        lv_obj_set_style_border_color(menu_rows[i], lv_color_hex(UI_BORDER), 0);
        if (studio) lv_obj_set_style_radius(menu_rows[i], 6, 0);
        menu_bars[i] = lv_obj_create(menu_rows[i]);
        lv_obj_remove_style_all(menu_bars[i]);
        lv_obj_set_pos(menu_bars[i], 0, studio ? 6 : 0);
        lv_obj_set_size(menu_bars[i], 3, studio ? 18 : 30);
        if (studio) lv_obj_set_style_radius(menu_bars[i], 2, 0);
        lv_obj_set_style_bg_opa(menu_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(menu_bars[i], lv_color_hex(studio ? UI_WARNING : UI_ACCENT), 0);
        menu_labels[i] = ui_pixel_label(menu_rows[i], "", &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(menu_labels[i], 10, 5);
        lv_obj_set_size(menu_labels[i], 196, 20);
        lv_label_set_long_mode(menu_labels[i], LV_LABEL_LONG_DOT);
    }

    // 2. Programs & Episodes list group
    list_group = make_group(screen);
    list_heading = make_label(list_group, "", 12, 34, 150, 20, UI_ACCENT);
    list_counter = make_label(list_group, "", 164, 34, 64, 20, UI_TEXT_DIM,
                              &buddy_font_16, LV_TEXT_ALIGN_RIGHT);
    for (std::size_t r = 0; r < kListSlots; ++r) {
        const int y = 60 + static_cast<int>(r) * 68;
        list_cards[r] = make_box(list_group, 12, y, 216, 62, UI_SURFACE);
        lv_obj_set_style_border_width(list_cards[r], 1, 0);
        lv_obj_set_style_border_color(list_cards[r], lv_color_hex(UI_BORDER), 0);
        if (studio) lv_obj_set_style_radius(list_cards[r], 8, 0);
        list_bars[r] = lv_obj_create(list_cards[r]);
        lv_obj_remove_style_all(list_bars[r]);
        lv_obj_set_pos(list_bars[r], 0, studio ? 8 : 0);
        lv_obj_set_size(list_bars[r], 3, studio ? 46 : 62);
        if (studio) lv_obj_set_style_radius(list_bars[r], 2, 0);
        lv_obj_set_style_bg_opa(list_bars[r], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(list_bars[r], lv_color_hex(studio ? UI_WARNING : UI_ACCENT), 0);
        list_titles[r] = ui_pixel_label(list_cards[r], "", &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(list_titles[r], 10, 7);
        lv_obj_set_size(list_titles[r], 196, 22);
        lv_label_set_long_mode(list_titles[r], LV_LABEL_LONG_DOT);
        list_metas[r] = ui_pixel_label(list_cards[r], "", &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(list_metas[r], 10, 35);
        lv_obj_set_size(list_metas[r], 196, 20);
        lv_label_set_long_mode(list_metas[r], LV_LABEL_LONG_DOT);
    }
    list_empty = make_label(list_group, "", 12, 132, 216, 48, UI_TEXT_DIM,
                            &buddy_font_16, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);

    // 3. Dedicated Player group
    player_group = make_group(screen);
    player_program = make_label(player_group, "", 12, 36, 124, 20, UI_TEXT_DIM);
    player_status = make_label(player_group, "", 140, 36, 88, 20, UI_TEXT_DIM,
                               &buddy_font_16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *title_card = make_box(player_group, 12, 64, 216, 96, UI_SURFACE);
    lv_obj_set_style_border_width(title_card, 1, 0);
    lv_obj_set_style_border_color(title_card, lv_color_hex(UI_BORDER), 0);
    if (studio) lv_obj_set_style_radius(title_card, 8, 0);
    lv_obj_t *corner = lv_obj_create(title_card);
    lv_obj_remove_style_all(corner);
    lv_obj_set_pos(corner, 0, studio ? 10 : 0);
    lv_obj_set_size(corner, 3, studio ? 20 : 18);
    if (studio) lv_obj_set_style_radius(corner, 2, 0);
    lv_obj_set_style_bg_opa(corner, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(corner, lv_color_hex(studio ? UI_WARNING : UI_ACCENT), 0);
    player_title = ui_pixel_label(title_card, "", &buddy_font_16, UI_TEXT);
    lv_obj_set_pos(player_title, 12, 10);
    lv_obj_set_size(player_title, 192, 76);
    lv_label_set_long_mode(player_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(player_title, 4, 0);

    player_note = make_label(player_group, "", 12, 166, 216, 20, UI_WARNING,
                             &buddy_font_16, LV_TEXT_ALIGN_CENTER);
    player_meter = make_box(player_group, 12, 194, 216, 24, UI_BG);
    std::memset(meter_value, 0, sizeof(meter_value));
    lv_obj_add_event_cb(player_meter, draw_meter, LV_EVENT_DRAW_MAIN, nullptr);

    lv_obj_t *track = make_box(player_group, 12, 228, 216, studio ? 4 : 3,
                               studio ? UI_SURFACE_ALT : UI_BORDER);
    player_bar_fill = make_box(player_group, 12, 228, 1, studio ? 4 : 3, UI_ACCENT);
    if (studio) {
        lv_obj_set_style_radius(track, 2, 0);
        lv_obj_set_style_radius(player_bar_fill, 2, 0);
    }
    player_time = make_label(player_group, "", 12, 238, 146, 20, UI_TEXT_DIM,
                             &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    player_quality = make_label(player_group, "", 160, 238, 68, 20, UI_TEXT_DIM,
                                &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);

    footer_primary = ui_pixel_footer(screen, "", "", &buddy_font_16);
    footer_secondary = lv_obj_get_child(screen, lv_obj_get_index(footer_primary) + 1);
    status_at = 0;
    last_refresh_ms = 0;
}

void start() {
    if (!bsp_lvgl_lock(1000)) return;
    exiting.store(false);
    session = new (std::nothrow) Session;
    const auto intent = voice_intent::take(APP_ID_PODCAST);
    if (session) {
        auto &v = *session;
        v.recent = load_recent();
        if (intent.source != voice_intent::Source::None) {
            v.autoplay = true;
            v.latest = intent.podcast_latest;
            if (intent.stop_minutes) {
                v.stop_deadline = esp_timer_get_time() + static_cast<int64_t>(intent.stop_minutes) * 60000000LL;
            }
            if (intent.source == voice_intent::Source::PodcastSearch) {
                std::memcpy(v.query, intent.name, sizeof(v.query));
                open_feed(v, Feed::Search);
            } else {
                open_feed(v, intent.source == voice_intent::Source::PodcastHot ? Feed::Hot : Feed::Recommended);
            }
        }
    }
    build_ui();
    update_status_bar(true);
    refresh();
    lv_screen_load(screen);
    timer = lv_timer_create(tick, 250, nullptr);
    bsp_lvgl_unlock();
}

bool prepare_stop() {
    exiting.store(true);
    if (!bsp_lvgl_lock(20)) return false;
    bool ready = true;
    if (request) {
        request->cancel.store(true);
        if (request->done.load(std::memory_order_acquire)) {
            delete request;
            request = nullptr;
        } else ready = false;
    }
    if (session && session->player_ready) {
        if (radio_player_deinit(0)) {
            save_progress(*session, radio_player_episode());
            session->player_ready = false;
        } else ready = false;
    }
    bsp_lvgl_unlock();
    return ready;
}

void stop() {
    if (!bsp_lvgl_lock(-1)) return;
    if (timer) { lv_timer_delete(timer); timer = nullptr; }
    if (screen) { lv_obj_delete(screen); screen = nullptr; }
    menu_group = list_group = player_group = nullptr;
    footer_primary = footer_secondary = nullptr;
    menu_count_label = list_heading = list_counter = list_empty = nullptr;
    player_program = player_status = player_title = player_note = nullptr;
    player_meter = player_bar_fill = player_time = player_quality = nullptr;
    for (auto &bar : wifi_bars) bar = nullptr;
    battery = {};
    for (std::size_t i = 0; i < kMenuSlots; ++i) menu_rows[i] = menu_bars[i] = menu_labels[i] = nullptr;
    for (std::size_t i = 0; i < kListSlots; ++i) list_cards[i] = list_bars[i] = list_titles[i] = list_metas[i] = nullptr;
    std::memset(meter_value, 0, sizeof(meter_value));
    delete session;
    session = nullptr;
    bsp_lvgl_unlock();
}

bool back() {
    if (!bsp_lvgl_lock(100)) return true;
    const bool nested = session && session->view != View::Menu;
    if (nested) {
        auto &v = *session;
        if (v.view == View::Player && v.player_ready) {
            v.returning = true;
            v.message = "正在停止播放";
        } else {
            if (request) request->cancel.store(true);
            v.autoplay = false;
            v.latest = false;
            v.volume_notice = false;
            v.stop_deadline = 0;
            std::memset(meter_value, 0, sizeof(meter_value));
            if (v.view == View::Player) v.view = View::Episodes;
            else if (v.view == View::Episodes && v.feed != Feed::Resume) v.view = View::Programs;
            else {
                v.view = View::Menu;
                if (v.menu >= menu_count(v)) v.menu = menu_count(v) - 1;
            }
            v.message = nullptr;
        }
        refresh();
    }
    bsp_lvgl_unlock();
    return nested;
}

void on_key(uint8_t button, app_btn_event_t event, uint16_t) {
    if ((event != BTN_EVT_CLICK && event != BTN_EVT_LONG) || !bsp_lvgl_lock(100)) return;
    if (!session || exiting.load() || session->returning) { bsp_lvgl_unlock(); return; }
    auto &v = *session;
    if (v.view == View::Menu && event == BTN_EVT_CLICK && !request) {
        const unsigned count = menu_count(v);
        if (button == BSP_BTN_UP && v.menu) --v.menu;
        if (button == BSP_BTN_DOWN && v.menu + 1 < count) ++v.menu;
        if (button == BSP_BTN_OK) {
            if (v.recent.program && v.menu == 0) open_resume(v);
            else open_feed(v, menu_feed(v, v.menu));
        }
    } else if (v.view == View::Programs && !request) {
        if (event == BTN_EVT_CLICK) {
            if (button == BSP_BTN_UP && v.program) --v.program;
            if (button == BSP_BTN_DOWN && v.program + 1 < v.programs.count) ++v.program;
            if (button == BSP_BTN_OK) {
                v.latest = false;
                if (v.programs.count) open_program(v);
                else fetch(v);
            }
        } else if (button == BSP_BTN_UP) {
            fetch(v);
        }
    } else if (v.view == View::Episodes && !request) {
        if (event == BTN_EVT_LONG) {
            if (button == BSP_BTN_UP && v.page > 1) fetch(v, v.page - 1);
            if (button == BSP_BTN_DOWN && v.page < 100000 && (v.page_data.total ?
                v.page * kPageSize < v.page_data.total : v.page_data.count == kPageSize)) fetch(v, v.page + 1);
        } else if (button == BSP_BTN_UP && v.selected) --v.selected;
        else if (button == BSP_BTN_DOWN && v.selected + 1 < v.page_data.count) ++v.selected;
        else if (button == BSP_BTN_OK) {
            if (!v.page_data.count) fetch(v, v.page);
            else {
                v.view = View::Player;
                v.started = false;
                v.volume_notice = false;
                std::memset(meter_value, 0, sizeof(meter_value));
                play(v);
            }
        }
    } else if (v.view == View::Player && event == BTN_EVT_CLICK) {
        if (button == BSP_BTN_OK) {
            if (v.player_ready && radio_player_is_playing()) {
                radio_player_set_playing(false);
                v.message = nullptr;
            } else {
                if (v.started && radio_player_wait_idle(0)) save_progress(v, radio_player_episode());
                play(v);
            }
        } else if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
            radio_player_set_volume(static_cast<uint8_t>(std::clamp(
                int(radio_player_volume()) + (button == BSP_BTN_UP ? 5 : -5), 0, 100)));
            v.volume_notice = true;
            v.volume_until_ms = lv_tick_get() + 2000;
        }
    }
    refresh();
    bsp_lvgl_unlock();
}
} // namespace
} // namespace podcast

extern const passport_app_t g_podcast_app = {
    .id = APP_ID_PODCAST,
    .name = "播客",
    .en_name = "PODCAST",
    .desc = "热门发现与断点续播",
    .tag = "Wi-Fi",
    .theme_color = 0xC18DD9,
    .init = nullptr,
    .start = podcast::start,
    .stop = podcast::stop,
    .on_key = podcast::on_key,
    .prepare_stop = podcast::prepare_stop,
    .on_back = podcast::back,
};
