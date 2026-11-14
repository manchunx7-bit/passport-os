#include "apps/netease/netease_url_cache.h"
#include "apps/netease/netease_meter.h"
#include "apps/netease/netease_voice_cache.h"
#include <cassert>
#include <string>
#include <cstdio>

using namespace netease;

int main() {
    VoiceSearchCache voice{};
    Song offered[2]{};offered[0].id=11;offered[1].id=22;
    std::snprintf(offered[0].name,sizeof(offered[0].name),"chosen song");
    std::snprintf(offered[1].artist,sizeof(offered[1].artist),"next artist");
    Song restored[5]{};uint64_t restored_ids[5]{};size_t restored_count=99;
    constexpr int64_t second=1000000;
    voice.save("query",offered,2,100*second);
    assert(voice.take("query",restored,restored_ids,5,&restored_count,101*second,false));
    assert(restored_count==2&&restored_ids[0]==11&&restored_ids[1]==22);
    assert(!std::strcmp(restored[0].name,"chosen song")&&!std::strcmp(restored[1].artist,"next artist"));
    assert(!voice.take("query",restored,restored_ids,5,&restored_count,101*second,false));
    voice.save("query",offered,2,100*second);
    assert(!voice.take("other",restored,restored_ids,5,&restored_count,101*second,false));
    voice.save("query",offered,2,100*second);
    assert(!voice.take("query",restored,restored_ids,5,&restored_count,146*second,false));
    voice.save("query",offered,2,100*second);
    assert(voice.take("query",restored,restored_ids,5,&restored_count,2*second,true));
    voice.save("query",offered,2,100*second);
    assert(!voice.take("query",restored,restored_ids,5,&restored_count,2*second,false));
    voice.save("query",offered,2,100*second);voice.songs[0].id^=1;
    assert(!voice.take("query",restored,restored_ids,5,&restored_count,101*second,false));
    voice.save("query",offered,2,100*second);voice.count=100;
    assert(!voice.take("query",restored,restored_ids,5,&restored_count,101*second,false));
    UrlCache cache;
    cache.entries[0].id = 11;
    cache.entries[1].id = 22;
    cache.entries[2].id = 33;
    UrlCollector collector{cache.entries, 3};
    JsonScanner scanner(UrlCollector::sink, &collector);
    const std::string long_url = "https://example.test/" + std::string(380, 'x');
    // Out of order, URL before ID, chunked signature, null/unplayable, trial.
    const std::string json = "{\"data\":[{\"url\":\"" + long_url +
        "\",\"freeTrialInfo\":{\"start\":0},\"code\":200,\"id\":22},"
        "{\"id\":33,\"url\":null,\"code\":404},"
        "{\"code\":200,\"url\":\"http://example.test/a\",\"id\":11}],\"code\":200}";
    for (size_t i = 0; i < json.size(); ++i) assert(scanner.feed(json.data() + i, 1));
    assert(scanner.complete() && collector.code == 200);
    assert(!cache.find(22, 10));  // not published until request success
    cache.fetched_at = 10;
    assert(cache.find(22, 11)->trial);
    assert(long_url == cache.find(22, 11)->url);
    assert(std::string(cache.find(11, 11)->url) == "http://example.test/a");
    assert(!cache.find(33, 11) && !cache.find(44, 11));
    assert(!cache.find(22, 9));
    assert(cache.find(22, 10 + UrlCache::kLifetimeUs - 1));
    assert(!cache.find(22, 10 + UrlCache::kLifetimeUs));
    cache.clear();
    assert(!cache.find(22, 11));
    assert(!cache.entries[1].url[0]);

    SongUrl out[2]{}; out[0].id = 1; out[1].id = 2;
    UrlCollector c{out, 2}; JsonScanner s(UrlCollector::sink, &c);
    const std::string overflow = "{\"data\":[{\"id\":1,\"url\":\"" + std::string(600, 'x') +
        "\",\"code\":200},{\"id\":2,\"url\":\"http://ok\",\"code\":200}],\"code\":200}";
    assert(s.feed(overflow.data(), overflow.size()) && s.complete());
    assert(!out[0].url[0] && std::string(out[1].url) == "http://ok");

    int16_t pcm[180]{};
    uint8_t levels[kMeterBars];
    meter_levels(pcm, 180, levels);
    for (auto value : levels) assert(value == 0);
    std::fill_n(pcm, 10, INT16_MIN);
    meter_levels(pcm, 180, levels);
    assert(levels[0] == 100);
    for (size_t i = 1; i < kMeterBars; ++i) assert(levels[i] == 0);
    std::fill_n(pcm, 180, 512);
    meter_levels(pcm, 180, levels);
    for (auto value : levels) assert(value > 0 && value < 100);
    meter_levels(nullptr, 0, levels);
    for (auto value : levels) assert(value == 0);
    std::puts("NetEase URL cache / chunked batch parser / PCM meter: PASS");
}
