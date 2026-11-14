// NetEase Cloud Music web API and PC-client eapi requests.
//
// Requests that carry the login cookie use HTTPS, except large list calls
// (user playlists, playlist track ids, daily songs), which use HTTP; anonymous metadata (song
// detail, lyrics) uses plain HTTP so it can run beside the MP3 decoder without
// a TLS session. Responses stream through JsonScanner; nothing is buffered.
#pragma once

#include <cstddef>
#include <cstdint>

namespace netease {

class Lyrics;

enum class Result : uint8_t { Ok, Cancelled, Network, Http, Api, Parse, NoMemory };
const char *result_text(Result result);
// The browser identity used for API calls; the audio CDN expects the same.
const char *browser_agent();

using Cancelled = bool (*)();

enum class PlaylistKind : uint8_t { Playlist, Favorites, Daily };

struct Playlist {
    uint64_t id;
    uint32_t count;
    char name[52];
    PlaylistKind kind = PlaylistKind::Playlist;
};

struct Song {
    uint64_t id;
    uint32_t duration_ms;
    char name[64];
    char artist[44];
};

// MUSIC_U from the desktop login is ~1 KB ("MUSIC_U=<value>").
constexpr size_t kCookieBytes = 1280;

// QR login uses the PC client's encrypted "eapi" requests: the plain web
// endpoints are refused with 8821 after the scan. device_id is a stable
// random id for this device (kept in NVS by the app).
void set_device_id(const char *device_id);
// The phone opens https://music.163.com/login?codekey=<key>&chainId=...
Result qr_key(char *key, size_t capacity, Cancelled cancelled);
// code: 800 expired, 801 waiting, 802 scanned (confirm on phone), 803 done,
// 8821 refused by risk control (returned with Result::Api).
// On 803 cookie receives "MUSIC_U=..." from Set-Cookie.
Result qr_poll(const char *key, int *code, char *cookie, size_t capacity, Cancelled cancelled);
// Api when the cookie is no longer accepted.
Result account(const char *cookie, uint64_t *uid, char *nickname, size_t capacity, Cancelled cancelled);

Result user_playlists(const char *cookie, uint64_t uid, Playlist *out, size_t max, size_t *count,
                      Playlist *favorites, Cancelled cancelled);
// Personalized daily recommendations; requires login. Streams only song IDs.
Result daily_songs(const char *cookie, uint64_t *ids, size_t max, size_t *count, Cancelled cancelled);
// Ids of up to max tracks; count receives the number actually stored.
Result playlist_tracks(const char *cookie, uint64_t playlist, uint64_t *ids, size_t max, size_t *count,
                       Cancelled cancelled);
// Keyword song search in relevance order (song and/or artist names). Anonymous
// plain HTTP: no login cookie travels with it, and the account's own rights are
// applied later when the play URL is requested. Api when nothing matched.
Result search_songs(const char *query, Song *songs, uint64_t *ids, size_t max, size_t *count, Cancelled cancelled);
// Fills out[i] for ids[i]; songs missing from the reply keep their id and an empty name.
Result song_details(const uint64_t *ids, size_t n, Song *out, Cancelled cancelled);
// Api with url[0] == 0 when the account may not play the song (VIP / no rights).
struct SongUrl;
Result song_urls(const char *cookie, const uint64_t *ids, size_t count, SongUrl *out, Cancelled cancelled);
Result song_url(const char *cookie, uint64_t id, char *url, size_t capacity, bool *trial, Cancelled cancelled);
Result lyric(uint64_t id, Lyrics &lyrics, Cancelled cancelled);

}  // namespace netease
