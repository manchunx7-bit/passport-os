#pragma once

#include "apps/netease/netease_json.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace netease {

struct SongUrl {
    uint64_t id = 0;
    char url[512]{};
    bool trial = false;
};

// A response may reorder songs and split signed URLs across scanner chunks.
// Publish only complete, playable records, matched by ID, never by position.
struct UrlCollector {
    SongUrl *out;
    size_t count;
    int code = 0, item_code = 0;
    SongUrl current{};
    size_t used = 0;
    bool overflow = false;
    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t length) {
        auto &c = *static_cast<UrlCollector *>(opaque);
        if (event == JsonScanner::Event::ObjectEnd) {
            if (std::strcmp(path, "data[]")) return;
            if (c.item_code == 200 && !c.overflow && c.current.url[0])
                for (size_t i = 0; i < c.count; ++i)
                    if (c.out[i].id == c.current.id) c.out[i] = c.current;
            c.current = {};
            c.used = 0;
            c.overflow = false;
            c.item_code = 0;
            return;
        }
        if (!std::strcmp(path, "data[].url")) {
            if (!c.used && event == JsonScanner::Event::Value && !std::strcmp(value, "null")) return;
            if (c.used + length >= sizeof(c.current.url)) { c.overflow = true; return; }
            std::memcpy(c.current.url + c.used, value, length);
            c.used += length;
            c.current.url[c.used] = 0;
        } else if (event == JsonScanner::Event::Value) {
            if (!std::strcmp(path, "code")) c.code = std::atoi(value);
            else if (!std::strcmp(path, "data[].id")) c.current.id = std::strtoull(value, nullptr, 10);
            else if (!std::strcmp(path, "data[].code")) c.item_code = std::atoi(value);
            else if (!std::strncmp(path, "data[].freeTrialInfo.", 21)) c.current.trial = true;
        }
    }
};

struct UrlCache {
    static constexpr size_t kCount = 3;
    static constexpr int64_t kLifetimeUs = 5LL * 60 * 1000 * 1000;
    SongUrl entries[kCount]{};
    int64_t fetched_at = 0;
    const SongUrl *find(uint64_t id, int64_t now) const {
        if (!fetched_at || now < fetched_at || now - fetched_at >= kLifetimeUs) return nullptr;
        for (const auto &entry : entries) if (entry.id == id && entry.url[0]) return &entry;
        return nullptr;
    }
    void clear() { *this = {}; }
};

}  // namespace netease
