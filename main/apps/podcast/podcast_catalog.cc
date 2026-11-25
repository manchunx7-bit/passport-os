#include "podcast_catalog.h"
#include "apps/radio/radio_json_stream.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <strings.h>
#include <cstring>
#include <memory>
#include <new>

namespace podcast {
namespace {
constexpr std::size_t kObjectBytes = 8192;
constexpr std::size_t kMaxBody = 96 * 1024;

uint32_t number(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > UINT32_MAX) return 0;
    return static_cast<uint32_t>(item->valuedouble);
}

template <std::size_t N>
void copy_title(char (&out)[N], const char *title) {
    auto length = std::min(std::strlen(title), N - 1);
    while (length && (static_cast<unsigned char>(title[length]) & 0xc0) == 0x80) --length;
    std::memcpy(out, title, length);
    out[length] = '\0';
}

void append_episode(const char *object, Page &page) {
    if (page.count >= kPageSize) return;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> item(cJSON_Parse(object), cJSON_Delete);
    if (!item) return;
    const uint32_t id = number(item.get(), "trackId");
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item.get(), "title"));
    if (!id || !title || !*title) return;
    auto &entry = page.episodes[page.count++];
    entry.id = id;
    entry.duration = number(item.get(), "duration");
    entry.paid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item.get(), "isPaid"));
    copy_title(entry.title, title);
    if (!entry.paid) {
        const char *primary = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item.get(), "playUrl64"));
        const char *backup = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item.get(), "playUrl32"));
        if (audio_url(primary, entry.url, sizeof(entry.url))) {
            if (audio_url(backup, entry.backup_url, sizeof(entry.backup_url)) &&
                std::strcmp(entry.url, entry.backup_url) == 0) entry.backup_url[0] = '\0';
        } else audio_url(backup, entry.url, sizeof(entry.url));
    }
}

void append_program(const char *object, ProgramList &list) {
    if (list.count >= kDiscoveryLimit) return;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> item(cJSON_Parse(object), cJSON_Delete);
    if (!item || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item.get(), "is_paid")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item.get(), "isDraft")) ||
        !number(item.get(), "tracks")) return;
    const uint32_t id = number(item.get(), "id");
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item.get(), "title"));
    if (!id || !title || !*title) return;
    for (unsigned i = 0; i < list.count; ++i) if (list.items[i].id == id) return;
    auto &entry = list.items[list.count++];
    entry.id = id;
    entry.plays = number(item.get(), "play");
    entry.tracks = number(item.get(), "tracks");
    copy_title(entry.title, title);
}

// The legacy response surrounds the track array with large album fields.
// Keep only one track object in RAM and scan the outer pagination count.
struct CatalogReader {
    explicit CatalogReader(Page &target) : page(&target), marker("\"tracks\":{"), key("\"list\"") {}
    explicit CatalogReader(ProgramList &target) : programs(&target), marker("\"response\":{"), key("\"docs\"") {}
    char object[kObjectBytes];
    Page *page = nullptr;
    ProgramList *programs = nullptr;
    RadioJsonStream tracks{object, sizeof(object)};
    const char *marker;
    const char *key;
    unsigned matched = 0;
    bool in_array = false;
    bool found_array = false;
    bool reading_total = false;
    unsigned total = 0;
    void feed(const char *bytes, std::size_t size) {
        if ((page && page->total) || (programs && tracks.Complete())) return;
        for (std::size_t i = 0; i < size; ++i) {
            const char c = bytes[i];
            if (in_array) {
                tracks.Feed(&c, 1, [&](const char *item) {
                    if (page) append_episode(item, *page);
                    else append_program(item, *programs);
                });
                if (tracks.Complete()) {
                    if (programs) return;
                    in_array = false;
                    marker = "\"totalCount\"";
                }
            } else if (reading_total) {
                if (c >= '0' && c <= '9' && total < 100000) total = total * 10 + unsigned(c - '0');
                else if (total) { page->total = total; return; }
            } else if (found_array && !marker) {
                if (c == '[') {
                    in_array = true;
                    tracks.Feed(&c, 1, [](const char *) {});
                }
            } else {
                matched = c == marker[matched] ? matched + 1 : unsigned(c == marker[0]);
                if (marker[matched]) continue;
                matched = 0;
                if (found_array) reading_total = true;
                else if (marker != key) marker = key;
                else { found_array = true; marker = nullptr; }
            }
        }
    }
};
} // namespace

// Public Ximalaya endpoints emit 8.2-16.4 KiB TLS records, exceeding
// CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192, and HTTP avoids TLS RAM pressure alongside MP3 decode.
bool audio_url(const char *input, char *output, std::size_t capacity) {
    if (!input || !output || !capacity) return false;
    const char *host = std::strncmp(input, "https://", 8) == 0 ? input + 8 :
                       std::strncmp(input, "http://", 7) == 0 ? input + 7 : nullptr;
    const char *path = host ? std::strchr(host, '/') : nullptr;
    constexpr char suffix[] = ".xmcdn.com";
    if (!path || path - host <= static_cast<ptrdiff_t>(sizeof(suffix) - 1) ||
        std::memcmp(path - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) ||
        std::strpbrk(host, "\r\n\t @\\") || std::strlen(host) + 8 > capacity) return false;
    std::snprintf(output, capacity, "http://%s", host);
    return true;
}

namespace {
bool fetch_json(const char *url, const std::atomic<bool> &cancel, CatalogReader *reader) {
    if (!reader || cancel.load()) return false;
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.timeout_ms = 5000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 512;
    if (std::strncmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    bool success = false;
    esp_err_t open_err = esp_http_client_open(client, 0);
    int hdr_len = -1;
    int status = 0;
    std::size_t read = 0;
    int last_n = 0;
    if (open_err == ESP_OK && !cancel.load()) {
        hdr_len = esp_http_client_fetch_headers(client);
        status = hdr_len >= 0 ? esp_http_client_get_status_code(client) : 0;
        if (hdr_len >= 0 && status == 200) {
            const auto deadline = esp_timer_get_time() + 12000000;
            char chunk[768];
            while (read < kMaxBody && !cancel.load() && esp_timer_get_time() < deadline) {
                const int n = esp_http_client_read(client, chunk, sizeof(chunk));
                last_n = n;
                if (n < 0) break;
                if (n == 0) {
                    success = esp_http_client_is_complete_data_received(client) && reader->tracks.Complete();
                    break;
                }
                reader->feed(chunk, static_cast<std::size_t>(n));
                read += static_cast<std::size_t>(n);
            }
        }
    }
    const bool complete = reader->tracks.Complete();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!success && !cancel.load()) {
        ESP_LOGW("podcast", "Fetch failed: open=%d hdr=%d status=%d read=%u err=%d complete=%d",
                 static_cast<int>(open_err), hdr_len, status,
                 static_cast<unsigned>(read), last_n, static_cast<int>(complete));
    }
    return success && !cancel.load();
}
} // namespace

bool fetch_page(uint32_t album, unsigned page, const std::atomic<bool> &cancel, Page &result) {
    result = {};
    char url[144];
    std::snprintf(url, sizeof(url), "http://mobile.ximalaya.com/mobile/others/ca/album/track/%lu/true/%u/%u",
                  static_cast<unsigned long>(album), page, kPageSize);
    auto reader = std::unique_ptr<CatalogReader>(new (std::nothrow) CatalogReader(result));
    return fetch_json(url, cancel, reader.get());
}

namespace {
unsigned theme_index() {
    const std::time_t now = std::time(nullptr);
    return now > 0 ? static_cast<unsigned>((static_cast<uint64_t>(now) / 86400) % 4) : 0;
}
} // namespace

const char *recommended_theme_name() {
    static constexpr const char *kThemes[]{"访谈", "科技", "故事", "商业"};
    return kThemes[theme_index()];
}

namespace {
void sort_by_plays(ProgramList &list) {
    std::sort(list.items, list.items + list.count, [](const Listing &a, const Listing &b) {
        return a.plays != b.plays ? a.plays > b.plays : a.id < b.id;
    });
}

bool search_albums(const char *query, const std::atomic<bool> &cancel, ProgramList &result) {
    result = {};
    char encoded[190];  // up to 63 bytes when every byte needs escaping
    const auto size = query ? std::strlen(query) : 0;
    if (!size || size > (sizeof(encoded) - 1) / 3) return false;
    constexpr char hex[] = "0123456789ABCDEF";
    std::size_t used = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const auto c = static_cast<unsigned char>(query[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) encoded[used++] = c;
        else {
            encoded[used++] = '%';
            encoded[used++] = hex[c >> 4];
            encoded[used++] = hex[c & 15];
        }
    }
    encoded[used] = '\0';
    char url[288];
    std::snprintf(url, sizeof(url), "http://search.ximalaya.com/front/v1?kw=%s&core=album&page=1&rows=%u",
                  encoded, kDiscoveryLimit);
    auto reader = std::unique_ptr<CatalogReader>(new (std::nothrow) CatalogReader(result));
    return fetch_json(url, cancel, reader.get());
}
} // namespace

bool discover(Discovery source, const char *query, const std::atomic<bool> &cancel, ProgramList &result) {
    if (source == Discovery::Hot) query = "播客";
    else if (source == Discovery::Recommended) {
        static constexpr const char *kThemes[]{"播客 访谈", "播客 科技", "播客 故事", "播客 商业"};
        query = kThemes[theme_index()];
    } else if (source == Discovery::Business) {
        query = "商业 财经";
    } else if (source == Discovery::Story) {
        query = "故事 人文";
    } else if (source == Discovery::Crosstalk) {
        query = "相声 评书";
    } else if (source == Discovery::BookHot) {
        query = "有声书";
    } else if (source == Discovery::BookMystery) {
        query = "悬疑 有声书";
    } else if (source == Discovery::BookHistory) {
        query = "历史 评书";
    } else if (source == Discovery::BookClassics) {
        query = "世界名著";
    }
    if (!search_albums(query, cancel, result)) return false;
    if (source == Discovery::Search) return true;
    sort_by_plays(result);
    if (source != Discovery::BookClassics) return true;
    // Half world classics, half science fiction, each in popularity order.
    std::unique_ptr<ProgramList> fiction(new (std::nothrow) ProgramList{});
    if (!fiction || !search_albums("科幻 有声书", cancel, *fiction)) return !cancel.load() && result.count;
    sort_by_plays(*fiction);
    result.count = std::min(result.count, kDiscoveryLimit / 2);
    for (unsigned i = 0; i < fiction->count && result.count < kDiscoveryLimit; ++i) {
        const auto &item = fiction->items[i];
        bool duplicate = false;
        for (unsigned j = 0; j < result.count; ++j) duplicate |= result.items[j].id == item.id;
        if (!duplicate) result.items[result.count++] = item;
    }
    return true;
}

unsigned match_book(const ProgramList &list, const char *query) {
    if (!query || !*query) return list.count;
    unsigned best = list.count;
    for (unsigned i = 0; i < list.count; ++i) {
        const auto &item = list.items[i];
        if (!strcasecmp(item.title, query)) return i;
        if (strcasestr(item.title, query) && (best == list.count || item.plays > list.items[best].plays)) best = i;
    }
    return best;
}

unsigned match_program(const ProgramList &list, const char *query) {
    if (!query || !*query) return list.count;
    unsigned exact = list.count;
    unsigned partial = list.count;
    unsigned partial_count = 0;
    for (unsigned i = 0; i < list.count; ++i) {
        const auto &item = list.items[i];
        if (!strcasecmp(item.title, query)) {
            if (exact == list.count || item.plays > list.items[exact].plays) exact = i;
        } else if (strcasestr(item.title, query)) {
            partial = i;
            ++partial_count;
        }
    }
    if (exact != list.count) return exact;
    return partial_count == 1 ? partial : list.count;
}
} // namespace podcast
