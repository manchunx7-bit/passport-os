#pragma once

#include <cstddef>
#include <cstdint>

struct RadioStation {
    char name[64];
    char description[96];
    char url[256];
    // Real FM frequency in units of 0.1 MHz (for example 947 means 94.7 MHz).
    // Zero means the directory entry carried no usable frequency, and the dial
    // falls back to spreading the station across the scale by its position.
    uint16_t frequency_decihz;
};

enum class RadioPlaybackState {
    Stopped,
    Connecting,
    Buffering,
    Playing,
    Reconnecting,
    Error,
    Finished,
};

// A finite stream shares the radio decoder/workspace but has no station UI or auto-skip.
inline constexpr std::size_t kEpisodeMeterBars = 18;
struct EpisodePlayback {
    RadioPlaybackState state;
    uint32_t offset;
    uint32_t total;
};
bool radio_player_init(bool finite_stream = false);
bool radio_player_play_episode(const char *url, uint32_t offset);
EpisodePlayback radio_player_episode();
void radio_player_episode_levels(uint8_t out[kEpisodeMeterBars]);
// Stops the streaming task and releases decoder, codec, I2S/DMA and sync
// resources. Returns false when the worker did not exit within the bounded
// wait; in that case resources are deliberately kept to avoid use-after-free.
bool radio_player_deinit(uint32_t timeout_ms = 5000);
void radio_player_set_network(bool connected);
void radio_player_toggle();
void radio_player_set_playing(bool playing);
bool radio_player_is_playing();
bool radio_player_wait_idle(uint32_t timeout_ms);
void radio_player_select_relative(int delta);
void radio_player_set_station(std::size_t index);
std::size_t radio_player_station_index();
std::size_t radio_player_station_count();
RadioStation radio_player_station(std::size_t index);
bool radio_player_replace_stations(const RadioStation *stations, std::size_t count);
// Installs a catalog and keeps listening to the station the user last chose
// when the new catalog still offers it, so a restart resumes where it left off.
bool radio_player_replace_stations_resuming(const RadioStation *stations,
                                           std::size_t count);
// Same persisted system volume used by Settings and Xiaozhi, including 0=mute.
void radio_player_set_volume(uint8_t volume);
uint8_t radio_player_volume();
