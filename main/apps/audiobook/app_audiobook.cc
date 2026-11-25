// 听书: long-form Ximalaya audiobooks with a five-book shelf, chapter-accurate
// resume, continuous playback across pages and a bedtime timer. Shares the
// podcast catalog parser and the finite-stream mode of the radio player.
#include "apps/podcast/podcast_catalog.h"
#include "apps/audiobook/audiobook_shelf.h"
#include "apps/app_base.h"
#include "apps/radio/radio_player.h"
#include "launcher/app_registry.h"
#include "launcher/power.h"
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
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <strings.h>

namespace audiobook {
namespace {

using podcast::Discovery;
using podcast::Listing;
using podcast::Page;
using podcast::ProgramList;
using podcast::kPageSize;

enum class View { Menu, Shelf, Books, Chapters, Player };
enum class Feed : uint8_t { Hot, Mystery, History, Classics, Search };
enum class Entry : uint8_t { Resume, Shelf, Hot, Mystery, History, Classics };

constexpr std::size_t kMenuSlots = 6;
constexpr std::size_t kListSlots = 3;
constexpr unsigned kShelfBooks = 5;
constexpr unsigned kMaxPage = 100000;
constexpr unsigned kMaxSkippedPages = 3;
// Bookmark offsets: the top bit marks the 32 kbps stream; kFinished marks a
// chapter heard to the end, so resuming starts with the next chapter.
constexpr uint32_t kBackupBookmark = uint32_t{1} << 31;
constexpr uint32_t kFinished = kBackupBookmark - 1;
constexpr uint32_t kShelfVersion = 1;
constexpr uint16_t kSleepChoices[] = {15, 30, 45};
constexpr char kNamespace[] = "audiobook";

struct Book {
    uint32_t album;
    uint32_t episode;  // 0 until a chapter starts
    uint32_t page;
    uint32_t offset;
    uint32_t tracks;
    uint32_t chapter;  // 1-based position in the ascending album
    char title[96];
};

struct Shelf {
    uint32_t version;
    uint32_t count;
    Book books[kShelfBooks];  // most recent first
};

struct Request {
    uint32_t album = 0;  // non-zero: chapter page, otherwise book discovery
    unsigned page = 1;
    Discovery discovery = Discovery::BookHot;
    char query[voice_intent::kNameBytes]{};
    Page result{};
    ProgramList books{};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
};

struct Session {
    Shelf shelf{};
    Page page_data{};
    ProgramList books{};
    Book book{};
    View view = View::Menu;
    View origin = View::Menu;  // where the chapter list returns
    Feed feed = Feed::Hot;
    unsigned menu = 0;
    unsigned shelf_index = 0;
    unsigned book_index = 0;
    unsigned page = 1;
    unsigned selected = 0;
    unsigned skipped_pages = 0;
    char query[voice_intent::kNameBytes]{};
    bool autoplay = false;   // start the first playable chapter after the fetch
    bool advancing = false;  // the fetch continues playback on the next page
    bool advance_failed = false;
    bool pending_next = false;
    bool player_ready = false;
    bool returning = false;
    bool online = false;
    bool started = false;
    bool backup_active = false;
    bool volume_notice = false;
    bool sleep_chapter = false;
    bool shutting_down = false;
    uint16_t sleep_minutes = 0;
    int64_t sleep_deadline = 0;
    uint32_t volume_until_ms = 0;
    uint32_t initial_offset = 0;
    int64_t save_at = 0;
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

lv_obj_t *player_book = nullptr;
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

lv_obj_t *make_accent_bar(lv_obj_t *parent, int h) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    const bool studio = ui_theme_is_studio();
    const int bar_y = studio ? (h >= 60 ? 8 : h >= 28 ? 6 : 10) : 0;
    const int bar_h = studio ? (h >= 28 ? h - bar_y * 2 : 20) : h;
    lv_obj_set_pos(bar, 0, bar_y);
    lv_obj_set_size(bar, 3, bar_h);
    if (studio) lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(studio ? UI_TERTIARY : UI_ACCENT), 0);
    return bar;
}

// ---- Shelf persistence -------------------------------------------------

Shelf load_shelf() {
    Shelf value{};
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return value;
    std::size_t size = sizeof(value);
    const bool ok = nvs_get_blob(handle, "shelf", &value, &size) == ESP_OK && size == sizeof(value) &&
                    value.version == kShelfVersion && value.count <= kShelfBooks;
    nvs_close(handle);
    if (!ok) return {};
    unsigned kept = 0;
    for (unsigned i = 0; i < value.count; ++i) {
        Book book = value.books[i];
        book.title[sizeof(book.title) - 1] = '\0';
        if (!book.album || !book.title[0] || book.page > kMaxPage) continue;
        if (!book.page) book.page = 1;
        value.books[kept++] = book;
    }
    value.count = kept;
    return value;
}

void write_shelf(Session &v, const Shelf &next) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, "shelf", &next, sizeof(next)) == ESP_OK && nvs_commit(handle) == ESP_OK) {
        v.shelf = next;
    }
    nvs_close(handle);
}

unsigned shelf_find(const Shelf &shelf, uint32_t album) {
    for (unsigned i = 0; i < shelf.count; ++i) if (shelf.books[i].album == album) return i;
    return shelf.count;
}

// Moves the book to the front, dropping the least recent beyond five.
void shelf_put(Session &v, const Book &book) {
    if (!book.album || !book.title[0]) return;
    if (v.shelf.count && !std::memcmp(&v.shelf.books[0], &book, sizeof(book))) return;
    Shelf next{};
    next.version = kShelfVersion;
    next.books[next.count++] = book;
    for (unsigned i = 0; i < v.shelf.count && next.count < kShelfBooks; ++i) {
        if (v.shelf.books[i].album != book.album) next.books[next.count++] = v.shelf.books[i];
    }
    write_shelf(v, next);
}

void shelf_remove(Session &v, unsigned index) {
    if (index >= v.shelf.count) return;
    Shelf next = v.shelf;
    next.version = kShelfVersion;
    std::memmove(&next.books[index], &next.books[index + 1], (next.count - index - 1) * sizeof(Book));
    --next.count;
    next.books[next.count] = {};
    write_shelf(v, next);
}

uint32_t chapter_number(const Session &v, unsigned index) {
    return static_cast<uint32_t>((v.page - 1) * kPageSize + index + 1);
}

void save_progress(Session &v, EpisodePlayback playback) {
    v.save_at = esp_timer_get_time();
    if (!v.started || !v.page_data.count || v.selected >= v.page_data.count) return;
    if (playback.offset >= kBackupBookmark) return;
    const auto &ep = v.page_data.episodes[v.selected];
    v.book.episode = ep.id;
    v.book.page = v.page;
    v.book.chapter = chapter_number(v, v.selected);
    v.book.offset = playback.state == RadioPlaybackState::Finished
        ? kFinished : playback.offset | (v.backup_active ? kBackupBookmark : 0);
    shelf_put(v, v.book);
}

// ---- Fetching ----------------------------------------------------------

void fetch_task(void *arg) {
    auto &job = *static_cast<Request *>(arg);
    job.ok = job.album ? podcast::fetch_page(job.album, job.page, job.cancel, job.result)
                       : podcast::discover(job.discovery, job.query, job.cancel, job.books);
    job.done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

Discovery feed_discovery(Feed feed) {
    switch (feed) {
        case Feed::Hot: return Discovery::BookHot;
        case Feed::Mystery: return Discovery::BookMystery;
        case Feed::History: return Discovery::BookHistory;
        case Feed::Classics: return Discovery::BookClassics;
        default: return Discovery::Search;
    }
}

bool fetch(Session &v, bool chapters, unsigned page = 1) {
    if (request) return false;
    if (!WifiManager::GetInstance().IsConnected()) {
        v.message = "请先连接 Wi-Fi";
        return false;
    }
    request = new (std::nothrow) Request;
    if (request) {
        if (chapters) {
            request->album = v.book.album;
            request->page = page;
        } else {
            request->discovery = feed_discovery(v.feed);
            std::memcpy(request->query, v.query, sizeof(request->query));
        }
    }
    if (!request || xTaskCreate(fetch_task, "book_list", 6144, request, 4, nullptr) != pdPASS) {
        delete request;
        request = nullptr;
        v.message = "内存不足，请重试";
        return false;
    }
    v.message = chapters ? "正在获取章节" : "正在获取书目";
    return true;
}

bool has_more_pages(const Session &v) {
    if (v.page >= kMaxPage) return false;
    return v.page_data.total ? v.page * kPageSize < v.page_data.total : v.page_data.count == kPageSize;
}

void open_book(Session &v, const Book &book, View origin) {
    v.book = book;
    v.origin = origin;
    v.view = View::Chapters;
    v.page_data = {};
    v.selected = 0;
    v.skipped_pages = 0;
    v.advancing = false;
    v.page = book.page ? book.page : 1;
    if (!fetch(v, true, v.page)) v.autoplay = false;
}

Book book_from_listing(const Session &v, const Listing &item) {
    const unsigned found = shelf_find(v.shelf, item.id);
    Book book = found < v.shelf.count ? v.shelf.books[found] : Book{item.id, 0, 1, 0, 0, 0, {}};
    if (item.tracks) book.tracks = item.tracks;
    std::snprintf(book.title, sizeof(book.title), "%s", item.title);
    return book;
}

void open_feed(Session &v, Feed feed) {
    v.view = View::Books;
    v.feed = feed;
    v.book_index = 0;
    v.books = {};
    v.message = nullptr;
    if (!fetch(v, false)) v.autoplay = false;
}

void resume_book(Session &v, unsigned index, View origin) {
    if (index >= v.shelf.count) return;
    v.autoplay = true;
    open_book(v, v.shelf.books[index], origin);
}

void play(Session &v);

void enter_player(Session &v) {
    v.view = View::Player;
    v.started = false;
    v.volume_notice = false;
    std::memset(meter_value, 0, sizeof(meter_value));
}

// Plays the first free chapter from index on, fetching later pages if needed.
bool start_from(Session &v, unsigned index) {
    for (unsigned i = index; i < v.page_data.count; ++i) {
        const auto &ep = v.page_data.episodes[i];
        if (ep.paid || !*ep.url) continue;
        v.selected = i;
        v.skipped_pages = 0;
        if (v.view != View::Player) enter_player(v);
        play(v);
        return v.started;
    }
    if (has_more_pages(v) && v.skipped_pages < kMaxSkippedPages) {
        ++v.skipped_pages;
        v.advancing = true;
        if (fetch(v, true, v.page + 1)) {
            v.message = "正在获取下一章";
            return true;
        }
        v.advancing = false;
    }
    v.skipped_pages = 0;
    v.message = has_more_pages(v) ? "后续章节需付费" : index >= v.page_data.count ? "全书已听完" : "暂无可播放章节";
    return false;
}

bool advance(Session &v) {
    v.advance_failed = !start_from(v, v.selected + 1);
    return !v.advance_failed;
}

bool finish_fetch(Session &v) {
    if (!request || !request->done.load(std::memory_order_acquire)) return false;
    auto *done = request;
    request = nullptr;
    if (done->cancel.load()) { delete done; return false; }
    if (!done->album) {
        if (v.view != View::Books) { delete done; return true; }
        if (!done->ok) v.message = "书目获取失败，按确定重试";
        else {
            v.books = done->books;
            v.book_index = 0;
            v.message = v.books.count ? nullptr : "没有找到有声书";
        }
        delete done;
        if (!v.autoplay || !v.books.count) { v.autoplay = false; return true; }
        if (v.feed == Feed::Search) {
            const auto found = podcast::match_book(v.books, v.query);
            if (found == v.books.count) {
                v.autoplay = false;
                v.message = "未找到同名书，请选择";
                return true;
            }
            v.book_index = found;
        }
        open_book(v, book_from_listing(v, v.books.items[v.book_index]), View::Books);
        return true;
    }
    if (v.view != View::Chapters && v.view != View::Player) { delete done; return true; }
    const auto page = done->page;
    const bool fetched = done->ok;
    const bool has_chapters = fetched && done->result.count != 0;
    if (has_chapters) {
        v.page = page;
        v.page_data = done->result;
        v.selected = 0;
        v.message = nullptr;
    } else v.message = fetched ? "此页没有章节" : "章节获取失败，按确定重试";
    delete done;

    if (v.advancing) {
        v.advancing = false;
        if (has_chapters) v.advance_failed = !start_from(v, 0);
        else { v.advance_failed = true; v.skipped_pages = 0; }
        return true;
    }
    unsigned marked = v.page_data.count;
    if (has_chapters) {
        for (unsigned i = 0; i < v.page_data.count; ++i) {
            if (v.page_data.episodes[i].id == v.book.episode) marked = v.selected = i;
        }
    }
    if (!v.autoplay) return true;
    v.autoplay = false;
    if (!has_chapters) return true;
    if (marked == v.page_data.count && page > 1) {
        // The album changed order since the bookmark: start over from page 1.
        v.autoplay = true;
        if (fetch(v, true, 1)) return true;
        v.autoplay = false;
    }
    const unsigned from = marked == v.page_data.count ? 0
        : marked + (v.book.offset == kFinished ? 1 : 0);
    if (start_from(v, from) || v.advancing) return true;
    // A hot pick without a free chapter: try the next few books.
    if (v.origin == View::Books && v.feed != Feed::Search &&
        v.book_index + 1 < std::min(v.books.count, 3u)) {
        ++v.book_index;
        v.autoplay = true;
        open_book(v, book_from_listing(v, v.books.items[v.book_index]), View::Books);
    } else if (v.view == View::Player) {
        v.view = View::Chapters;
    }
    return true;
}

// ---- Bedtime timer -----------------------------------------------------

void shutdown_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(300));
    power_off(nullptr);
}

void set_sleep(Session &v, uint16_t minutes, bool chapter_end) {
    v.sleep_minutes = minutes;
    v.sleep_chapter = chapter_end;
    v.sleep_deadline = minutes ? esp_timer_get_time() + int64_t(minutes) * 60000000LL : 0;
}

// Off -> 15 -> 30 -> 45 minutes -> end of chapter -> off.
void cycle_sleep(Session &v) {
    if (v.sleep_chapter) { set_sleep(v, 0, false); return; }
    for (const auto minutes : kSleepChoices) {
        if (minutes > v.sleep_minutes) { set_sleep(v, minutes, false); return; }
    }
    set_sleep(v, 0, true);
}

// Same as the radio sleep timer: stop the stream, keep the bookmark, power off.
void bedtime(Session &v) {
    if (v.shutting_down) return;
    v.shutting_down = true;
    if (v.player_ready) {
        radio_player_set_playing(false);
        if (v.started) save_progress(v, radio_player_episode());
        radio_player_set_network(false);
    }
    set_sleep(v, 0, false);
    v.message = "定时结束，正在关机";
    if (xTaskCreate(shutdown_task, "book_sleep", 2048, nullptr, 7, nullptr) != pdPASS) power_off(nullptr);
}

// ---- Rendering ---------------------------------------------------------

const char *status_text(RadioPlaybackState state) {
    switch (state) {
        case RadioPlaybackState::Stopped: return "已暂停";
        case RadioPlaybackState::Connecting: return "连接中";
        case RadioPlaybackState::Buffering: return "缓冲中";
        case RadioPlaybackState::Playing: return "播放中";
        case RadioPlaybackState::Reconnecting: return "重连中";
        case RadioPlaybackState::Error: return "播放失败";
        case RadioPlaybackState::Finished: return "本章播完";
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

void format_book_meta(const Listing &item, char *out, std::size_t capacity) {
    char plays[32];
    if (item.plays >= 100000000u) {
        std::snprintf(plays, sizeof(plays), "%lu.%lu亿播放", static_cast<unsigned long>(item.plays / 100000000u),
                      static_cast<unsigned long>((item.plays / 10000000u) % 10u));
    } else if (item.plays >= 10000u) {
        std::snprintf(plays, sizeof(plays), "%lu万播放", static_cast<unsigned long>(item.plays / 10000u));
    } else {
        std::snprintf(plays, sizeof(plays), "免费收听");
    }
    if (item.tracks) std::snprintf(out, capacity, "共%lu集 · %s", static_cast<unsigned long>(item.tracks), plays);
    else std::snprintf(out, capacity, "%s", plays);
}

void format_shelf_meta(const Book &book, char *out, std::size_t capacity) {
    char total[28] = "";
    if (book.tracks) std::snprintf(total, sizeof(total), " · 共%lu集", static_cast<unsigned long>(book.tracks));
    if (!book.chapter) std::snprintf(out, capacity, "尚未开始%s", total);
    else std::snprintf(out, capacity, "%s第%lu集%s", book.offset == kFinished ? "听完" : "听到",
                       static_cast<unsigned long>(book.chapter), total);
}

void format_chapter_meta(const Session &v, unsigned index, char *out, std::size_t capacity) {
    const auto &ep = v.page_data.episodes[index];
    char duration[24] = "";
    if (ep.duration) std::snprintf(duration, sizeof(duration), " · %02u:%02u",
                                   unsigned(ep.duration / 60), unsigned(ep.duration % 60));
    const bool marked = ep.id == v.book.episode;
    const char *badge = ep.paid ? " · 需付费"
        : marked && v.book.offset == kFinished ? " · 已听完"
        : marked && (v.book.offset & ~kBackupBookmark) ? " · 继续收听" : "";
    std::snprintf(out, capacity, "第%lu集%s%s", static_cast<unsigned long>(chapter_number(v, index)),
                  duration, badge);
}

unsigned menu_entries(const Session &v, Entry *out) {
    unsigned count = 0;
    if (v.shelf.count) {
        out[count++] = Entry::Resume;
        out[count++] = Entry::Shelf;
    }
    for (Entry e : {Entry::Hot, Entry::Mystery, Entry::History, Entry::Classics}) out[count++] = e;
    return count;
}

const char *feed_name(Feed feed) {
    switch (feed) {
        case Feed::Hot: return "热播有声书";
        case Feed::Mystery: return "悬疑推理";
        case Feed::History: return "历史评书";
        case Feed::Classics: return "名著科幻";
        default: return "搜索结果";
    }
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

void style_row(lv_obj_t *row, lv_obj_t *bar, bool active) {
    ui_pixel_bg_color(row, active ? UI_SURFACE_ALT : UI_SURFACE);
    set_border_color(row, active ? (ui_theme_is_studio() ? UI_ACCENT : UI_AMBER) : UI_BORDER);
    set_visible(bar, active);
}

void refresh_menu(Session &v) {
    show_group(menu_group);
    Entry entries[kMenuSlots];
    const unsigned count = menu_entries(v, entries);
    if (v.menu >= count) v.menu = count - 1;
    char counter[16];
    std::snprintf(counter, sizeof(counter), "%02u / %02u", v.menu + 1, count);
    ui_pixel_label_set_text(menu_count_label, counter);
    for (std::size_t i = 0; i < kMenuSlots; ++i) {
        if (i >= count) { set_visible(menu_rows[i], false); continue; }
        set_visible(menu_rows[i], true);
        const bool active = i == v.menu;
        style_row(menu_rows[i], menu_bars[i], active);
        const Entry entry = entries[i];
        ui_pixel_text_color(menu_labels[i], active ? UI_TEXT : entry == Entry::Resume ? UI_ACCENT : UI_TEXT_DIM);
        char text[64];
        switch (entry) {
            case Entry::Resume:
                ui_pixel_label_set_text(menu_labels[i], ("继续 · " + shown(v.shelf.books[0].title)).c_str());
                continue;
            case Entry::Shelf:
                std::snprintf(text, sizeof(text), "我的书架 (%u)", unsigned(v.shelf.count));
                break;
            case Entry::Hot: std::snprintf(text, sizeof(text), "%s", feed_name(Feed::Hot)); break;
            case Entry::Mystery: std::snprintf(text, sizeof(text), "%s", feed_name(Feed::Mystery)); break;
            case Entry::History: std::snprintf(text, sizeof(text), "%s", feed_name(Feed::History)); break;
            case Entry::Classics: std::snprintf(text, sizeof(text), "%s", feed_name(Feed::Classics)); break;
        }
        ui_pixel_label_set_text(menu_labels[i], text);
    }
    set_footer(v.message ? v.message : "上下选择 · 确定打开", "双击小智点书 · 长按返回");
}

void refresh_list(Session &v) {
    show_group(list_group);
    char counter[24] = "";
    unsigned total = 0;
    unsigned active_index = 0;
    std::string heading;
    if (v.view == View::Shelf) {
        heading = "我的书架";
        total = v.shelf.count;
        active_index = v.shelf_index;
        if (total) std::snprintf(counter, sizeof(counter), "%u / %u", active_index + 1, total);
    } else if (v.view == View::Books) {
        heading = v.feed == Feed::Search ? shown(v.query) : feed_name(v.feed);
        total = v.books.count;
        active_index = v.book_index;
        if (total) std::snprintf(counter, sizeof(counter), "%u / %u", active_index + 1, total);
    } else {
        heading = shown(v.book.title);
        total = v.page_data.count;
        active_index = v.selected;
        const unsigned pages = v.page_data.total ? (v.page_data.total + kPageSize - 1) / kPageSize : 0;
        if (pages) std::snprintf(counter, sizeof(counter), "%u/%u页", v.page, pages);
        else std::snprintf(counter, sizeof(counter), "第%u页", v.page);
    }
    ui_pixel_label_set_text(list_heading, heading.c_str());
    ui_pixel_label_set_text(list_counter, counter);

    const unsigned first = total <= kListSlots ? 0u
        : std::min(active_index > 1 ? active_index - 1 : 0u, total - unsigned(kListSlots));
    const unsigned visible = total > first ? std::min<unsigned>(total - first, kListSlots) : 0u;
    for (std::size_t r = 0; r < kListSlots; ++r) {
        if (r >= visible) { set_visible(list_cards[r], false); continue; }
        set_visible(list_cards[r], true);
        const unsigned idx = first + static_cast<unsigned>(r);
        const bool active = idx == active_index;
        style_row(list_cards[r], list_bars[r], active);
        ui_pixel_text_color(list_titles[r], active ? UI_TEXT : UI_TEXT_DIM);
        ui_pixel_text_color(list_metas[r], active ? UI_ACCENT : UI_TEXT_DIM);
        char meta[64];
        const char *title = "";
        if (v.view == View::Shelf) {
            title = v.shelf.books[idx].title;
            format_shelf_meta(v.shelf.books[idx], meta, sizeof(meta));
        } else if (v.view == View::Books) {
            title = v.books.items[idx].title;
            format_book_meta(v.books.items[idx], meta, sizeof(meta));
        } else {
            title = v.page_data.episodes[idx].title;
            format_chapter_meta(v, idx, meta, sizeof(meta));
        }
        ui_pixel_label_set_text(list_titles[r], shown(title).c_str());
        ui_pixel_label_set_text(list_metas[r], meta);
    }
    set_visible(list_empty, visible == 0);
    if (!visible) {
        ui_pixel_label_set_text(list_empty, v.message ? v.message
            : v.view == View::Shelf ? "书架还是空的" : "暂无内容");
    }
    if (v.view == View::Shelf) set_footer(v.message ? v.message : "确定续听 · 长按上移出", "长按确定返回");
    else if (v.view == View::Books) set_footer(v.message ? v.message : "上下选择 · 确定选章", "长按确定返回");
    else set_footer(v.message ? v.message : "上下选章 · 长按上下翻页", "长按确定返回");
}

void refresh_player(Session &v) {
    show_group(player_group);
    const auto playback = v.player_ready ? radio_player_episode() : EpisodePlayback{};
    const auto &ep = v.page_data.episodes[std::min<unsigned>(v.selected, kPageSize - 1)];
    ui_pixel_label_set_text(player_book, shown(v.book.title).c_str());
    char chapter_title[sizeof(ep.title) + 24];
    std::snprintf(chapter_title, sizeof(chapter_title), "第%lu集  %s",
                  static_cast<unsigned long>(chapter_number(v, v.selected)), ep.title);
    ui_pixel_label_set_text(player_title, shown(chapter_title).c_str());

    if (v.volume_notice && static_cast<int32_t>(lv_tick_get() - v.volume_until_ms) < 0) {
        char vol[24];
        std::snprintf(vol, sizeof(vol), "音量 %u%%", unsigned(radio_player_volume()));
        ui_pixel_label_set_text(player_status, vol);
        ui_pixel_text_color(player_status, UI_AMBER);
    } else {
        v.volume_notice = false;
        ui_pixel_label_set_text(player_status, v.started || v.player_ready ? status_text(playback.state) : "");
        ui_pixel_text_color(player_status, status_color(playback.state));
    }

    char sleep_note[40];
    const char *note = v.message;
    uint32_t note_color = UI_WARNING;
    if (!note && v.started && playback.state == RadioPlaybackState::Error) note = "播放中断，按确定重试";
    if (!note && v.sleep_chapter) { note = "本章播完后关机"; note_color = UI_ACCENT; }
    if (!note && v.sleep_deadline) {
        const int64_t left = std::max<int64_t>(0, v.sleep_deadline - esp_timer_get_time()) / 1000000;
        std::snprintf(sleep_note, sizeof(sleep_note), "定时 %02u:%02u 后关机",
                      unsigned(left / 60), unsigned(left % 60));
        note = sleep_note;
        note_color = UI_ACCENT;
    }
    set_visible(player_note, note != nullptr);
    if (note) {
        ui_pixel_label_set_text(player_note, note);
        ui_pixel_text_color(player_note, note_color);
    }

    char time_buf[32];
    int bar_w = 1;
    if (v.started && playback.total && ep.duration) {
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
    set_footer("确定暂停 · 上下音量", "长按上定时 · 长按下切章");
}

void refresh() {
    if (!session) {
        show_group(list_group);
        ui_pixel_label_set_text(list_heading, "听书");
        ui_pixel_label_set_text(list_counter, "");
        for (std::size_t r = 0; r < kListSlots; ++r) set_visible(list_cards[r], false);
        set_visible(list_empty, true);
        ui_pixel_label_set_text(list_empty, "内存不足，请退出重试");
        set_footer("长按确定返回", "");
        return;
    }
    auto &v = *session;
    if (v.view == View::Menu) refresh_menu(v);
    else if (v.view == View::Player) refresh_player(v);
    else refresh_list(v);
}

// ---- Playback ----------------------------------------------------------

void play(Session &v) {
    if (v.selected >= v.page_data.count) return;
    const auto &ep = v.page_data.episodes[v.selected];
    if (ep.paid || !*ep.url) { v.message = ep.paid ? "此章节需要付费" : "此章节暂无音频"; return; }
    if (!WifiManager::GetInstance().IsConnected()) { v.message = "请先连接 Wi-Fi"; return; }
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
    const bool resume = v.book.episode == ep.id && v.book.offset != kFinished;
    const uint32_t stored = resume ? v.book.offset : 0;
    v.backup_active = (stored & kBackupBookmark) && *ep.backup_url;
    const uint32_t at = bool(stored & kBackupBookmark) == v.backup_active ? stored & ~kBackupBookmark : 0;
    const uint32_t from = at > kRewindBytes ? at - kRewindBytes : 0;
    if (radio_player_play_episode(v.backup_active ? ep.backup_url : ep.url, from)) {
        v.initial_offset = from;
        v.started = true;
        v.advance_failed = false;
        v.save_at = esp_timer_get_time();
        v.message = nullptr;
        v.book.episode = ep.id;
        v.book.page = v.page;
        v.book.chapter = chapter_number(v, v.selected);
        v.book.offset = from | (v.backup_active ? kBackupBookmark : 0);
        shelf_put(v, v.book);
    } else v.message = "音频启动失败，按确定重试";
}

void tick(lv_timer_t *t) {
    if (!session || exiting.load()) return;
    auto &v = *session;
    const bool screen_on = app_registry_screen_is_on();
    lv_timer_set_period(t, !screen_on ? 1000 : v.view == View::Player ? 50 : 250);
    if (v.shutting_down) return;

    bool dirty = finish_fetch(v);
    if (v.player_ready) {
        const bool online = WifiManager::GetInstance().IsConnected();
        if (v.online != online) {
            v.online = online;
            radio_player_set_network(online);
            dirty = true;
        }
    }
    if (v.sleep_deadline && esp_timer_get_time() >= v.sleep_deadline) {
        bedtime(v);
        dirty = true;
    } else if (v.returning && radio_player_deinit(0)) {
        save_progress(v, radio_player_episode());
        v.player_ready = v.returning = v.started = false;
        v.volume_notice = false;
        std::memset(meter_value, 0, sizeof(meter_value));
        v.view = View::Chapters;
        v.message = nullptr;
        dirty = true;
    } else if (v.pending_next && !v.returning &&
               (!v.player_ready || (!radio_player_is_playing() && radio_player_wait_idle(0)))) {
        v.pending_next = false;
        if (v.player_ready && v.started) save_progress(v, radio_player_episode());
        v.started = false;
        advance(v);
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
        } else if (idle && playback.state == RadioPlaybackState::Finished) {
            save_progress(v, playback);
            v.started = false;
            if (v.sleep_chapter) bedtime(v);
            else advance(v);
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

// ---- Lifecycle ---------------------------------------------------------

void build_ui() {
    screen = lv_obj_create(nullptr);
    ui_pixel_background(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = ui_pixel_top_bar(screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "听书", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);

    menu_group = make_group(screen);
    make_label(menu_group, "喜马拉雅有声书", 12, 34, 144, 20, UI_TEXT_DIM);
    menu_count_label = make_label(menu_group, "", 160, 36, 68, 18, UI_TEXT_DIM,
                                  &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    for (std::size_t i = 0; i < kMenuSlots; ++i) {
        const int y = 58 + static_cast<int>(i) * 33;
        menu_rows[i] = make_box(menu_group, 12, y, 216, 30, UI_SURFACE);
        lv_obj_set_style_border_width(menu_rows[i], 1, 0);
        lv_obj_set_style_border_color(menu_rows[i], lv_color_hex(UI_BORDER), 0);
        if (ui_theme_is_studio()) lv_obj_set_style_radius(menu_rows[i], 6, 0);
        menu_bars[i] = make_accent_bar(menu_rows[i], 30);
        menu_labels[i] = ui_pixel_label(menu_rows[i], "", &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(menu_labels[i], 10, 5);
        lv_obj_set_size(menu_labels[i], 196, 20);
        lv_label_set_long_mode(menu_labels[i], LV_LABEL_LONG_DOT);
    }

    list_group = make_group(screen);
    list_heading = make_label(list_group, "", 12, 34, 136, 20, UI_ACCENT);
    list_counter = make_label(list_group, "", 150, 34, 78, 20, UI_TEXT_DIM,
                              &buddy_font_16, LV_TEXT_ALIGN_RIGHT);
    for (std::size_t r = 0; r < kListSlots; ++r) {
        const int y = 60 + static_cast<int>(r) * 68;
        list_cards[r] = make_box(list_group, 12, y, 216, 62, UI_SURFACE);
        lv_obj_set_style_border_width(list_cards[r], 1, 0);
        lv_obj_set_style_border_color(list_cards[r], lv_color_hex(UI_BORDER), 0);
        if (ui_theme_is_studio()) lv_obj_set_style_radius(list_cards[r], 8, 0);
        list_bars[r] = make_accent_bar(list_cards[r], 62);
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

    player_group = make_group(screen);
    player_book = make_label(player_group, "", 12, 36, 124, 20, UI_TEXT_DIM);
    player_status = make_label(player_group, "", 140, 36, 88, 20, UI_TEXT_DIM,
                               &buddy_font_16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *title_card = make_box(player_group, 12, 64, 216, 96, UI_SURFACE);
    lv_obj_set_style_border_width(title_card, 1, 0);
    lv_obj_set_style_border_color(title_card, lv_color_hex(UI_BORDER), 0);
    if (ui_theme_is_studio()) lv_obj_set_style_radius(title_card, 8, 0);
    make_accent_bar(title_card, 18);
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

    const bool studio = ui_theme_is_studio();
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

void apply_intent(Session &v, const voice_intent::Request &intent) {
    using voice_intent::Source;
    if (intent.stop_minutes) set_sleep(v, intent.stop_minutes, false);
    if (intent.source == Source::AudiobookResume) {
        if (v.shelf.count) resume_book(v, 0, View::Menu);
        else v.message = "书架还没有书，请先选一本";
        return;
    }
    if (intent.source == Source::AudiobookSearch) {
        // A book already on the shelf continues from its bookmark.
        for (unsigned i = 0; i < v.shelf.count; ++i) {
            if (strcasestr(v.shelf.books[i].title, intent.name)) { resume_book(v, i, View::Menu); return; }
        }
        std::memcpy(v.query, intent.name, sizeof(v.query));
        v.autoplay = true;
        open_feed(v, Feed::Search);
        return;
    }
    v.autoplay = true;
    open_feed(v, Feed::Hot);
}

void start() {
    if (!bsp_lvgl_lock(1000)) return;
    exiting.store(false);
    session = new (std::nothrow) Session;
    const auto intent = voice_intent::take(APP_ID_AUDIOBOOK);
    if (session) {
        session->shelf = load_shelf();
        if (intent.source != voice_intent::Source::None) apply_intent(*session, intent);
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
    player_book = player_status = player_title = player_note = nullptr;
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
        if (request) request->cancel.store(true);
        v.autoplay = v.advancing = v.pending_next = false;
        if (v.view == View::Player && v.player_ready) {
            v.returning = true;
            v.message = "正在停止播放";
        } else {
            v.volume_notice = false;
            std::memset(meter_value, 0, sizeof(meter_value));
            if (v.view == View::Player) v.view = View::Chapters;
            else if (v.view == View::Chapters) v.view = v.origin;
            else v.view = View::Menu;
            if (v.view == View::Shelf && !v.shelf.count) v.view = View::Menu;
            v.message = nullptr;
        }
        refresh();
    }
    bsp_lvgl_unlock();
    return nested;
}

void on_menu_key(Session &v, uint8_t button) {
    Entry entries[kMenuSlots];
    const unsigned count = menu_entries(v, entries);
    if (button == BSP_BTN_UP && v.menu) --v.menu;
    if (button == BSP_BTN_DOWN && v.menu + 1 < count) ++v.menu;
    if (button != BSP_BTN_OK || v.menu >= count) return;
    v.message = nullptr;
    switch (entries[v.menu]) {
        case Entry::Resume: resume_book(v, 0, View::Menu); break;
        case Entry::Shelf: v.view = View::Shelf; v.shelf_index = 0; break;
        case Entry::Hot: open_feed(v, Feed::Hot); break;
        case Entry::Mystery: open_feed(v, Feed::Mystery); break;
        case Entry::History: open_feed(v, Feed::History); break;
        case Entry::Classics: open_feed(v, Feed::Classics); break;
    }
}

void on_key(uint8_t button, app_btn_event_t event, uint16_t) {
    if ((event != BTN_EVT_CLICK && event != BTN_EVT_LONG) || !bsp_lvgl_lock(100)) return;
    if (!session || exiting.load() || session->returning || session->shutting_down) { bsp_lvgl_unlock(); return; }
    auto &v = *session;
    if (v.view == View::Menu && event == BTN_EVT_CLICK && !request) {
        on_menu_key(v, button);
    } else if (v.view == View::Shelf && !request) {
        if (event == BTN_EVT_LONG) {
            if (button == BSP_BTN_UP && v.shelf.count) {
                shelf_remove(v, v.shelf_index);
                if (v.shelf_index && v.shelf_index >= v.shelf.count) --v.shelf_index;
                v.message = v.shelf.count ? "已移出书架" : nullptr;
                if (!v.shelf.count) { v.view = View::Menu; v.menu = 0; }
            }
        } else if (button == BSP_BTN_UP && v.shelf_index) { --v.shelf_index; v.message = nullptr; }
        else if (button == BSP_BTN_DOWN && v.shelf_index + 1 < v.shelf.count) { ++v.shelf_index; v.message = nullptr; }
        else if (button == BSP_BTN_OK) resume_book(v, v.shelf_index, View::Shelf);
    } else if (v.view == View::Books && !request) {
        if (event == BTN_EVT_CLICK) {
            if (button == BSP_BTN_UP && v.book_index) --v.book_index;
            if (button == BSP_BTN_DOWN && v.book_index + 1 < v.books.count) ++v.book_index;
            if (button == BSP_BTN_OK) {
                if (v.books.count) open_book(v, book_from_listing(v, v.books.items[v.book_index]), View::Books);
                else fetch(v, false);
            }
        } else if (button == BSP_BTN_UP) {
            fetch(v, false);
        }
    } else if (v.view == View::Chapters && !request) {
        if (event == BTN_EVT_LONG) {
            if (button == BSP_BTN_UP && v.page > 1) fetch(v, true, v.page - 1);
            if (button == BSP_BTN_DOWN && has_more_pages(v)) fetch(v, true, v.page + 1);
        } else if (button == BSP_BTN_UP && v.selected) --v.selected;
        else if (button == BSP_BTN_DOWN && v.selected + 1 < v.page_data.count) ++v.selected;
        else if (button == BSP_BTN_OK) {
            if (!v.page_data.count) fetch(v, true, v.page);
            else if (v.page_data.episodes[v.selected].paid) v.message = "此章节需要付费";
            else {
                enter_player(v);
                play(v);
            }
        }
    } else if (v.view == View::Player) {
        if (event == BTN_EVT_LONG) {
            if (button == BSP_BTN_UP) cycle_sleep(v);
            else if (button == BSP_BTN_DOWN && !request && !v.pending_next) {
                if (v.player_ready && radio_player_is_playing()) radio_player_set_playing(false);
                v.pending_next = true;
                v.message = "正在切换下一章";
            }
        } else if (button == BSP_BTN_OK && !request && !v.pending_next) {
            if (v.player_ready && radio_player_is_playing()) {
                radio_player_set_playing(false);
                v.message = nullptr;
            } else if (v.advance_failed) {
                advance(v);
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

size_t read_shelf_summary(ShelfItem *out, size_t cap) {
    if (!out || !cap) return 0;
    const Shelf shelf = load_shelf();
    const size_t n = std::min<size_t>(cap, shelf.count);
    for (size_t i = 0; i < n; ++i) {
        const Book &book = shelf.books[i];
        out[i].chapter = book.chapter;
        out[i].tracks = book.tracks;
        out[i].finished = book.offset == kFinished;
        std::snprintf(out[i].title, sizeof(out[i].title), "%s", book.title);
    }
    return n;
}

} // namespace audiobook

extern const passport_app_t g_audiobook_app = {
    .id = APP_ID_AUDIOBOOK,
    .name = "听书",
    .en_name = "AUDIOBOOK",
    .desc = "长篇有声书与睡前定时",
    .tag = "Wi-Fi",
    .theme_color = 0xD9A86C,
    .init = nullptr,
    .start = audiobook::start,
    .stop = audiobook::stop,
    .on_key = audiobook::on_key,
    .prepare_stop = audiobook::prepare_stop,
    .on_back = audiobook::back,
};
