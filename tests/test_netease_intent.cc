// Voice media requests: playlist name matching, handover freshness, search parsing.
#include "apps/netease/netease_catalog.h"
#include "voice_intent.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

using namespace netease;
using voice_intent::fresh;

int main() {
    Playlist lists[kHomeEntries + 4]{};
    fill_home_entries(lists, Playlist{});
    const char *own[] = {"跑步专用", "Coding Music", "周杰伦", "深夜 歌单"};
    for (size_t i = 0; i < 4; ++i) std::snprintf(lists[kHomeEntries + i].name, sizeof(lists[0].name), "%s", own[i]);
    const size_t n = sizeof(lists) / sizeof(lists[0]);
    const int home = static_cast<int>(kHomeEntries);

    // Exact names win, ignoring ASCII case, spaces and a spoken 歌单 suffix.
    assert(find_playlist(lists, n, "热歌榜") == 2);
    assert(find_playlist(lists, n, "coding music") == home + 1);
    assert(find_playlist(lists, n, "深夜") == home + 3);
    assert(find_playlist(lists, n, "跑步专用歌单") == home);
    assert(find_playlist(lists, n, "我喜欢的音乐") == 0);
    // Partial names: a list containing the request, then a request naming a list.
    assert(find_playlist(lists, n, "跑步") == home);
    assert(find_playlist(lists, n, "播放周杰伦的歌") == home + 2);
    // A single Chinese character is too weak to match inside a request.
    Playlist single[1]{};
    std::snprintf(single[0].name, sizeof(single[0].name), "歌");
    assert(find_playlist(single, 1, "我的歌") == -1);
    assert(find_playlist(lists, n, "古典") == -1);
    assert(find_playlist(lists, n, "") == -1 && find_playlist(lists, n, "歌单") == -1);
    assert(find_playlist(lists, 0, "热歌榜") == -1);

    // Same boot: bounded age. After a restart: only a software restart moments ago.
    const int64_t s = 1000 * 1000;
    assert(fresh(10 * s, 10 * s, false) && fresh(10 * s, 55 * s, false) && !fresh(10 * s, 56 * s, false));
    assert(fresh(120 * s, 3 * s, true) && !fresh(120 * s, 3 * s, false) && !fresh(120 * s, 46 * s, true));
    // Search reply (real public response): ids and rows in relevance order,
    // first artist only, nested album/artist ids ignored, capacity respected.
    FILE *f = std::fopen("tests/fixtures/netease/cloudsearch.json", "rb");
    assert(f);
    std::string body;
    char chunk[333];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) body.append(chunk, got);
    std::fclose(f);
    for (size_t capacity : {size_t(10), size_t(3)}) {
        Song songs[10]{};
        uint64_t ids[10]{};
        SearchCollector c{songs, ids, capacity};
        JsonScanner scanner(SearchCollector::sink, &c);
        for (size_t i = 0; i < body.size(); i += 97) assert(scanner.feed(body.data() + i, std::min<size_t>(97, body.size() - i)));
        assert(scanner.complete() && c.code == 200 && c.count == capacity);
        assert(ids[0] == 1901371647 && songs[0].id == ids[0] && !std::strcmp(songs[0].name, "孤勇者"));
        assert(!std::strcmp(songs[0].artist, "陈奕迅") && songs[0].duration_ms == 256000);
        assert(ids[2] == 2737753398 && !std::strcmp(songs[2].artist, "田一名"));
        if (capacity == 10) assert(ids[9] == 64093 && !std::strcmp(songs[9].name, "孤独患者"));
        else assert(!ids[3]);
    }
    char encoded[64];
    assert(url_encode("孤勇者 a-1", encoded, sizeof(encoded)) && !std::strcmp(encoded, "%E5%AD%A4%E5%8B%87%E8%80%85%20a-1"));
    assert(!url_encode("孤勇者", encoded, 9) && url_encode("", encoded, 1) && !encoded[0]);
    std::puts("Voice media: playlist matching, handover freshness, search parsing PASS");
}
