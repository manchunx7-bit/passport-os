// State shared by the NetEase music task, the key handler and the UI timer.
// Lives in the shared audio workspace's model block while the app owns it;
// every field is guarded by model_lock() except the atomics in app_netease.cc.
#pragma once

#include "apps/netease/netease_api.h"
#include "apps/netease/netease_controls.h"
#include "apps/netease/netease_lyrics.h"
#include "apps/netease/netease_player.h"
#include "audio_workspace.h"

#include <cstddef>
#include <cstdint>

namespace netease {

constexpr size_t kMaxTracks = 400;
constexpr size_t kPageSize = 10;
constexpr size_t kMaxLists = 24;
constexpr size_t kVisibleRows = 7;

struct Model {
    PlayerTasks tasks;
    uint64_t ids[kMaxTracks];
    Song page[kPageSize];
    Playlist lists[kMaxLists];
    Song now;

    Page page_id;
    // Message page: text and whether OK retries.
    char message[64];
    bool can_retry;
    // Login page.
    char qr_url[176];
    char login_status[48];
    bool logged_in;
    char nickname[32];

    uint8_t list_count;       // playlists + charts
    uint8_t list_selected;    // includes the trailing login/logout row
    int8_t open_list;         // index into lists of the loaded track ids, -1 none
    uint16_t track_count;
    uint16_t track_selected;  // 0 = "返回歌单", n = ids[n - 1]
    uint16_t page_start;      // index in ids of page[0]
    uint8_t page_count;       // 0 = page not loaded
    bool page_loading;

    int16_t now_index;        // index in ids of the playing track, -1 none
    bool paused;
    bool session;             // decoder session running
    char player_note[40];     // "已跳过无法播放的歌曲" etc.
    bool volume_notice;
    uint32_t volume_until_ms;
    uint32_t revision;        // bumped on every change the UI must redraw
    bool lyrics_ready;        // false while the music task rewrites the lyrics

    alignas(8) uint8_t lyrics_object[sizeof(Lyrics)];

    alignas(4) uint8_t lyrics[1];  // remainder of the block, see lyrics_bytes()
};

constexpr size_t lyrics_bytes() { return audio_workspace::music::kModelBytes - offsetof(Model, lyrics); }
static_assert(lyrics_bytes() >= 2048, "model leaves too little room for lyrics");

Model &model();
bool model_lock(uint32_t timeout_ms = 1000);
void model_unlock();

}  // namespace netease
