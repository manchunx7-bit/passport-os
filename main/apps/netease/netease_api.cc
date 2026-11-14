#include "apps/netease/netease_api.h"
#include "apps/netease/netease_catalog.h"
#include "apps/netease/netease_url_cache.h"

#include "apps/netease/netease_json.h"
#include "apps/netease/netease_lyrics.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "mbedtls/aes.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <strings.h>

namespace netease {

namespace {

constexpr char kTag[] = "netease_api";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
constexpr char kDesktopAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Safari/537.36 "
    "Chrome/91.0.4472.164 NeteaseMusicDesktop/3.1.29.205117";
constexpr char kOsVersion[] = "Microsoft-Windows-10-Professional-build-19045-64bit";
constexpr char kAppVersion[] = "3.1.17.204416";
constexpr int64_t kRequestDeadlineUs = 20 * 1000 * 1000;
char s_device_id[40] = "";
// TLS needs one ~9 KiB record buffer plus handshake scratch. Refuse early
// rather than fail half way through a handshake on a fragmented heap.
constexpr size_t kTlsLargestBlock = 12 * 1024;
constexpr size_t kTlsFree = 36 * 1024;

struct Request {
    char *cookie_out;
    size_t cookie_capacity;
};

esp_err_t on_event(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data || !event->header_key || !event->header_value)
        return ESP_OK;
    auto &request = *static_cast<Request *>(event->user_data);
    if (!request.cookie_out || strcasecmp(event->header_key, "Set-Cookie")) return ESP_OK;
    const char *value = event->header_value;
    if (std::strncmp(value, "MUSIC_U=", 8) || value[8] == ';' || !value[8]) return ESP_OK;
    const char *end = std::strchr(value, ';');
    const size_t n = end ? static_cast<size_t>(end - value) : std::strlen(value);
    if (n < request.cookie_capacity) {
        std::memcpy(request.cookie_out, value, n);
        request.cookie_out[n] = 0;
    }
    return ESP_OK;
}

// GET (or POST a form body) and stream the reply into scanner.
// cookie: the full Cookie header value, or null.
Result request_json(const char *url, const char *cookie, const char *form, const char *agent,
                    JsonScanner &scanner, Cancelled cancelled, char *cookie_out, size_t cookie_capacity) {
    if (cancelled && cancelled()) return Result::Cancelled;
    const bool tls = std::strncmp(url, "https://", 8) == 0;
    if (tls && (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kTlsLargestBlock ||
                esp_get_free_heap_size() < kTlsFree)) {
        ESP_LOGW(kTag, "Skip HTTPS: free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return Result::NoMemory;
    }
    Request request{cookie_out, cookie_capacity};
#ifdef ESP_PLATFORM
    // Let the lower-priority LVGL task flush any pending screen/status change
    // before this priority-6 worker blocks on DNS/TCP/TLS.
    vTaskDelay(pdMS_TO_TICKS(15));
    if (cancelled && cancelled()) return Result::Cancelled;
#endif
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.timeout_ms = 5000;
    cfg.buffer_size = tls ? 1024 : 4096;
    cfg.buffer_size_tx = 2048;  // request line + ~1.1 KB login cookie
    cfg.user_agent = agent;
    cfg.disable_auto_redirect = true;
    cfg.keep_alive_enable = false;
    cfg.event_handler = on_event;
    cfg.user_data = &request;
    if (tls) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return Result::NoMemory;
    if (!form) esp_http_client_set_header(client, "Referer", "https://music.163.com/");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    const size_t form_length = form ? std::strlen(form) : 0;
    if (form) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    }
    // No extra cookies: "os=pc" alone makes the QR endpoint answer -462 (risk).
    if (cookie && cookie[0]) esp_http_client_set_header(client, "Cookie", cookie);

    Result result = Result::Network;
    const int64_t deadline = esp_timer_get_time() + kRequestDeadlineUs;
    do {
        if (esp_http_client_open(client, static_cast<int>(form_length)) != ESP_OK) break;
        if (form_length && esp_http_client_write(client, form, static_cast<int>(form_length)) !=
                               static_cast<int>(form_length)) break;
        if (esp_http_client_fetch_headers(client) < 0) break;
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(kTag, "HTTP %d", status);
            result = Result::Http;
            break;
        }
        char chunk[512];
        for (;;) {
            if (cancelled && cancelled()) { result = Result::Cancelled; break; }
            if (esp_timer_get_time() > deadline) { result = Result::Network; break; }
            const int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n < 0) { result = Result::Network; break; }
            if (n == 0) {
                result = esp_http_client_is_complete_data_received(client) && scanner.complete() ? Result::Ok
                         : scanner.failed() ? Result::Parse : Result::Network;
                break;
            }
            if (!scanner.feed(chunk, static_cast<size_t>(n))) { result = Result::Parse; break; }
        }
    } while (false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result != Result::Ok && result != Result::Cancelled) ESP_LOGW(kTag, "GET failed (%s)", result_text(result));
    return result;
}

Result get_json(const char *url, const char *cookie, JsonScanner &scanner, Cancelled cancelled,
                char *cookie_out = nullptr, size_t cookie_capacity = 0) {
    return request_json(url, cookie, nullptr, kUserAgent, scanner, cancelled, cookie_out, cookie_capacity);
}

// ---- eapi (PC client) ---------------------------------------------------------
// params = HEX(AES-128-ECB(path "-36cd479b6b5-" json "-36cd479b6b5-" md5(...)))

int64_t epoch_seconds() {
    const time_t now = std::time(nullptr);
    // Before SNTP the client fields still need a plausible timestamp.
    return now > 1700000000 ? static_cast<int64_t>(now) : 1700000000 + esp_timer_get_time() / 1000000;
}

char *eapi_form(const char *path, const char *json) {
    static constexpr char kSep[] = "-36cd479b6b5-";
    static constexpr uint8_t kKey[16] = {'e', '8', '2', 'c', 'k', 'e', 'n', 'h',
                                         '8', 'd', 'i', 'c', 'h', 'e', 'n', '8'};
    md5_context_t md5;
    esp_rom_md5_init(&md5);
    esp_rom_md5_update(&md5, "nobody", 6);
    esp_rom_md5_update(&md5, path, std::strlen(path));
    esp_rom_md5_update(&md5, "use", 3);
    esp_rom_md5_update(&md5, json, std::strlen(json));
    esp_rom_md5_update(&md5, "md5forencrypt", 13);
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);

    const size_t plain = std::strlen(path) + std::strlen(json) + 2 * (sizeof(kSep) - 1) + 32;
    const size_t padded = (plain / 16 + 1) * 16;  // PKCS#7 always adds 1..16 bytes
    std::unique_ptr<uint8_t[]> block(new (std::nothrow) uint8_t[padded]);
    char *form = static_cast<char *>(std::malloc(7 + padded * 2 + 1));
    if (!block || !form) { std::free(form); return nullptr; }
    std::snprintf(reinterpret_cast<char *>(block.get()), padded, "%s%s%s%s%s", path, kSep, json, kSep, hex);
    std::memset(block.get() + plain, static_cast<int>(padded - plain), padded - plain);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, kKey, 128);
    std::memcpy(form, "params=", 7);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t off = 0; off < padded; off += 16) {
        uint8_t out[16];
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block.get() + off, out);
        for (int i = 0; i < 16; ++i) {
            form[7 + (off + i) * 2] = kHex[out[i] >> 4];
            form[7 + (off + i) * 2 + 1] = kHex[out[i] & 15];
        }
    }
    mbedtls_aes_free(&aes);
    form[7 + padded * 2] = 0;
    return form;
}

// POST to interfacepc.music.163.com/eapi/... as the desktop client. fields is
// the request's own JSON members, e.g. "\"type\":3".
Result eapi_post(const char *path, const char *fields, JsonScanner &scanner, Cancelled cancelled,
                 char *cookie_out = nullptr, size_t cookie_capacity = 0, const char *login = nullptr,
                 bool large_reply = false) {
    const char *music_u = login && !std::strncmp(login, "MUSIC_U=", 8) ? login + 8 : nullptr;
    const int64_t now = epoch_seconds();
    char request_id[32];
    std::snprintf(request_id, sizeof(request_id), "%lld_%04u", static_cast<long long>(now * 1000 + (esp_timer_get_time() / 1000) % 1000),
                  static_cast<unsigned>(esp_random() % 1000));
    const size_t kJsonBytes = 640 + (music_u ? std::strlen(music_u) + 16 : 0);
    std::unique_ptr<char[]> json(new (std::nothrow) char[kJsonBytes]);
    std::unique_ptr<char[]> cookie(new (std::nothrow) char[kJsonBytes]);
    if (!json || !cookie) return Result::NoMemory;
    std::snprintf(json.get(), kJsonBytes,
                  "{%s,\"e_r\":false,\"header\":{\"osver\":\"%s\",\"deviceId\":\"%s\",\"os\":\"pc\","
                  "\"appver\":\"%s\",\"versioncode\":\"140\",\"mobilename\":\"\",\"buildver\":\"%lld\","
                  "\"resolution\":\"1920x1080\",\"__csrf\":\"\",\"channel\":\"netease\",\"requestId\":\"%s\"%s%s%s}}",
                  fields, kOsVersion, s_device_id, kAppVersion, static_cast<long long>(now), request_id,
                  music_u ? ",\"MUSIC_U\":\"" : "", music_u ? music_u : "", music_u ? "\"" : "");
    std::snprintf(cookie.get(), kJsonBytes,
                  "osver=%s; deviceId=%s; os=pc; appver=%s; versioncode=140; mobilename=; buildver=%lld; "
                  "resolution=1920x1080; __csrf=; channel=netease; requestId=%s%s%s",
                  kOsVersion, s_device_id, kAppVersion, static_cast<long long>(now), request_id,
                  music_u ? "; MUSIC_U=" : "", music_u ? music_u : "");
    char *form = eapi_form(path, json.get());
    if (!form) return Result::NoMemory;
    json.reset();
    char url[96];
    // Large authenticated lists share the documented HTTP tradeoff. Login and
    // playback URLs remain HTTPS. Eapi encryption does not protect the Cookie header.
    std::snprintf(url, sizeof(url), "%s://interfacepc.music.163.com/eapi/%s",
                  large_reply ? "http" : "https", path + 5);  // drop "/api/"
    const Result r = request_json(url, cookie.get(), form, kDesktopAgent, scanner, cancelled, cookie_out,
                                  cookie_capacity);
    std::free(form);
    return r;
}

uint64_t to_u64(const char *s) { return std::strtoull(s, nullptr, 10); }

void copy(char *dst, size_t capacity, const char *src) {
    if (capacity) std::snprintf(dst, capacity, "%s", src);
}

bool is(const char *path, const char *want) { return std::strcmp(path, want) == 0; }

// ---- per-endpoint collectors -------------------------------------------------

struct CodeOnly { int code = 0; };

}  // namespace

const char *browser_agent() { return kUserAgent; }

const char *result_text(Result result) {
    switch (result) {
    case Result::Ok: return "ok";
    case Result::Cancelled: return "cancelled";
    case Result::Network: return "network";
    case Result::Http: return "http";
    case Result::Api: return "api";
    case Result::Parse: return "parse";
    case Result::NoMemory: return "no memory";
    }
    return "?";
}

namespace {
struct QrKeyCtx { int code = 0; char *key; size_t capacity; };
void scanner_sink_qr_key(void *c, JsonScanner::Event e, const char *path, const char *value, size_t) {
    auto &ctx = *static_cast<QrKeyCtx *>(c);
    if (e != JsonScanner::Event::Value) return;
    if (is(path, "code")) ctx.code = std::atoi(value);
    else if (is(path, "unikey")) copy(ctx.key, ctx.capacity, value);
}

#ifdef ESP_PLATFORM
struct SessionCache {
    static constexpr uint32_t kMagic = 0x4E455343;  // "NESC"
    static constexpr size_t kMaxPlaylists = 19;     // kMaxLists - kHomeEntries
    static constexpr int64_t kTtlUs = 30LL * 60 * 1000 * 1000;
    uint32_t magic;
    uint32_t check;
    int64_t saved_us;
    uint32_t cookie_hash;
    uint32_t cookie_len;
    uint64_t uid;
    char nickname[40];
    bool has_playlists;
    uint8_t playlist_count;
    Playlist favorites;
    Playlist playlists[kMaxPlaylists];

    static uint32_t hash_cookie(const char *cookie) {
        uint32_t hash = 2166136261u;
        if (!cookie) return hash;
        for (const uint8_t *p = reinterpret_cast<const uint8_t *>(cookie); *p; ++p)
            hash = (hash ^ *p) * 16777619u;
        return hash;
    }

    uint32_t checksum() const {
        uint32_t hash = 2166136261u ^ kMagic;
        const auto mix = [&](const void *data, size_t size) {
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 16777619u;
        };
        mix(&saved_us, sizeof(saved_us));
        mix(&cookie_hash, sizeof(cookie_hash));
        mix(&cookie_len, sizeof(cookie_len));
        mix(&uid, sizeof(uid));
        mix(nickname, sizeof(nickname));
        mix(&has_playlists, sizeof(has_playlists));
        mix(&playlist_count, sizeof(playlist_count));
        mix(&favorites, sizeof(favorites));
        mix(playlists, sizeof(playlists));
        return hash;
    }

    bool valid(const char *cookie) {
        if (!cookie || !cookie[0]) return false;
        if (magic != kMagic || !uid || playlist_count > kMaxPlaylists ||
            nickname[sizeof(nickname) - 1] != 0 || check != checksum()) {
            magic = 0;
            return false;
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us < saved_us) {
            if (esp_reset_reason() == ESP_RST_SW) {
                saved_us = now_us;
                check = checksum();
            } else {
                magic = 0;
                return false;
            }
        } else if (now_us - saved_us > kTtlUs) {
            magic = 0;
            return false;
        }
        return cookie_len == std::strlen(cookie) && cookie_hash == hash_cookie(cookie);
    }

    void save_account(const char *cookie, uint64_t user_id, const char *nick) {
        if (!cookie || !cookie[0] || !user_id) return;
        const bool same_user = valid(cookie) && uid == user_id && has_playlists;
        Playlist saved_fav = same_user ? favorites : Playlist{};
        uint8_t saved_count = same_user ? playlist_count : 0;
        Playlist saved_lists[kMaxPlaylists]{};
        if (same_user) {
            for (size_t i = 0; i < saved_count; ++i) saved_lists[i] = playlists[i];
        }
        *this = {};
        cookie_len = static_cast<uint32_t>(std::strlen(cookie));
        cookie_hash = hash_cookie(cookie);
        uid = user_id;
        copy(nickname, sizeof(nickname), nick ? nick : "");
        has_playlists = same_user;
        playlist_count = saved_count;
        favorites = saved_fav;
        for (size_t i = 0; i < saved_count; ++i) playlists[i] = saved_lists[i];
        saved_us = esp_timer_get_time();
        check = checksum();
        magic = kMagic;
    }

    void save_playlists(const char *cookie, uint64_t user_id, const Playlist *rows,
                        size_t n, const Playlist &fav) {
        if (!valid(cookie) || uid != user_id || n > kMaxPlaylists) return;
        favorites = fav;
        playlist_count = static_cast<uint8_t>(n);
        for (size_t i = 0; i < n; ++i) playlists[i] = rows[i];
        for (size_t i = n; i < kMaxPlaylists; ++i) playlists[i] = {};
        has_playlists = true;
        saved_us = esp_timer_get_time();
        check = checksum();
        magic = kMagic;
    }
};
RTC_NOINIT_ATTR SessionCache s_session_cache;
#endif
}  // namespace

void set_device_id(const char *device_id) { std::snprintf(s_device_id, sizeof(s_device_id), "%s", device_id); }

Result qr_key(char *key, size_t capacity, Cancelled cancelled) {
    QrKeyCtx ctx{0, key, capacity};
    key[0] = 0;
    JsonScanner scanner(scanner_sink_qr_key, &ctx);
    Result r = eapi_post("/api/login/qrcode/unikey", "\"type\":3", scanner, cancelled);
    if (r == Result::Ok && ctx.code == 200 && key[0]) return Result::Ok;
    if (r == Result::Cancelled) return r;
    ESP_LOGW(kTag, "eapi QR key failed (%s, code=%d); trying the web endpoint", result_text(r), ctx.code);
    ctx.code = 0;
    key[0] = 0;
    JsonScanner legacy(scanner_sink_qr_key, &ctx);
    r = get_json("https://music.163.com/api/login/qrcode/unikey?type=1", nullptr, legacy, cancelled);
    if (r != Result::Ok) return r;
    if (ctx.code == 200 && key[0]) return Result::Ok;
    ESP_LOGW(kTag, "QR key refused: code=%d", ctx.code);
    return Result::Api;
}

Result qr_poll(const char *key, int *code, char *cookie, size_t capacity, Cancelled cancelled) {
    // The desktop protocol may return the session in the body ("cookie":
    // "MUSIC_U=...; ...") instead of Set-Cookie. Keep that field (bounded).
    struct Ctx { int code = 0; char *body; size_t used = 0; } ctx{0, static_cast<char *>(std::calloc(1, 2048))};
    if (!ctx.body) return Result::NoMemory;
    JsonScanner scanner([](void *c, JsonScanner::Event e, const char *path, const char *value, size_t length) {
        auto &ctx = *static_cast<Ctx *>(c);
        if (e == JsonScanner::Event::ObjectEnd) return;
        if (e == JsonScanner::Event::Value && is(path, "code")) ctx.code = std::atoi(value);
        if (is(path, "cookie") && ctx.used + length < 2048) {
            std::memcpy(ctx.body + ctx.used, value, length);
            ctx.used += length;
            ctx.body[ctx.used] = 0;
        }
    }, &ctx);
    char fields[96];
    std::snprintf(fields, sizeof(fields), "\"key\":\"%s\",\"type\":3", key);
    if (capacity) cookie[0] = 0;
    const Result r = eapi_post("/api/login/qrcode/client/login", fields, scanner, cancelled, cookie, capacity);
    *code = ctx.code;
    if (ctx.code == 803 && capacity && !cookie[0]) {
        const char *start = std::strstr(ctx.body, "MUSIC_U=");
        if (start && start[8] && start[8] != ';') {
            const char *end = std::strchr(start, ';');
            const size_t n = end ? static_cast<size_t>(end - start) : std::strlen(start);
            if (n < capacity) {
                std::memcpy(cookie, start, n);
                cookie[n] = 0;
            }
        }
    }
    std::free(ctx.body);
    if (r != Result::Ok) return r;
    if (ctx.code == 803 && (!capacity || !cookie[0])) {
        ESP_LOGW(kTag, "Login confirmed but no MUSIC_U cookie");
        return Result::Api;
    }
#ifdef ESP_PLATFORM
    if (ctx.code == 803) s_session_cache.magic = 0;
#endif
    if (ctx.code >= 800 && ctx.code <= 803) return Result::Ok;
    ESP_LOGW(kTag, "QR poll refused: code=%d", ctx.code);
    return Result::Api;
}

Result account(const char *cookie, uint64_t *uid, char *nickname, size_t capacity, Cancelled cancelled) {
    if (cancelled && cancelled()) return Result::Cancelled;
#ifdef ESP_PLATFORM
    if (s_session_cache.valid(cookie)) {
        *uid = s_session_cache.uid;
        if (capacity) copy(nickname, capacity, s_session_cache.nickname);
        return Result::Ok;
    }
#endif
    struct Ctx { int code = 0; uint64_t uid = 0; char *nick; size_t capacity; } ctx{0, 0, nickname, capacity};
    if (capacity) nickname[0] = 0;
    JsonScanner scanner([](void *c, JsonScanner::Event e, const char *path, const char *value, size_t) {
        auto &ctx = *static_cast<Ctx *>(c);
        if (e != JsonScanner::Event::Value) return;
        if (is(path, "code")) ctx.code = std::atoi(value);
        else if (is(path, "profile.userId") || (is(path, "account.id") && !ctx.uid)) ctx.uid = to_u64(value);
        else if (is(path, "profile.nickname")) copy(ctx.nick, ctx.capacity, value);
    }, &ctx);
    const Result r = get_json("https://music.163.com/api/nuser/account/get", cookie, scanner, cancelled);
    if (r != Result::Ok) return r;
    *uid = ctx.uid;
    if (ctx.code == 200 && ctx.uid) {
#ifdef ESP_PLATFORM
        s_session_cache.save_account(cookie, ctx.uid, nickname);
#endif
        return Result::Ok;
    }
#ifdef ESP_PLATFORM
    s_session_cache.magic = 0;
#endif
    ESP_LOGW(kTag, "Account refused: code=%d", ctx.code);
    return Result::Api;
}

Result user_playlists(const char *cookie, uint64_t uid, Playlist *out, size_t max, size_t *count,
                      Playlist *favorites, Cancelled cancelled) {
    if (cancelled && cancelled()) return Result::Cancelled;
#ifdef ESP_PLATFORM
    if (s_session_cache.valid(cookie) && s_session_cache.uid == uid &&
        s_session_cache.has_playlists && max <= SessionCache::kMaxPlaylists) {
        *favorites = s_session_cache.favorites;
        const size_t n = std::min<size_t>(max, s_session_cache.playlist_count);
        for (size_t i = 0; i < n; ++i) out[i] = s_session_cache.playlists[i];
        *count = n;
        return Result::Ok;
    }
#endif
    *favorites = {};
    PlaylistCollector ctx{uid, out, max, favorites};
    JsonScanner scanner(PlaylistCollector::sink, &ctx);
    char url[128];
    // ~50 KB replies arrive in 16 KB TLS records, beyond this device's 8 KB TLS
    // receive limit, so the two large list calls use plain HTTP (user's choice).
    std::snprintf(url, sizeof(url), "http://music.163.com/api/user/playlist?uid=%" PRIu64 "&limit=%u&offset=0", uid,
                  static_cast<unsigned>(max + 1));  // favorites has its own slot
    const Result r = get_json(url, cookie, scanner, cancelled);
    *count = ctx.count;
    if (r != Result::Ok) return r;
    if (ctx.code == 200) {
#ifdef ESP_PLATFORM
        s_session_cache.save_playlists(cookie, uid, out, ctx.count, *favorites);
#endif
        return Result::Ok;
    }
    return Result::Api;
}

Result daily_songs(const char *cookie, uint64_t *ids, size_t max, size_t *count, Cancelled cancelled) {
    *count = 0;
    if (!cookie || !cookie[0]) return Result::Api;
    DailyCollector ctx{ids, max};
    JsonScanner scanner(DailyCollector::sink, &ctx);
    const Result r = eapi_post("/api/v3/discovery/recommend/songs", "\"afresh\":false", scanner,
                               cancelled, nullptr, 0, cookie, true);
    if (r != Result::Ok) return r;
    if (ctx.code != 200) {
        ESP_LOGW(kTag, "Daily recommendations refused: code=%d", ctx.code);
        return Result::Api;
    }
    *count = ctx.count;
    return Result::Ok;
}

Result playlist_tracks(const char *cookie, uint64_t playlist, uint64_t *ids, size_t max, size_t *count,
                       Cancelled cancelled) {
    struct Ctx { int code = 0; uint64_t *ids; size_t max, n = 0; } ctx{0, ids, max};
    JsonScanner scanner([](void *c, JsonScanner::Event e, const char *path, const char *value, size_t) {
        auto &ctx = *static_cast<Ctx *>(c);
        if (e != JsonScanner::Event::Value) return;
        if (is(path, "code")) ctx.code = std::atoi(value);
        else if (is(path, "playlist.trackIds[].id") && ctx.n < ctx.max) ctx.ids[ctx.n++] = to_u64(value);
    }, &ctx);
    char url[128];
    // Plain HTTP for the same reason as user_playlists (large reply).
    std::snprintf(url, sizeof(url), "http://music.163.com/api/v6/playlist/detail?id=%" PRIu64 "&n=0", playlist);
    const Result r = get_json(url, cookie, scanner, cancelled);
    *count = ctx.n;
    if (r != Result::Ok) return r;
    return ctx.code == 200 ? Result::Ok : Result::Api;
}

Result search_songs(const char *query, Song *songs, uint64_t *ids, size_t max, size_t *count, Cancelled cancelled) {
    *count = 0;
    // One buffer on the music task stack: "s=" + up to 63 encoded bytes + options.
    char form[2 + 3 * 63 + 48];
    std::memcpy(form, "s=", 2);
    if (!query || !query[0] || !url_encode(query, form + 2, sizeof(form) - 48)) return Result::Api;
    const size_t used = std::strlen(form);
    std::snprintf(form + used, sizeof(form) - used, "&type=1&limit=%u&offset=0", static_cast<unsigned>(max));
    SearchCollector ctx{songs, ids, max};
    JsonScanner scanner(SearchCollector::sink, &ctx);
    // ~1.5 KB per song: plain HTTP keeps the reply clear of the 8 KB TLS record limit.
    const Result r = request_json("http://music.163.com/api/cloudsearch/pc", nullptr, form, kUserAgent, scanner,
                                  cancelled, nullptr, 0);
    *count = ctx.count;
    if (r != Result::Ok) return r;
    if (ctx.code != 200) {
        ESP_LOGW(kTag, "Search refused: code=%d", ctx.code);
        return Result::Api;
    }
    return ctx.count ? Result::Ok : Result::Api;
}

Result song_details(const uint64_t *ids, size_t n, Song *out, Cancelled cancelled) {
    if (!n) return Result::Ok;
    char url[96 + 10 * 48];
    int len = std::snprintf(url, sizeof(url), "http://music.163.com/api/v3/song/detail?c=%%5B");
    for (size_t i = 0; i < n && i < 10; ++i) {
        out[i] = {};
        out[i].id = ids[i];
        len += std::snprintf(url + len, sizeof(url) - len, "%s%%7B%%22id%%22%%3A%" PRIu64 "%%7D", i ? "%2C" : "", ids[i]);
    }
    std::snprintf(url + len, sizeof(url) - len, "%%5D");
    struct Ctx { int code = 0; Song *out; size_t n; Song cur{}; } ctx{0, out, n < 10 ? n : 10};
    JsonScanner scanner([](void *c, JsonScanner::Event e, const char *path, const char *value, size_t) {
        auto &ctx = *static_cast<Ctx *>(c);
        if (e == JsonScanner::Event::ObjectEnd) {
            if (is(path, "songs[]")) {
                for (size_t i = 0; i < ctx.n; ++i) {
                    if (ctx.out[i].id == ctx.cur.id) { ctx.out[i] = ctx.cur; break; }
                }
                ctx.cur = {};
            }
            return;
        }
        if (is(path, "code") && e == JsonScanner::Event::Value) ctx.code = std::atoi(value);
        else if (is(path, "songs[].id")) ctx.cur.id = to_u64(value);
        else if (is(path, "songs[].dt")) ctx.cur.duration_ms = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
        else if (is(path, "songs[].name") && !ctx.cur.name[0]) copy(ctx.cur.name, sizeof(ctx.cur.name), value);
        else if (is(path, "songs[].ar[].name") && !ctx.cur.artist[0]) copy(ctx.cur.artist, sizeof(ctx.cur.artist), value);
    }, &ctx);
    const Result r = get_json(url, nullptr, scanner, cancelled);
    if (r != Result::Ok) return r;
    return ctx.code == 200 ? Result::Ok : Result::Api;
}

Result song_urls(const char *cookie, const uint64_t *ids, size_t count, SongUrl *out, Cancelled cancelled) {
    if (!count || count > UrlCache::kCount) return Result::Api;
    char id_list[72]{};
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        out[i] = {};
        out[i].id = ids[i];
        used += std::snprintf(id_list + used, sizeof(id_list) - used, "%s%" PRIu64, i ? "," : "", ids[i]);
    }
    UrlCollector ctx{out, count};
    JsonScanner scanner(UrlCollector::sink, &ctx);
    Result r;
    if (cookie && cookie[0]) {
        char fields[160];
        std::snprintf(fields, sizeof(fields),
                      "\"ids\":\"[%s]\",\"level\":\"standard\",\"encodeType\":\"mp3\"", id_list);
        r = eapi_post("/api/song/enhance/player/url/v1", fields, scanner, cancelled, nullptr, 0, cookie);
    } else {
        char request[208];
        std::snprintf(request, sizeof(request),
                      "https://music.163.com/api/song/enhance/player/url?ids=%%5B%s%%5D&br=128000", id_list);
        r = get_json(request, nullptr, scanner, cancelled);
    }
    if (r != Result::Ok || ctx.code != 200) {
        for (size_t i = 0; i < count; ++i) out[i].url[0] = 0;
        return r != Result::Ok ? r : Result::Api;
    }
    return Result::Ok;
}

Result song_url(const char *cookie, uint64_t id, char *url, size_t capacity, bool *trial, Cancelled cancelled) {
    SongUrl item;
    const Result r = song_urls(cookie, &id, 1, &item, cancelled);
    url[0] = 0;
    *trial = item.trial;
    if (r != Result::Ok) return r;
    if (!item.url[0] || std::strlen(item.url) >= capacity) return Result::Api;
    std::strcpy(url, item.url);
    return Result::Ok;
}

Result lyric(uint64_t id, Lyrics &lyrics, Cancelled cancelled) {
    struct Ctx { int code = 0; Lyrics *lyrics; } ctx{0, &lyrics};
    lyrics.clear();
    JsonScanner scanner([](void *c, JsonScanner::Event e, const char *path, const char *value, size_t length) {
        auto &ctx = *static_cast<Ctx *>(c);
        if (e == JsonScanner::Event::ObjectEnd) return;
        if (is(path, "code") && e == JsonScanner::Event::Value) ctx.code = std::atoi(value);
        else if (is(path, "lrc.lyric")) ctx.lyrics->feed(value, length);
    }, &ctx);
    char url[96];
    std::snprintf(url, sizeof(url), "http://music.163.com/api/song/lyric?id=%" PRIu64 "&lv=1", id);
    const Result r = get_json(url, nullptr, scanner, cancelled);
    lyrics.finish();
    if (r != Result::Ok) { lyrics.clear(); return r; }
    return ctx.code == 200 ? Result::Ok : Result::Api;
}

}  // namespace netease
