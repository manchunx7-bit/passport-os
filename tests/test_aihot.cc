// AIHOT reply formatting on real public responses (tests/fixtures/aihot).
#include "apps/aihot/aihot_format.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

using namespace aihot;

namespace {

std::string read(const char *name) {
    std::string path = std::string("tests/fixtures/aihot/") + name, body;
    FILE *f = std::fopen(path.c_str(), "rb");
    assert(f);
    char chunk[333];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) body.append(chunk, got);
    std::fclose(f);
    return body;
}

template <typename Collector>
std::string run(const char *fixture, Collector &collector, size_t step = 97) {
    JsonScanner scanner(Collector::sink, &collector);
    const std::string body = read(fixture);
    for (size_t i = 0; i < body.size(); i += step) assert(scanner.feed(body.data() + i, std::min(step, body.size() - i)));
    assert(scanner.complete());
    return collector.out.text;
}

bool valid_utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

}  // namespace

int main() {
    char text[kTextBytes];
    char t[16];
    beijing_time("2026-11-07T09:16:42.957Z", t, sizeof(t)); assert(!std::strcmp(t, "11-07 17:16"));
    beijing_time("2026-12-31T20:05:00Z", t, sizeof(t)); assert(!std::strcmp(t, "01-01 04:05"));
    beijing_time("2028-02-28T16:00:00Z", t, sizeof(t)); assert(!std::strcmp(t, "02-29 00:00"));
    beijing_time("garbage", t, sizeof(t)); assert(!t[0]);
    char c[10];
    clip(c, sizeof(c), "孤勇者孤勇者"); assert(!std::strcmp(c, "孤勇…"));  // 9 bytes: 2 chars + ellipsis
    clip(c, sizeof(c), "abc"); assert(!std::strcmp(c, "abc"));

    {
        Writer out{text, sizeof(text)};
        HotCollector hot{out, kMaxHot};
        const std::string s = run("hot_topics.json", hot);
        assert(out.entries == 10 && valid_utf8(s) && s.size() < kTextBytes);
        assert(s.rfind("1. Google 发布 Omni 1.2 Flash，视频生成提至 15 秒（11 个信源，11-06 10:29）\n", 0) == 0);
        assert(s.find("\n10. ") != std::string::npos);
    }
    {
        Writer out{text, sizeof(text)};
        HotCollector hot{out, 3};
        const std::string s = run("hot_topics.json", hot, 7);  // tiny chunks: values split across feeds
        assert(out.entries == 3 && s.find("\n3. ") != std::string::npos && s.find("\n4. ") == std::string::npos);
    }
    {
        Writer out{text, sizeof(text)};
        NewsCollector news{out, kMaxNews};
        const std::string s = run("items.json", news);
        assert(out.entries >= 6 && valid_utf8(s) && s.size() < kTextBytes);
        // 463-byte summary: clipped at a character boundary with an ellipsis.
        assert(s.rfind("1. 美国家长起诉谷歌 Gemini 被指协助 13 岁女孩制定轻生计划（IT之家·人工智能，11-07 17:16）：美国北卡罗来纳州", 0) == 0);
        assert(s.find("…\n2. ") != std::string::npos);
    }
    {
        Writer out{text, sizeof(text)};
        NewsCollector news{out, 2};
        const std::string s = run("search.json", news);
        assert(out.entries == 2 && s.find("1. 法官裁定 Anthropic") == 0 && s.find("\n3. ") == std::string::npos);
    }
    {
        // A small reply buffer keeps whole lines only.
        char small[300];
        Writer out{small, sizeof(small)};
        NewsCollector news{out, kMaxNews};
        const std::string s = run("items.json", news);
        assert(out.full && out.entries >= 1 && s.size() < sizeof(small) && s.back() == '\n' && valid_utf8(s));
    }
    {
        Writer out{text, sizeof(text)};
        DailyCollector daily{out};
        const std::string s = run("daily.json", daily);
        assert(valid_utf8(s) && s.size() < kTextBytes);
        assert(s.rfind("AIHOT 日报 2026-11-07\n头条：Bloomberg：OpenAI 完成 110 亿美元融资首批到账，估值达 1.25 万亿美元\n", 0) == 0);
        assert(s.find("【模型发布/更新】百度发布 ERNIE-5.5-Flash-Base") != std::string::npos);
        assert(s.find("【快讯】Artificial Analysis 实测 GPT Image 2.6") != std::string::npos);
        // At most three per section and three flashes; no line repeated from a split string.
        size_t flashes = 0;
        for (size_t at = 0; (at = s.find("【快讯】", at)) != std::string::npos; ++at) ++flashes;
        assert(flashes == 3 && s.find("头条：", s.find("头条：") + 1) == std::string::npos);
    }
    std::puts("AIHOT: hot topics, news/search, daily, Beijing time, UTF-8 clipping, whole-line budget PASS");
}
