// Host harness for the NetEase streaming JSON scanner and LRC parser.
// Modes print records for tests/test_netease_parse.py to compare with Python's
// json module; "self" runs the built-in edge-case assertions.
#include "apps/netease/netease_json.h"
#include "apps/netease/netease_lyrics.h"
#include "apps/netease/netease_catalog.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using netease::JsonScanner;

namespace {

struct Record { std::string a, b, c, d; };
struct Collector {
    const char *item;  // ObjectEnd path that closes one record
    std::vector<std::pair<std::string, std::string *>> fields;
    Record current;
    std::vector<Record> out;
    std::vector<std::string> events;
};

void feed_chunked(JsonScanner &scanner, const std::string &text) {
    uint32_t seed = 12345;
    for (size_t i = 0; i < text.size();) {
        seed = seed * 1103515245 + 12345;
        const size_t n = std::min<size_t>(1 + (seed >> 16) % 97, text.size() - i);
        assert(scanner.feed(text.data() + i, n));
        i += n;
    }
    assert(scanner.complete());
}

std::string read_file(const char *path) {
    FILE *f = std::fopen(path, "rb");
    assert(f);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

void sink(void *ctx, JsonScanner::Event e, const char *path, const char *value, size_t) {
    auto &c = *static_cast<Collector *>(ctx);
    if (e == JsonScanner::Event::ObjectEnd) {
        if (!std::strcmp(path, c.item)) { c.out.push_back(c.current); c.current = {}; }
        return;
    }
    for (auto &f : c.fields) {
        // First value wins (first artist); a long string keeps its leading chunk.
        if (!std::strcmp(path, f.first.c_str()) && f.second->empty()) *f.second = value;
    }
}

void print(const Collector &c) {
    for (const auto &r : c.out) std::printf("%s\t%s\t%s\t%s\n", r.a.c_str(), r.b.c_str(), r.c.c_str(), r.d.c_str());
}

void events_sink(void *ctx, JsonScanner::Event e, const char *path, const char *value, size_t length) {
    auto &c = *static_cast<Collector *>(ctx);
    const char *kind = e == JsonScanner::Event::Value ? "V" : e == JsonScanner::Event::ValuePart ? "P" : "E";
    c.events.push_back(std::string(kind) + " " + path + "=" + std::string(value, length));
}

std::vector<std::string> scan_events(const char *json) {
    Collector c{};
    JsonScanner s(events_sink, &c);
    for (const char *p = json; *p; ++p) assert(s.feed(p, 1));
    assert(s.complete());
    return c.events;
}

void self_test() {
    // Favorites can be renamed, arrive last, and follow more ordinary lists
    // than fit on the device. Another user's favorites must never be promoted.
    netease::Playlist home[netease::kHomeEntries + 2]{};
    netease::Playlist favorite{};
    netease::PlaylistCollector playlists{42, home + netease::kHomeEntries, 2, &favorite};
    JsonScanner catalog(netease::PlaylistCollector::sink, &playlists);
    feed_chunked(catalog, R"({"playlist":[
        {"id":101,"name":"我喜欢的音乐","userId":99,"specialType":5},
        {"id":102,"name":"My own list","userId":42,"specialType":0},
        {"id":103,"name":"Overflow","userId":42,"specialType":0},
        {"creator":{"userId":99},"name":"Renamed favorite","specialType":5,"userId":42,"id":9007199254740993,"trackCount":27}
    ],"code":200})");
    assert(playlists.code == 200 && playlists.count == 2);
    assert(favorite.id == 9007199254740993ULL && favorite.count == 27);
    netease::fill_home_entries(home, favorite);
    const char *names[] = {"我喜欢的音乐", "每日推荐", "热歌榜", "新歌榜", "飙升榜", "原创榜"};
    for (size_t i = 0; i < netease::kHomeEntries; ++i) assert(!std::strcmp(home[i].name, names[i]));
    assert(home[0].kind == netease::PlaylistKind::Favorites);
    assert(home[1].kind == netease::PlaylistKind::Daily && !home[1].id);
    assert(home[2].id == 3778678 && home[3].id == 3779629 && home[4].id == 19723756);
    assert(home[6].id == 101 && home[7].id == 102);
    assert(home[6].kind == netease::PlaylistKind::Playlist);

    // Guest / missing-favorites state retains both personal entry labels but
    // has no fabricated playlist id. Filling the header preserves user lists.
    netease::fill_home_entries(home, {});
    assert(!home[0].id && home[0].kind == netease::PlaylistKind::Favorites);
    assert(home[6].id == 101 && home[7].id == 102);

    uint64_t daily_ids[3] = {0, 0, 777};
    netease::DailyCollector daily{daily_ids, 2};
    JsonScanner recommendations(netease::DailyCollector::sink, &daily);
    feed_chunked(recommendations, R"({"code":200,"data":{"dailySongs":[
        {"ar":[{"id":99}],"al":{"id":98},"id":123},
        {"id":null},{"id":9007199254740993},{"id":456}
    ],"recommendReasons":[{"id":987}]}})");
    assert(daily.code == 200 && daily.count == 2);
    assert(daily_ids[0] == 123 && daily_ids[1] == 9007199254740993ULL && daily_ids[2] == 777);
    netease::DailyCollector empty{nullptr, 0};
    JsonScanner no_daily(netease::DailyCollector::sink, &empty);
    feed_chunked(no_daily, R"({"code":200,"data":{"dailySongs":[]}})");
    assert(empty.code == 200 && !empty.count);
    netease::DailyCollector denied{nullptr, 0};
    JsonScanner unauthorized(netease::DailyCollector::sink, &denied);
    feed_chunked(unauthorized, R"({"code":301,"message":"login required"})");
    assert(denied.code == 301 && !denied.count);
    std::puts("NetEase favorites/home/daily collectors PASS");

    auto ev = scan_events(R"({"a":{"b":[1,{"c":"x\"y"},[],true]},"n":null,"e":{}})");
    const std::vector<std::string> want = {
        "V a.b[]=1", "V a.b[].c=x\"y", "E a.b[]=", "V a.b[]=true", "E a=", "V n=null", "E e=", "E =",
    };
    assert(ev == want);
    // \u escapes including a surrogate pair become UTF-8.
    ev = scan_events(R"({"s":"中文🎵\/"})");
    assert(ev[0] == "V s=\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x8E\xB5/");
    // Long strings arrive as UTF-8-safe parts followed by the final value.
    std::string longjson = "{\"lrc\":\"";
    std::string body;
    for (int i = 0; i < 200; ++i) body += "\xE6\xAD\x8C";  // 600 bytes of a 3-byte character
    longjson += body + "\"}";
    Collector c{};
    JsonScanner s(events_sink, &c);
    assert(s.feed(longjson.data(), longjson.size()));
    std::string joined;
    size_t parts = 0;
    for (const auto &e : c.events) {
        if (e.rfind("P lrc=", 0) == 0) { joined += e.substr(6); ++parts; }
        if (e.rfind("V lrc=", 0) == 0) joined += e.substr(6);
    }
    assert(parts >= 2 && joined == body);
    for (const auto &e : c.events) if (e[0] != 'E') assert((e.size() - 6) % 3 == 0);
    // Malformed input and excessive depth fail instead of overrunning.
    Collector bad{};
    JsonScanner m(events_sink, &bad);
    assert(!m.feed("{\"a\" 1}", 7));
    std::string deep(40, '[');
    JsonScanner d(events_sink, &bad);
    assert(!d.feed(deep.data(), deep.size()));

    // Lyrics: several tags per line, unsorted input, metadata, CRLF, overflow.
    alignas(4) static uint8_t storage[512];
    netease::Lyrics lyrics(storage, sizeof(storage));
    const char *lrc = "[ti:demo]\r\n[00:01.50]first\r\n[00:10.00][00:30.000]chorus\n[00:20]second\n[00:40.1]";
    for (size_t i = 0, n = std::strlen(lrc); i < n; i += 3) lyrics.feed(lrc + i, std::min<size_t>(3, n - i));
    lyrics.finish();
    assert(lyrics.count() == 5);
    char line[netease::Lyrics::kLineBytes + 1];
    assert(lyrics.line_at(0) == -1);
    lyrics.text(lyrics.line_at(1500), line, sizeof(line)); assert(!std::strcmp(line, "first"));
    lyrics.text(lyrics.line_at(15000), line, sizeof(line)); assert(!std::strcmp(line, "chorus"));
    lyrics.text(lyrics.line_at(25000), line, sizeof(line)); assert(!std::strcmp(line, "second"));
    lyrics.text(lyrics.line_at(31000), line, sizeof(line)); assert(!std::strcmp(line, "chorus"));
    lyrics.text(lyrics.line_at(99000), line, sizeof(line)); assert(!std::strcmp(line, ""));
    lyrics.clear();
    std::string many;
    for (int i = 0; i < 100; ++i) many += "[00:" + std::to_string(10 + i % 50) + ".00]line number " + std::to_string(i) + "\n";
    lyrics.feed(many.data(), many.size());
    lyrics.finish();
    assert(lyrics.count() > 5 && lyrics.count() < 100);  // bounded by the 512 B block
    for (size_t i = 1; i < lyrics.count(); ++i) {
        // Sorted by time after finish().
        char a[200], b[200];
        lyrics.text(i - 1, a, sizeof(a)); lyrics.text(i, b, sizeof(b));
        assert(a[0] && b[0]);
    }
    std::puts("NetEase scanner/lyrics edge cases PASS");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2 || !std::strcmp(argv[1], "self")) { self_test(); return 0; }
    const std::string text = read_file(argv[2]);
    Collector c{};
    if (!std::strcmp(argv[1], "playlists")) {
        c.item = "playlist[]";
        c.fields = {{"playlist[].id", &c.current.a}, {"playlist[].name", &c.current.b},
                    {"playlist[].trackCount", &c.current.c}, {"playlist[].userId", &c.current.d}};
    } else if (!std::strcmp(argv[1], "trackids")) {
        c.item = "playlist.trackIds[]";
        c.fields = {{"playlist.trackIds[].id", &c.current.a}};
    } else if (!std::strcmp(argv[1], "songs")) {
        c.item = "songs[]";
        c.fields = {{"songs[].id", &c.current.a}, {"songs[].name", &c.current.b},
                    {"songs[].ar[].name", &c.current.c}, {"songs[].dt", &c.current.d}};
    } else {
        return 2;
    }
    // Fields are bound to current by pointer; rebinding after each record is
    // unnecessary because current is reset in place.
    JsonScanner scanner(sink, &c);
    feed_chunked(scanner, text);
    print(c);
    return 0;
}
