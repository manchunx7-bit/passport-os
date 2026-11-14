// One-track MP3 playback session for the NetEase app.
//
// Runs on the caller's task (the music task, whose stack lives in the shared
// audio workspace). A session owns the HTTP reader task, the compressed FIFO
// and the decoder; all are released before play() returns, so the caller may
// open a TLS request afterwards without the decoder's ~23 KiB still resident.
#pragma once

#include "apps/netease/netease_meter.h"
#include <cstddef>
#include <cstdint>

namespace netease {

enum class PlayEnd : uint8_t {
    Finished,     // whole file decoded
    Interrupted,  // interrupted() became true (key command, app exit)
    NetworkLost,  // connection dropped; resume from position with Range
    Expired,      // CDN refused the URL (403/404): fetch a fresh one
    Failed,       // unsupported data or decoder error
    NoMemory,
};

struct PlayRequest {
    const char *url;
    uint32_t offset;       // byte offset to resume from
    uint32_t total_bytes;  // full file size if known (0 = learn it)
    uint32_t duration_ms;  // from song detail; drives elapsed time
    bool (*interrupted)();
};

struct PlayOutcome {
    PlayEnd end;
    uint32_t position;     // byte offset reached
    uint32_t total_bytes;
    bool played;           // at least one frame reached the speaker
};

// Session workspace: static task control blocks for the music task and the
// HTTP reader, placed inside the app's model block (no extra .bss).
struct PlayerTasks {
    alignas(8) uint8_t music_tcb[400];
    alignas(8) uint8_t reader_tcb[400];
};

bool player_init();
void player_deinit();
PlayOutcome play(const PlayRequest &request, PlayerTasks &tasks);

// Live status for the UI (relaxed atomics).
void player_reset_status();
uint32_t player_elapsed_ms();
bool player_buffering();
void player_audio_levels(uint8_t (&levels)[kMeterBars]);

}  // namespace netease
