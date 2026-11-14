// A voice request for a media app, handed over across the Xiaozhi exit.
//
// Xiaozhi and the media apps share the audio workspace, so the request outlives
// the Xiaozhi session and may cross the clean-launch restart that app_registry
// uses to defragment the heap. Freshness is pure so host tests can cover it.
#pragma once

#include "apps/app_base.h"

#include <cstddef>
#include <cstdint>

namespace voice_intent {

enum class Source : uint8_t { None, Favorites, Daily, Playlist, Search, Radio };

constexpr size_t kNameBytes = 64;
constexpr size_t kDetailBytes = 32;
constexpr uint16_t kMaxStopMinutes = 180;
// Long enough for exit, a possible restart, Wi-Fi and login; short enough that
// a dropped request never starts playback on a later manual visit.
constexpr int64_t kLifetimeUs = 45LL * 1000 * 1000;

struct Request {
    passport_app_id_t app = APP_ID_COUNT;
    Source source = Source::None;
    bool shuffle = false;
    uint16_t stop_minutes = 0;  // pause playback after this long; 0 = never
    uint64_t song_id = 0;       // Search: start from this result
    char name[kNameBytes] = {};      // playlist name, search keywords or radio city
    char detail[kDetailBytes] = {};  // Radio: station name keywords
};

// Input task: armed when the voice request is accepted, cleared when dropped.
void arm(const Request &request);
void clear();
// App start: returns the request at most once, only for that app and while fresh.
Request take(passport_app_id_t app);

// esp_timer restarts at boot, so an older arm time means a restart happened in
// between; only a software restart moments ago is the clean launch itself.
inline bool fresh(int64_t armed_us, int64_t now_us, bool software_restart) {
    if (now_us >= armed_us) return now_us - armed_us <= kLifetimeUs;
    return software_restart && now_us <= kLifetimeUs;
}

}  // namespace voice_intent
