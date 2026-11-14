// A one-shot copy of the results already offered by Xiaozhi. Kept in RTC
// memory by the owner so the normal clean-launch restart can carry it too.
#pragma once

#include "apps/netease/netease_api.h"
#include "voice_intent.h"
#include <cstdio>
#include <cstring>

namespace netease {

struct VoiceSearchCache {
    static constexpr size_t kCount = 5;
    static constexpr uint32_t kMagic = 0x4D534331; // MSC1
    uint32_t magic, check;
    int64_t saved_us;
    size_t count;
    char query[64];
    Song songs[kCount];

    uint32_t checksum() const {
        uint32_t hash = 2166136261u ^ kMagic;
        const auto mix = [&](const void *data, size_t size) {
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 16777619u;
        };
        mix(&saved_us, sizeof(saved_us));
        mix(&count, sizeof(count));
        mix(query, sizeof(query));
        mix(songs, sizeof(songs));
        return hash;
    }

    void save(const char *text, const Song *rows, size_t n, int64_t now_us) {
        *this = {};
        if (!text || !text[0] || std::strlen(text) >= sizeof(query) || !rows || !n) return;
        std::snprintf(query, sizeof(query), "%s", text);
        count = n < kCount ? n : kCount;
        for (size_t i = 0; i < count; ++i) songs[i] = rows[i];
        saved_us = now_us;
        check = checksum();
        magic = kMagic;
    }

    bool take(const char *text, Song *rows, uint64_t *ids, size_t capacity, size_t *n,
              int64_t now_us, bool software_restart) {
        *n = 0;
        const bool hit = magic == kMagic && count > 0 && count <= kCount && query[sizeof(query) - 1] == 0 &&
            check == checksum() && text && !std::strcmp(text, query) &&
            voice_intent::fresh(saved_us, now_us, software_restart);
        magic = 0; // mismatches, stale/corrupt data and a hit are all consumed
        if (!hit || !rows || !ids || !capacity) return false;
        *n = count < capacity ? count : capacity;
        for (size_t i = 0; i < *n; ++i) { rows[i] = songs[i]; ids[i] = songs[i].id; }
        return true;
    }
};

} // namespace netease
