// Bounded collectors shared by the device and host regression tests.
#pragma once

#include "apps/netease/netease_api.h"
#include "apps/netease/netease_json.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace netease {

constexpr size_t kHomeEntries = 6;

// out starts after the fixed home entries. Favorites are selected by owner
// and specialType, never by localized name or response order.
struct PlaylistCollector {
    uint64_t uid;
    Playlist *out;
    size_t capacity;
    Playlist *favorites;
    int code = 0;
    size_t count = 0;
    Playlist current{};
    uint64_t owner = 0;
    int special = 0;

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        auto &c = *static_cast<PlaylistCollector *>(opaque);
        if (event == JsonScanner::Event::ObjectEnd) {
            if (std::strcmp(path, "playlist[]")) return;
            if (c.current.id) {
                if (c.owner == c.uid && c.special == 5) {
                    *c.favorites = c.current;
                    c.favorites->kind = PlaylistKind::Favorites;
                } else if (c.count < c.capacity) c.out[c.count++] = c.current;
            }
            c.current = {};
            c.owner = 0;
            c.special = 0;
            return;
        }
        if (!std::strcmp(path, "playlist[].name") && !c.current.name[0])
            std::snprintf(c.current.name, sizeof(c.current.name), "%s", value);
        if (event != JsonScanner::Event::Value) return;
        if (!std::strcmp(path, "code")) c.code = std::atoi(value);
        else if (!std::strcmp(path, "playlist[].id")) c.current.id = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(path, "playlist[].userId")) c.owner = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(path, "playlist[].specialType")) c.special = std::atoi(value);
        else if (!std::strcmp(path, "playlist[].trackCount")) c.current.count = std::strtoul(value, nullptr, 10);
    }
};

struct DailyCollector {
    uint64_t *ids;
    size_t capacity;
    int code = 0;
    size_t count = 0;

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        if (event != JsonScanner::Event::Value) return;
        auto &c = *static_cast<DailyCollector *>(opaque);
        if (!std::strcmp(path, "code")) c.code = std::atoi(value);
        else if (!std::strcmp(path, "data.dailySongs[].id")) {
            const uint64_t id = std::strtoull(value, nullptr, 10);
            if (id && c.count < c.capacity) c.ids[c.count++] = id;
        }
    }
};

// Song search: ids and display rows in relevance order, first artist only.
// Nested album/artist ids live on other paths and are ignored.
struct SearchCollector {
    Song *songs;
    uint64_t *ids;
    size_t capacity;
    int code = 0;
    size_t count = 0;
    Song current{};

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        auto &c = *static_cast<SearchCollector *>(opaque);
        if (event == JsonScanner::Event::ObjectEnd) {
            if (std::strcmp(path, "result.songs[]")) return;
            if (c.current.id && c.count < c.capacity) {
                c.songs[c.count] = c.current;
                c.ids[c.count++] = c.current.id;
            }
            c.current = {};
            return;
        }
        if (!std::strcmp(path, "result.songs[].name") && !c.current.name[0])
            std::snprintf(c.current.name, sizeof(c.current.name), "%s", value);
        else if (!std::strcmp(path, "result.songs[].ar[].name") && !c.current.artist[0])
            std::snprintf(c.current.artist, sizeof(c.current.artist), "%s", value);
        if (event != JsonScanner::Event::Value) return;
        if (!std::strcmp(path, "code")) c.code = std::atoi(value);
        else if (!std::strcmp(path, "result.songs[].id")) c.current.id = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(path, "result.songs[].dt"))
            c.current.duration_ms = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
};

// Form/query encoding of UTF-8 text; false when out is too small.
inline bool url_encode(const char *in, char *out, size_t capacity) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in; ++in) {
        const unsigned char c = static_cast<unsigned char>(*in);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (n + (plain ? 1 : 3) >= capacity) return false;
        if (plain) out[n++] = static_cast<char>(c);
        else {
            out[n++] = '%';
            out[n++] = kHex[c >> 4];
            out[n++] = kHex[c & 15];
        }
    }
    if (!capacity) return false;
    out[n] = 0;
    return true;
}

// ASCII case and spaces, and a spoken "歌单" suffix, do not distinguish names.
inline void normalize(const char *in, char *out, size_t capacity) {
    size_t n = 0;
    for (; *in && n + 1 < capacity; ++in) {
        const unsigned char c = static_cast<unsigned char>(*in);
        if (c == ' ' || c == '\t') continue;
        out[n++] = c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    }
    out[n] = 0;
    static constexpr char kSuffix[] = "歌单";
    constexpr size_t kSuffixBytes = sizeof(kSuffix) - 1;
    if (n > kSuffixBytes && !std::strcmp(out + n - kSuffixBytes, kSuffix)) out[n - kSuffixBytes] = 0;
}

// Index of the best playlist match: exact name, then a name containing the
// request, then a request containing a name of at least two Chinese characters.
inline int find_playlist(const Playlist *lists, size_t count, const char *name) {
    char want[64];
    normalize(name, want, sizeof(want));
    if (!want[0]) return -1;
    int contains = -1, contained = -1;
    for (size_t i = 0; i < count; ++i) {
        char have[sizeof(Playlist::name)];
        normalize(lists[i].name, have, sizeof(have));
        if (!have[0]) continue;
        if (!std::strcmp(have, want)) return static_cast<int>(i);
        if (contains < 0 && std::strstr(have, want)) contains = static_cast<int>(i);
        if (contained < 0 && std::strlen(have) >= 6 && std::strstr(want, have)) contained = static_cast<int>(i);
    }
    return contains >= 0 ? contains : contained;
}

// Caller reserves kHomeEntries slots before any user playlists.
inline void fill_home_entries(Playlist *out, const Playlist &favorites) {
    out[0] = favorites;
    out[0].kind = PlaylistKind::Favorites;
    std::snprintf(out[0].name, sizeof(out[0].name), "我喜欢的音乐");
    out[1] = {};
    out[1].kind = PlaylistKind::Daily;
    std::snprintf(out[1].name, sizeof(out[1].name), "每日推荐");
    struct Chart { uint64_t id; const char *name; };
    static constexpr Chart charts[] = {
        {3778678, "热歌榜"}, {3779629, "新歌榜"}, {19723756, "飙升榜"}, {2884035, "原创榜"},
    };
    static_assert(sizeof(charts) / sizeof(charts[0]) + 2 == kHomeEntries);
    for (size_t i = 0; i < kHomeEntries - 2; ++i) {
        out[i + 2] = {};
        out[i + 2].id = charts[i].id;
        std::snprintf(out[i + 2].name, sizeof(out[i + 2].name), "%s", charts[i].name);
    }
}

}  // namespace netease
