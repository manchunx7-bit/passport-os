// NetEase Cloud Music: QR login, playlists and sequential MP3 playback.
//
// One music task runs every network step in order, so the TLS session used
// for logged-in requests and the MP3 decoder never coexist. Its stack, the
// compressed FIFO and the model all live in the shared audio workspace.
#include "apps/app_base.h"
#include "apps/netease/netease_api.h"
#include "apps/netease/netease_catalog.h"
#include "apps/netease/netease_lyrics.h"
#include "apps/netease/netease_model.h"
#include "apps/netease/netease_player.h"
#include "apps/netease/netease_ui.h"
#include "apps/netease/netease_url_cache.h"
#include "apps/netease/netease_voice_search.h"
#include "apps/xiaozhi/xiaozhi_store.h"
#include "audio_workspace.h"
#include "bsp_button.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "launcher/settings.h"
#include "bsp_audio.h"
#include "apps/reminders/reminder_service.h"
#include "wifi_manager.h"
#include "voice_intent.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace netease {

namespace {

constexpr char kTag[] = "netease";
constexpr char kNs[] = "netease";
constexpr uint32_t kMusicStackBytes = 7168;  // matches netease_player.cc layout
constexpr uint32_t kMetaStackBytes = 6144;
constexpr unsigned kMaxConsecutiveFailures = 3;
constexpr int64_t kUrlLifetimeUs = 15LL * 60 * 1000 * 1000;
constexpr size_t kUrlBytes = 512;

static_assert(kMaxLists > kHomeEntries);

enum class Cmd : uint8_t { Boot, SkipLogin, Login, Logout, BackHome, OpenList, LoadPage, Play, Toggle, Next, Prev };
struct Command { Cmd cmd; uint16_t arg; };

std::atomic<bool> s_active{false};
std::atomic<bool> s_shutdown{false};
std::atomic<bool> s_interrupt{false};
std::atomic<bool> s_browse_cancel{false};
std::atomic<bool> s_task_running{false};
std::atomic<bool> s_meta_running{false};
std::atomic<uint16_t> s_meta_start{0};
QueueHandle_t s_queue = nullptr;
SemaphoreHandle_t s_lock = nullptr;
SemaphoreHandle_t s_stopped = nullptr;
TaskHandle_t s_task = nullptr;
UrlCache *s_urls = nullptr;
char *s_cookie = nullptr;  // "MUSIC_U=..." or empty
uint64_t s_uid = 0;
bool s_lists_pending = false; // voice startup defers personal lists until BackHome
Lyrics *s_lyrics = nullptr;
Cmd s_retry = Cmd::Boot;
char s_device_id[33] = "";
bool s_player_ready = false;
bool s_owner = false;  // this session acquired the workspace
voice_intent::Request *s_intent = nullptr;  // heap; taken at start, run once after boot
// Voice "stop after N minutes": the timer only raises flags; the music task
// pauses at its next check, exactly like a manual pause.
esp_timer_handle_t s_sleep_timer = nullptr;
std::atomic<bool> s_sleep_due{false};
int64_t s_started_us = 0;  // serial timing only

int elapsed_ms() { return static_cast<int>((esp_timer_get_time() - s_started_us) / 1000); }

bool shutting_down() { return s_shutdown.load(std::memory_order_acquire); }
bool browse_cancelled() { return shutting_down() || s_browse_cancel.load(std::memory_order_acquire); }
// Any queued command ends the current step (playback, preparation) promptly.
bool interrupted() {
    return shutting_down() || reminders::sound_pending() || s_interrupt.load(std::memory_order_acquire) ||
           s_sleep_due.load(std::memory_order_acquire) ||
           (s_queue && uxQueueMessagesWaiting(s_queue) > 0);
}
bool login_cancelled() { return browse_cancelled() || (s_queue && uxQueueMessagesWaiting(s_queue) > 0); }

// Model writes from the music task. Keep these short; the UI timer reads them.
struct Guard {
    bool ok;
    Guard() : ok(model_lock()) {}
    ~Guard() { if (ok) { ++model().revision; model_unlock(); } }
};

void show_message(const char *text, bool retry, Cmd retry_cmd = Cmd::Boot) {
    Guard g;
    if (s_browse_cancel.load()) return;
    Model &m = model();
    std::snprintf(m.message, sizeof(m.message), "%s", text);
    m.can_retry = retry;
    m.page_id = Page::Message;
    s_retry = retry_cmd;
}

void set_note(const char *text) {
    Guard g;
    std::snprintf(model().player_note, sizeof(model().player_note), "%s", text ? text : "");
}

bool post(Cmd cmd, uint16_t arg = 0, bool interrupt = false) {
    if (!s_queue || shutting_down()) return false;
    if (interrupt) s_interrupt.store(true, std::memory_order_release);
    const Command c{cmd, arg};
    return xQueueSend(s_queue, &c, 0) == pdTRUE;
}

// Interruptible sleep for the music task.
bool nap(uint32_t ms, bool stop_on_command) {
    for (uint32_t waited = 0; waited < ms; waited += 100) {
        if (shutting_down() || (stop_on_command && uxQueueMessagesWaiting(s_queue))) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !shutting_down();
}

bool wait_wifi() {
    if (WifiManager::GetInstance().IsConnected()) return true;
    show_message("Wi-Fi 未连接，正在等待网络", false);
    while (!WifiManager::GetInstance().IsConnected() && !browse_cancelled()) {
        if (!nap(500, false)) return false;
    }
    return !browse_cancelled();
}

const char *network_text(Result r) {
    switch (r) {
    case Result::NoMemory: return "内存暂时不足 · 确定重试";
    case Result::Api: return "网易云返回错误 · 确定重试";
    default: return "网络错误 · 确定重试";
    }
}

// ---- lists ------------------------------------------------------------------

void load_lists(bool personal = true) {
    size_t n = 0;
    Playlist favorites{};
    {
        // The UI only reads lists[0, list_count); fill them while it is 0.
        Guard g;
        model().list_count = 0;
        model().open_list = -1;
    }
    if (personal && s_cookie[0] && s_uid) {
        show_message("正在加载歌单", false);
        const Result r = user_playlists(s_cookie, s_uid, model().lists + kHomeEntries,
                                       kMaxLists - kHomeEntries, &n, &favorites, browse_cancelled);
        if (r != Result::Ok) {
            if (r != Result::Cancelled) show_message(network_text(r), true, Cmd::Boot);
            return;
        }
    }
    Guard g;
    if (browse_cancelled()) return;
    Model &m = model();
    fill_home_entries(m.lists, favorites);
    m.list_count = static_cast<uint8_t>(n + kHomeEntries);
    m.list_selected = 0;
    m.page_id = Page::Lists;
    s_lists_pending = !personal && s_cookie[0] && s_uid;
    ESP_LOGI(kTag, "Home lists ready: +%d ms (%u lists)", elapsed_ms(), static_cast<unsigned>(m.list_count));
}

void forget_login() {
    s_lists_pending = false;
    if (s_urls) s_urls->clear();
    store_erase(kNs, "cookie");
    store_erase(kNs, "uid");
    s_cookie[0] = 0;
    s_uid = 0;
    Guard g;
    model().logged_in = false;
    model().nickname[0] = 0;
}

bool finish_login(bool personal_lists = true) {
    char nick[sizeof(Model::nickname)];
    uint64_t uid = 0;
    const Result r = account(s_cookie, &uid, nick, sizeof(nick), browse_cancelled);
    ESP_LOGI(kTag, "Account check done: +%d ms (%s)", elapsed_ms(), result_text(r));
    if (r == Result::Api) {
        forget_login();
        return false;
    }
    if (r != Result::Ok) {
        if (r != Result::Cancelled) show_message(network_text(r), true, Cmd::Boot);
        return true;  // keep the cookie; the network may come back
    }
    s_uid = uid;
    char text[24];
    std::snprintf(text, sizeof(text), "%" PRIu64, uid);
    store_set_string(kNs, "uid", text);
    {
        Guard g;
        model().logged_in = true;
        std::snprintf(model().nickname, sizeof(model().nickname), "%s", nick);
    }
    load_lists(personal_lists);
    return true;
}

void login_flow() {
    for (unsigned code_round = 0; code_round < 5 && !browse_cancelled(); ++code_round) {
        char key[64];
        const Result r = qr_key(key, sizeof(key), login_cancelled);
        if (r == Result::Cancelled) return;
        if (r != Result::Ok) { show_message(network_text(r), true, Cmd::Login); return; }
        {
            Guard g;
            if (browse_cancelled()) return;
            Model &m = model();
            // chainId ties the scan to this device's PC-client login.
            std::snprintf(m.qr_url, sizeof(m.qr_url), "https://music.163.com/login?codekey=%s&chainId=v1_%s_pc_login_%lld",
                          key, s_device_id, static_cast<long long>(esp_timer_get_time() / 1000 + 1700000000000LL));
            std::snprintf(m.login_status, sizeof(m.login_status), "等待扫码");
            m.page_id = Page::Login;
        }
        unsigned errors = 0;
        // A code lives about two minutes; 800 ends it earlier.
        for (unsigned poll = 0; poll < 60; ++poll) {
            if (!nap(3000, true)) return;
            int code = 0;
            const Result p = qr_poll(key, &code, s_cookie, kCookieBytes, login_cancelled);
            if (p == Result::Cancelled) return;
            if (p == Result::Api && code == 803) {
                // The code is spent once confirmed; polling it again looks abusive.
                show_message("已确认但未取到登录信息 · 确定重试", true, Cmd::Login);
                return;
            }
            if (p == Result::Api && code == 8821) {
                // Polling the same code again only repeats the refusal.
                show_message("扫码登录被网易云拦截 · 确定重试", true, Cmd::Login);
                return;
            }
            if (p != Result::Ok) {
                if (++errors > 5) { show_message(network_text(p), true, Cmd::Login); return; }
                continue;
            }
            errors = 0;
            if (code == 800) {
                Guard g;
                std::snprintf(model().login_status, sizeof(model().login_status), "二维码已过期，正在刷新");
                break;
            }
            if (code == 802) {
                Guard g;
                std::snprintf(model().login_status, sizeof(model().login_status), "已扫码，请在手机上确认");
            }
            if (code == 803) {
                store_set_string(kNs, "cookie", s_cookie);
                show_message("登录成功，正在加载", false);
                if (!finish_login()) show_message("登录失败 · 确定重试", true, Cmd::Login);
                return;
            }
        }
    }
    show_message("二维码已过期 · 确定刷新", true, Cmd::Login);
}

// Stable random id for the PC-client login fields.
void load_device_id() {
    std::string id = store_get_string(kNs, "device", "");
    if (id.size() != 32) {
        char hex[33];
        for (int i = 0; i < 32; i += 8) std::snprintf(hex + i, 9, "%08lX", static_cast<unsigned long>(esp_random()));
        id = hex;
        store_set_string(kNs, "device", id);
    }
    std::snprintf(s_device_id, sizeof(s_device_id), "%s", id.c_str());
    set_device_id(s_device_id);
}

void boot() {
    load_device_id();
    if (!wait_wifi()) return;
    const std::string saved = store_get_string(kNs, "cookie", "");
    if (saved.size() >= kCookieBytes) store_erase(kNs, "cookie");
    std::snprintf(s_cookie, kCookieBytes, "%s", saved.size() < kCookieBytes ? saved.c_str() : "");
    // Search and daily recommendations don't use the user's playlist index.
    // Keep account validation, then go straight to the requested queue.
    const bool personal_lists = !s_intent ||
        (s_intent->source != voice_intent::Source::Search && s_intent->source != voice_intent::Source::Daily);
    if (s_cookie[0]) {
        show_message("正在登录网易云", false);
        if (finish_login(personal_lists)) return;
    }
    load_lists(personal_lists);
}

// ---- tracks -----------------------------------------------------------------

// Writes model().page directly: the UI ignores it while page_count is 0, and
// only this task or the single page task (never both) fills it.
void fetch_page(uint16_t start) {
    uint64_t ids[kPageSize];
    size_t n = 0;
    {
        Guard g;
        Model &m = model();
        if (start >= m.track_count) return;
        n = m.track_count - start < kPageSize ? m.track_count - start : kPageSize;
        std::memcpy(ids, m.ids + start, n * sizeof(uint64_t));
        m.page_loading = true;
        m.page_count = 0;
    }
    const Result r = song_details(ids, n, model().page, browse_cancelled);
    Guard g;
    Model &m = model();
    m.page_loading = false;
    if (r != Result::Ok) return;
    m.page_start = start;
    m.page_count = static_cast<uint8_t>(n);
}

void meta_task(void *) {
    fetch_page(s_meta_start.load());
    s_meta_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

// Fisher-Yates over ids[first, n) and, when given, the matching display rows.
void shuffle_tracks(uint64_t *ids, Song *rows, size_t first, size_t n) {
    for (size_t i = n; i > first + 1; --i) {
        const size_t j = first + esp_random() % (i - first);
        std::swap(ids[i - 1], ids[j]);
        if (rows) std::swap(rows[i - 1], rows[j]);
    }
}

void open_list(uint16_t index, bool shuffle = false) {
    uint64_t id = 0;
    PlaylistKind kind = PlaylistKind::Playlist;
    bool needs_login = false;
    {
        Guard g;
        Model &m = model();
        if (index >= m.list_count) return;
        kind = m.lists[index].kind;
        needs_login = kind != PlaylistKind::Playlist && !m.logged_in;
        if (m.open_list == static_cast<int8_t>(index) && m.track_count &&
            (kind != PlaylistKind::Daily || m.session || m.paused)) {
            m.page_id = Page::Tracks;
            return;
        }
        id = m.lists[index].id;
        m.now_index = -1;
        m.paused = false;
        m.player_note[0] = 0;
    }
    if (needs_login) { login_flow(); return; }
    if (kind == PlaylistKind::Favorites && !id) {
        show_message("未找到喜欢的歌单 · 确定重试", true, Cmd::Boot);
        return;
    }
    show_message("正在加载歌曲", false);
    size_t n = 0;
    const Result r = kind == PlaylistKind::Daily
        ? daily_songs(s_cookie, model().ids, kMaxTracks, &n, browse_cancelled)
        : playlist_tracks(s_cookie, id, model().ids, kMaxTracks, &n, browse_cancelled);
    if (browse_cancelled()) return;
    if (r != Result::Ok) {
        if (r != Result::Cancelled) show_message(network_text(r), true, Cmd::OpenList);
        Guard g;
        model().open_list = -1;
        model().track_count = 0;
        return;
    }
    // Before track_count is published: the UI does not read ids yet.
    if (shuffle) shuffle_tracks(model().ids, nullptr, 0, n);
    {
        Guard g;
        Model &m = model();
        m.open_list = static_cast<int8_t>(index);
        m.track_count = static_cast<uint16_t>(n);
        m.track_selected = n ? 1 : 0;
        m.page_count = 0;
        m.page_start = 0;
    }
    fetch_page(0);
    Guard g;
    if (browse_cancelled()) return;
    model().page_id = n ? Page::Tracks : Page::Message;
    if (!n) std::snprintf(model().message, sizeof(model().message), "%s",
                          kind == PlaylistKind::Daily ? "暂时没有每日推荐" : "这个歌单是空的");
    model().can_retry = false;
}

// ---- playback ---------------------------------------------------------------

struct Track {
    uint64_t id = 0;
    Song song{};
    char url[kUrlBytes]{};
    int64_t url_at = 0;
    bool downgraded = false;  // url was rewritten from https:// to http://
};

// The audio CDN normally serves plain HTTP too, which keeps TLS away from the
// decoder. Rewrites in place (no second 512 B buffer on the music stack).
void prefer_plain_http(Track &t) {
    t.downgraded = false;
    if (std::strncmp(t.url, "https://", 8) || !std::strstr(t.url, ".music.126.net/")) return;
    std::memmove(t.url + 4, t.url + 5, std::strlen(t.url + 5) + 1);
    t.downgraded = true;
}

void restore_https(Track &t) {
    const size_t n = std::strlen(t.url);
    if (!t.downgraded || n + 2 > sizeof(t.url)) return;
    std::memmove(t.url + 5, t.url + 4, n - 4 + 1);
    t.url[4] = 's';
    t.downgraded = false;
}

// Song detail, lyrics (both HTTP) then the play URL (HTTPS). Decoder is closed.
Result prepare(uint16_t index, Track &t) {
    player_reset_status();
    {
        Guard g;
        Model &m = model();
        if (index >= m.track_count) return Result::Api;
        t.id = m.ids[index];
        t.song = {};
        if (m.page_count && index >= m.page_start && index < m.page_start + m.page_count)
            t.song = m.page[index - m.page_start];
        m.now_index = static_cast<int16_t>(index);
        m.paused = false;
        m.now = t.song;
        m.now.id = t.id;
        m.lyrics_ready = false;
    }
    if (!t.song.name[0]) {
        const uint64_t id = t.id;
        if (song_details(&id, 1, &t.song, interrupted) == Result::Cancelled) return Result::Cancelled;
        Guard g;
        model().now = t.song;
        model().now.id = t.id;
    }
    // The UI does not read the lyric pool while lyrics_ready is false.
    lyric(t.id, *s_lyrics, interrupted);
    {
        Guard g;
        model().lyrics_ready = true;
    }
    bool trial = false;
    Result r = Result::Ok;
    const SongUrl *cached = s_urls ? s_urls->find(t.id, esp_timer_get_time()) : nullptr;
    if (!cached && s_urls) {
        uint64_t ids[UrlCache::kCount]{};
        size_t n = 0;
        {
            Guard g;
            const Model &m = model();
            for (int delta : {0, 1, -1}) {
                const auto id = m.ids[(index + m.track_count + delta) % m.track_count];
                bool duplicate = false;
                for (size_t i = 0; i < n; ++i) duplicate |= ids[i] == id;
                if (!duplicate) ids[n++] = id;
            }
        }
        s_urls->clear();
        r = song_urls(s_cookie, ids, n, s_urls->entries, interrupted);
        if (r == Result::Ok) s_urls->fetched_at = esp_timer_get_time();
        cached = s_urls->find(t.id, esp_timer_get_time());
    }
    if (cached) {
        std::snprintf(t.url, sizeof(t.url), "%s", cached->url);
        trial = cached->trial;
    } else if (!s_urls) r = song_url(s_cookie, t.id, t.url, sizeof(t.url), &trial, interrupted);
    else if (r == Result::Ok) r = Result::Api;
    t.url_at = esp_timer_get_time();
    if (r == Result::Ok) prefer_plain_http(t);
    if (r == Result::Ok && trial) set_note("非会员仅可试听片段");
    return r;
}

uint16_t step_index(uint16_t index, int delta) {
    uint16_t count;
    {
        Guard g;
        count = model().track_count;
    }
    if (!count) return 0;
    return static_cast<uint16_t>((index + count + delta) % count);
}

// Only called outside a playback session, after its HTTP worker/decoder join.
bool service_reminder_audio() {
    if (!reminders::sound_pending() || shutting_down()) return !shutting_down();
    player_deinit(); s_player_ready = false;
    reminders::play_music_notice(shutting_down);
    if (shutting_down()) return false;
    s_player_ready = player_init();
    if (!s_player_ready) {
        show_message("音频恢复失败，请退出后重新打开", false);
        s_shutdown = true; s_active = false;
        ESP_LOGE(kTag, "Audio could not resume after reminder; exit and reopen music");
    }
    return s_player_ready;
}

// Waits while paused. Returns the command that ends the pause.
bool wait_paused(Command &c) {
    {
        Guard g;
        model().paused = true;
    }
    for (;;) {
        if (shutting_down()) return false;
        if (!service_reminder_audio()) return false;
        if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (c.cmd == Cmd::LoadPage) { fetch_page(c.arg); continue; }
        s_interrupt.store(false, std::memory_order_release);
        s_sleep_due.store(false, std::memory_order_release);  // a timer that fired while paused is moot
        Guard g;
        model().paused = false;
        return true;
    }
}

void dispatch(const Command &c);

void play_from(uint16_t index) {
    Track t;
    unsigned failures = 0;
    set_note(nullptr);
    {
        Guard g;
        model().page_id = Page::Player;
    }
    // A track command retargets this loop; anything else leaves playback.
    // Never call play_from() recursively from here.
    const auto retarget = [&](const Command &c) {
        if (c.cmd == Cmd::Play) { index = c.arg; failures = 0; return true; }
        if (c.cmd == Cmd::Next || c.cmd == Cmd::Prev) {
            index = step_index(index, c.cmd == Cmd::Next ? 1 : -1);
            failures = 0;
            return true;
        }
        if (c.cmd == Cmd::Toggle) { failures = 0; return true; }
        {
            Guard g;
            model().now_index = -1;
            model().paused = false;
        }
        dispatch(c);
        return false;
    };
    while (!shutting_down()) {
        if (!service_reminder_audio()) return;
        s_interrupt.store(false, std::memory_order_release);
        {
            Guard g;
            Model &m = model();
            m.session = false;
        }
        const Result prepared = prepare(index, t);
        if (prepared == Result::Cancelled) {
            if (shutting_down()) return;
            Command c;
            if (s_sleep_due.load(std::memory_order_acquire)) {
                // Stop timer between tracks: pause; OK replays this track.
                set_note("定时已到，已暂停");
                if (!wait_paused(c)) return;
                set_note(nullptr);
                if (retarget(c)) continue;
                return;
            }
            if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(500)) != pdTRUE) continue;
            s_interrupt.store(false);
            if (c.cmd == Cmd::LoadPage) { fetch_page(c.arg); continue; }
            if (retarget(c)) continue;
            return;
        }
        if (prepared != Result::Ok) {
            ESP_LOGW(kTag, "Track %u unavailable (%s)", index, result_text(prepared));
            if (prepared == Result::Api && ++failures < kMaxConsecutiveFailures) {
                set_note("已跳过无法播放的歌曲");
                index = step_index(index, 1);
                continue;
            }
            set_note(prepared == Result::Api ? "连续几首都无法播放" : network_text(prepared));
            Command c;
            if (!wait_paused(c)) return;
            if (retarget(c)) continue;
            return;
        }

        uint32_t offset = 0, total = 0;
        unsigned network_retries = 0;
        bool refreshed = false;
        for (;;) {
            {
                Guard g;
                model().session = true;
            }
            const PlayOutcome o = play({t.url, offset, total, t.song.duration_ms, interrupted}, model().tasks);
            {
                Guard g;
                model().session = false;
            }
            total = o.total_bytes;
            if (o.played) { failures = 0; network_retries = 0; }
            if (o.end == PlayEnd::Finished) {
                index = step_index(index, 1);
                break;
            }
            if (o.end == PlayEnd::Interrupted) {
                if (shutting_down()) return;
                if (reminders::sound_pending()) {
                    if (!service_reminder_audio()) return;
                    offset = o.position;
                    continue;
                }
                Command c;
                if (s_sleep_due.load(std::memory_order_acquire)) {
                    // The stop timer pauses exactly like the OK key, so OK resumes
                    // in place (refreshing an expired URL) after any delay.
                    set_note("定时已到，已暂停");
                    c = {Cmd::Toggle, 0};
                } else if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(500)) != pdTRUE) {
                    s_interrupt.store(false);
                    offset = o.position;
                    continue;
                }
                s_interrupt.store(false, std::memory_order_release);
                if (c.cmd == Cmd::Toggle) {
                    if (!wait_paused(c)) return;
                    set_note(nullptr);  // e.g. the stop timer's notice
                    if (c.cmd == Cmd::Toggle) {
                        offset = o.position;
                        // Old CDN URLs expire: refresh before resuming with Range.
                        if (esp_timer_get_time() - t.url_at > kUrlLifetimeUs) {
                            bool trial = false;
                            if (song_url(s_cookie, t.id, t.url, sizeof(t.url), &trial, interrupted) != Result::Ok) {
                                set_note("播放地址获取失败 · 确定重试");
                                continue;
                            }
                            t.url_at = esp_timer_get_time();
                            prefer_plain_http(t);
                        }
                        continue;
                    }
                }
                if (c.cmd == Cmd::LoadPage) { fetch_page(c.arg); offset = o.position; continue; }
                if (retarget(c)) break;
                return;
            }
            if (o.end == PlayEnd::Expired && t.downgraded) {
                // Some signed URLs are only valid over HTTPS.
                ESP_LOGW(kTag, "CDN refused plain HTTP; retrying over HTTPS");
                restore_https(t);
                offset = o.position;
                continue;
            }
            if (o.end == PlayEnd::Expired && !refreshed) {
                if (s_urls) s_urls->clear();
                refreshed = true;
                bool trial = false;
                if (song_url(s_cookie, t.id, t.url, sizeof(t.url), &trial, interrupted) == Result::Ok) {
                    t.url_at = esp_timer_get_time();
                    prefer_plain_http(t);
                    offset = o.position;
                    continue;
                }
            }
            if (o.end == PlayEnd::NetworkLost && network_retries < 3) {
                ++network_retries;
                set_note("网络波动，正在重连");
                if (!nap(1500 * network_retries, true) && shutting_down()) return;
                offset = o.position;
                if (WifiManager::GetInstance().IsConnected()) continue;
            }
            if (o.end == PlayEnd::Failed && ++failures < kMaxConsecutiveFailures) {
                set_note("已跳过无法播放的歌曲");
                index = step_index(index, 1);
                break;
            }
            // Out of retries, memory or connectivity: hold here until the user acts.
            set_note(o.end == PlayEnd::NoMemory ? "内存暂时不足 · 确定重试" : "网络中断 · 确定继续");
            Command c;
            if (!wait_paused(c)) return;
            set_note(nullptr);
            if (c.cmd == Cmd::Toggle) { offset = o.position; network_retries = 0; continue; }
            if (retarget(c)) break;
            return;
        }
        set_note(nullptr);
    }
}

void dispatch(const Command &c) {
    switch (c.cmd) {
    case Cmd::Boot: boot(); break;
    case Cmd::Login: forget_login(); login_flow(); break;
    case Cmd::SkipLogin: load_lists(); break;
    case Cmd::Logout: forget_login(); login_flow(); break;
    case Cmd::BackHome: {
        s_browse_cancel.store(false, std::memory_order_release);
        if (s_lists_pending) { load_lists(); break; }
        Guard g;
        Model &m = model();
        if (!m.list_count) {
            fill_home_entries(m.lists, {});
            m.list_count = kHomeEntries;
            m.list_selected = 0;
        }
        m.can_retry = false;
        m.page_id = Page::Lists;
        break;
    }
    case Cmd::OpenList: open_list(c.arg); break;
    case Cmd::LoadPage: fetch_page(c.arg); break;
    case Cmd::Play: play_from(c.arg); break;
    case Cmd::Toggle: {
        // Paused with no session (e.g. after leaving a failed track): resume it.
        int16_t now;
        {
            Guard g;
            now = model().now_index;
        }
        if (now >= 0) play_from(static_cast<uint16_t>(now));
        break;
    }
    case Cmd::Next:
    case Cmd::Prev: {
        int16_t now;
        {
            Guard g;
            now = model().now_index;
        }
        if (now >= 0) play_from(step_index(static_cast<uint16_t>(now), c.cmd == Cmd::Next ? 1 : -1));
        break;
    }
    }
}

// Search results become a temporary queue with no home entry: ids and the
// display page are filled together, so preparing a track needs no detail call.
// song_id (the result Xiaozhi named) starts the queue; a shuffled queue keeps
// it first and mixes the rest.
void play_search(const char *query, uint64_t song_id, bool shuffle) {
    show_message("正在搜索", false);
    {
        Guard g;
        Model &m = model();
        m.open_list = -1;
        m.track_count = 0;
        m.page_count = 0;
        m.now_index = -1;
        m.paused = false;
        m.player_note[0] = 0;
    }
    // The UI ignores ids and page while track_count and page_count are 0.
    size_t n = 0;
    const bool reused = take_voice_search(query, model().page, model().ids, kPageSize, &n);
    const Result r = reused ? Result::Ok :
        search_songs(query, model().page, model().ids, kPageSize, &n, browse_cancelled);
    if (reused) ESP_LOGI(kTag, "Reused %u voice search results; skipped duplicate HTTP", static_cast<unsigned>(n));
    if (browse_cancelled()) return;
    if (r != Result::Ok) {
        // No retry: the spoken query is gone, so OK returns to the home page.
        show_message(r == Result::Api ? "没有搜到这首歌 · 确定返回" : "搜索失败 · 确定返回", false);
        return;
    }
    Model &m = model();
    if (song_id) {
        size_t at = 0;
        while (at < n && m.ids[at] != song_id) ++at;
        if (at == n) {
            // Results can shift between searches: keep the named song anyway.
            // Its empty row makes prepare() fetch the details.
            at = n < kPageSize ? n++ : n - 1;
            m.ids[at] = song_id;
            m.page[at] = {};
            m.page[at].id = song_id;
        }
        // Rotate it to the front; the rest keep their relevance order.
        for (; at > 0; --at) {
            std::swap(m.ids[at], m.ids[at - 1]);
            std::swap(m.page[at], m.page[at - 1]);
        }
    }
    if (shuffle) shuffle_tracks(m.ids, m.page, song_id ? 1 : 0, n);
    {
        Guard g;
        m.track_count = static_cast<uint16_t>(n);
        m.track_selected = 1;
        m.page_start = 0;
        m.page_count = static_cast<uint8_t>(n);
    }
    if (!uxQueueMessagesWaiting(s_queue)) play_from(0);
}

void sleep_timer_fired(void *) {
    s_sleep_due.store(true, std::memory_order_release);
}

// Armed when the voice request runs, so the minutes count from playback start.
void arm_stop_timer(uint16_t minutes) {
    if (!minutes) return;
    if (!s_sleep_timer) {
        esp_timer_create_args_t args{};
        args.callback = sleep_timer_fired;
        args.name = "music_stop";
        if (esp_timer_create(&args, &s_sleep_timer) != ESP_OK) { s_sleep_timer = nullptr; return; }
    }
    esp_timer_stop(s_sleep_timer);
    s_sleep_due.store(false);
    if (esp_timer_start_once(s_sleep_timer, static_cast<uint64_t>(minutes) * 60 * 1000 * 1000) == ESP_OK)
        ESP_LOGI(kTag, "Voice stop timer: %u min", static_cast<unsigned>(minutes));
}

// A Xiaozhi voice request. A key press or Back during boot means the user took
// over, and an error or login page from boot keeps priority over it.
void play_requested() {
    if (!s_intent) return;
    const voice_intent::Request request = *s_intent;
    delete s_intent;
    s_intent = nullptr;
    using voice_intent::Source;
    if (browse_cancelled() || uxQueueMessagesWaiting(s_queue)) return;
    if (request.source == Source::Search) {
        bool home = false;
        {
            Guard g;
            home = g.ok && model().page_id == Page::Lists;
        }
        if (!home) return;
        arm_stop_timer(request.stop_minutes);
        play_search(request.name, request.song_id, request.shuffle);
        return;
    }
    int index = -1;
    bool needs_login = false;
    {
        Guard g;
        Model &m = model();
        if (!g.ok || m.page_id != Page::Lists) return;
        index = request.source == Source::Favorites ? 0
              : request.source == Source::Daily     ? 1
              : find_playlist(m.lists, m.list_count, request.name);
        needs_login = index >= 0 && m.lists[index].kind != PlaylistKind::Playlist && !m.logged_in;
        if (index >= 0) m.list_selected = static_cast<uint8_t>(index);
    }
    if (index < 0) {
        show_message("没找到这个歌单 · 确定返回", false);
        return;
    }
    if (needs_login) {
        show_message("需要先登录网易云 · 确定扫码", true, Cmd::Login);
        return;
    }
    open_list(static_cast<uint16_t>(index), request.shuffle);
    bool ready = false;
    {
        Guard g;
        const Model &m = model();
        ready = g.ok && m.open_list == index && m.track_count && m.page_id == Page::Tracks;
    }
    if (!ready || browse_cancelled() || uxQueueMessagesWaiting(s_queue)) return;
    arm_stop_timer(request.stop_minutes);
    play_from(0);
}

void music_task(void *) {
    // Boot's TLS handshake is CPU-bound; above the LVGL task it froze the
    // half-drawn first page for ~0.9 s. Let the page paint first.
    const UBaseType_t priority = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, 3);
    dispatch({Cmd::Boot, 0});
    vTaskPrioritySet(nullptr, priority);
    play_requested();
    while (!shutting_down()) {
        // Here nothing is playing: a stop timer firing now has nothing to stop.
        s_sleep_due.store(false, std::memory_order_release);
        if (!service_reminder_audio()) break;
        Command c;
        if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        s_interrupt.store(false, std::memory_order_release);
        dispatch(c);
    }
    s_task_running.store(false, std::memory_order_release);
    xSemaphoreGive(s_stopped);
    vTaskSuspend(nullptr);
}

// ---- lifecycle ----------------------------------------------------------------

void release_all() {
    s_lists_pending = false;
    if (s_task) { vTaskDelete(s_task); s_task = nullptr; }
    if (s_sleep_timer) {
        esp_timer_stop(s_sleep_timer);
        esp_timer_delete(s_sleep_timer);
        s_sleep_timer = nullptr;
    }
    s_sleep_due.store(false);
    delete s_intent;
    s_intent = nullptr;
    if (s_player_ready) { player_deinit(); s_player_ready = false; }
    if (s_lyrics) { s_lyrics->~Lyrics(); s_lyrics = nullptr; }
    delete s_urls;
    s_urls = nullptr;
    std::free(s_cookie);
    s_cookie = nullptr;
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_stopped) { vSemaphoreDelete(s_stopped); s_stopped = nullptr; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = nullptr; }
    if (s_owner) audio_workspace::release(audio_workspace::Owner::Music);
    s_owner = false;
}

void app_init() {}

void app_start() {
    s_started_us = esp_timer_get_time();
    // Taken even if this start fails, so the request cannot fire on a later visit.
    delete s_intent;
    s_intent = nullptr;
    const voice_intent::Request request = voice_intent::take(APP_ID_NETEASE);
    if (request.source != voice_intent::Source::None) s_intent = new (std::nothrow) voice_intent::Request(request);
    if (s_task_running.load() || s_meta_running.load()) {
        // A previous session never finished stopping: leave its resources alone.
        ui_show_unavailable("上次播放仍在退出，请稍后再试");
        return;
    }
    s_shutdown.store(false);
    s_interrupt.store(false);
    s_browse_cancel.store(false);
    s_owner = audio_workspace::acquire(audio_workspace::Owner::Music);
    if (!s_owner) {
        ESP_LOGE(kTag, "Audio workspace busy");
        ui_show_unavailable("音频资源被占用，请稍后再试");
        return;
    }
    static_assert(sizeof(Model) <= audio_workspace::music::kModelBytes);
    static_assert(alignof(Model) <= 16);
    std::memset(audio_workspace::music::model(), 0, audio_workspace::music::kModelBytes);
    Model &m = model();
    m.open_list = -1;
    m.now_index = -1;
    m.page_id = Page::Message;
    std::snprintf(m.message, sizeof(m.message), "正在启动");
    s_lock = xSemaphoreCreateMutex();
    s_stopped = xSemaphoreCreateBinary();
    s_queue = xQueueCreate(8, sizeof(Command));
    s_cookie = static_cast<char *>(std::calloc(1, kCookieBytes));
    s_urls = new (std::nothrow) UrlCache;  // optional; single-song fallback on OOM
    s_lyrics = new (m.lyrics_object) Lyrics(m.lyrics, lyrics_bytes());
    s_player_ready = s_lock && s_stopped && s_queue && s_cookie && player_init();
    if (!s_player_ready) {
        ESP_LOGE(kTag, "Start failed. free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        release_all();
        ui_show_unavailable("内存不足，请稍后再试");
        return;
    }
    ui_create(s_lyrics);
    s_active.store(true);
    s_task_running.store(true);
    s_task = xTaskCreateStatic(music_task, "netease", kMusicStackBytes,
                               nullptr, 6, reinterpret_cast<StackType_t *>(audio_workspace::storage),
                               reinterpret_cast<StaticTask_t *>(m.tasks.music_tcb));
    if (!s_task) {
        s_task_running.store(false);
        s_active.store(false);
        ui_destroy();
        release_all();
        ui_show_unavailable("启动失败，请稍后再试");
        return;
    }
}

bool app_prepare_stop() {
    s_active.store(false);
    s_shutdown.store(true, std::memory_order_release);
    s_interrupt.store(true, std::memory_order_release);
    return !s_task_running.load(std::memory_order_acquire) && !s_meta_running.load(std::memory_order_acquire);
}

void app_stop() {
    app_prepare_stop();
    for (int waited = 0; (s_task_running.load() || s_meta_running.load()) && waited < 8000; waited += 50)
        vTaskDelay(pdMS_TO_TICKS(50));
    ui_destroy();
    if (s_task_running.load() || s_meta_running.load()) {
        // Never free a stack or buffer a running task still uses. The next
        // start refuses to run until this session has really ended.
        ESP_LOGE(kTag, "Music task did not stop; keeping its resources");
        return;
    }
    if (s_stopped) xSemaphoreTake(s_stopped, 0);
    release_all();
}

// ---- keys -------------------------------------------------------------------

void request_page(Model &m) {
    if (!m.track_selected || m.page_loading) return;
    const uint16_t index = m.track_selected - 1;
    const uint16_t start = static_cast<uint16_t>(index / kPageSize * kPageSize);
    if (m.page_count && start == m.page_start) return;
    if (!m.session) {
        m.page_loading = true;
        post(Cmd::LoadPage, start);
        return;
    }
    // The decoder is busy: fetch over plain HTTP beside it, only with headroom.
    bool idle = false;
    if (esp_get_free_heap_size() < 24 * 1024 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8 * 1024 ||
        !s_meta_running.compare_exchange_strong(idle, true)) {
        std::snprintf(m.player_note, sizeof(m.player_note), "播放中暂不能翻页");
        return;
    }
    s_meta_start.store(start);
    m.page_loading = true;
    if (xTaskCreate(meta_task, "netease_page", kMetaStackBytes, nullptr, 4, nullptr) != pdPASS) {
        m.page_loading = false;
        s_meta_running.store(false);
    }
}

void move(uint16_t &value, uint16_t count, int delta) {
    if (!count) return;
    value = static_cast<uint16_t>((value + count + delta) % count);
}

bool app_on_back() {
    if (!s_active.load()) return false;  // failed startup must remain escapable
    if (!model_lock(200)) return true;  // busy model must never cause an accidental exit
    Model &m = model();
    if (m.page_id == Page::Lists) { model_unlock(); return false; }
    if (m.page_id == Page::Login || m.page_id == Page::Message) {
        // Cancel the in-flight login/list request before rebuilding the home
        // view. Its late response must not reopen the page after Back.
        s_browse_cancel.store(true, std::memory_order_release);
        xQueueReset(s_queue);
        post(Cmd::BackHome);
        m.page_id = Page::Message;
        m.can_retry = false;
        std::snprintf(m.message, sizeof(m.message), "正在返回");
    } else {
        m.page_id = back_page(m.page_id, m.open_list >= 0 && m.track_count);
        m.volume_notice = false;
        if (m.page_id == Page::Tracks && m.now_index >= 0) {
            m.track_selected = static_cast<uint16_t>(m.now_index + 1);
            request_page(m);
        }
    }
    ++m.revision;
    model_unlock();
    return true;
}

void app_on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (!s_active.load() || (event != BTN_EVT_CLICK && event != BTN_EVT_LONG)) return;
    if (!model_lock(200)) return;
    Model &m = model();
    const bool click = event == BTN_EVT_CLICK;
    const int delta = btn == BSP_BTN_UP ? -1 : 1;
    switch (m.page_id) {
    case Page::Message:
        if (click && btn == BSP_BTN_OK && m.can_retry) {
            m.can_retry = false;
            std::snprintf(m.message, sizeof(m.message), "正在重试");
            post(s_retry == Cmd::OpenList ? Cmd::OpenList : s_retry, m.list_selected);
        } else if (click && btn == BSP_BTN_OK && m.list_count && !m.can_retry) {
            m.page_id = Page::Lists;
        }
        break;
    case Page::Login:
        if (click && btn == BSP_BTN_OK) post(Cmd::SkipLogin);
        break;
    case Page::Lists: {
        const uint16_t rows = m.list_count + 1u;  // + login/logout
        if (click && btn != BSP_BTN_OK) {
            uint16_t sel = m.list_selected;
            move(sel, rows, delta);
            m.list_selected = static_cast<uint8_t>(sel);
        } else if (click) {
            if (m.list_selected < m.list_count) {
                const bool other = m.open_list != static_cast<int8_t>(m.list_selected);
                // Browsing the active queue must not enqueue a command: even a
                // non-interrupting command makes the audio reader yield. In
                // particular, keep today's recommendation IDs while playing.
                if (!other && m.track_count && m.now_index >= 0) m.page_id = Page::Tracks;
                else post(Cmd::OpenList, m.list_selected, other && (m.session || m.paused));
            } else {
                post(m.logged_in ? Cmd::Logout : Cmd::Login, 0, true);
            }
        } else if (btn == BSP_BTN_UP && m.now_index >= 0) {
            m.page_id = Page::Player;
        }
        break;
    }
    case Page::Tracks:
        if (click && btn != BSP_BTN_OK) {
            move(m.track_selected, m.track_count + 1u, delta);
            request_page(m);
        } else if (click) {
            if (!m.track_selected) m.page_id = Page::Lists;
            else if (m.now_index == static_cast<int>(m.track_selected - 1) && (m.session || m.paused))
                m.page_id = Page::Player;
            else {
                m.page_id = Page::Player;
                post(Cmd::Play, m.track_selected - 1u, true);
            }
        } else if (btn == BSP_BTN_UP && m.now_index >= 0) {
            m.page_id = Page::Player;
        } else if (btn == BSP_BTN_DOWN) {
            move(m.track_selected, m.track_count + 1u, 10);
            request_page(m);
        }
        break;
    case Page::Player:
        switch (player_action(btn, event)) {
        case PlayerAction::Previous: post(Cmd::Prev, 0, m.session); break;
        case PlayerAction::Next: post(Cmd::Next, 0, m.session); break;
        case PlayerAction::Toggle: post(Cmd::Toggle, 0, m.session); break;
        case PlayerAction::VolumeUp:
        case PlayerAction::VolumeDown:
            settings_set_volume(stepped_volume(bsp_audio_get_master_volume(), btn == BSP_BTN_UP ? 10 : -10));
            m.volume_notice = true;
            m.volume_until_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 3000;
            break;
        case PlayerAction::None: break;
        }
        break;
    }
    ++m.revision;
    model_unlock();
}

}  // namespace

Model &model() { return *reinterpret_cast<Model *>(audio_workspace::music::model()); }

bool model_lock(uint32_t timeout_ms) {
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void model_unlock() { xSemaphoreGive(s_lock); }

}  // namespace netease

extern const passport_app_t g_netease_app;
const passport_app_t g_netease_app = {
    .id = APP_ID_NETEASE,
    .name = "音乐播放器",
    .en_name = "MUSIC",
    .desc = "扫码登录，收听我的歌单",
    .tag = "Wi-Fi",
    .theme_color = 0xE60026,
    .init = netease::app_init,
    .start = netease::app_start,
    .stop = netease::app_stop,
    .on_key = netease::app_on_key,
    .prepare_stop = netease::app_prepare_stop,
    .on_back = netease::app_on_back,
};
