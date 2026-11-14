// AIHOT REST replies -> short Chinese text for Xiaozhi.
//
// Replies are 6-12 KB JSON; the assistant needs a few titles to read aloud.
// Collectors run on the streaming scanner and write straight into a bounded
// buffer, so memory stays fixed. Pure code: the host tests feed real replies.
#pragma once

#include "apps/netease/netease_json.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aihot {

using netease::JsonScanner;

constexpr size_t kTextBytes = 2048;   // whole tool reply text
constexpr size_t kTitleBytes = 150;   // ~50 Chinese characters
constexpr size_t kSummaryBytes = 120;  // ~40 Chinese characters
constexpr unsigned kMaxHot = 10, kMaxNews = 8;

// Length of the longest prefix of text within limit bytes that does not split
// a UTF-8 character.
inline size_t utf8_fit(const char *text, size_t limit) {
    size_t n = std::strlen(text);
    if (n <= limit) return n;
    n = limit;
    while (n && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    return n;
}

// Copies text, trimmed at a character boundary with an ellipsis when cut.
inline void clip(char *out, size_t capacity, const char *text) {
    static constexpr char kEllipsis[] = "…";
    const size_t whole = std::strlen(text);
    size_t n = utf8_fit(text, capacity - 1);
    const bool cut = n < whole;
    if (cut) n = utf8_fit(text, capacity - sizeof(kEllipsis));
    std::memcpy(out, text, n);
    if (cut) std::memcpy(out + n, kEllipsis, sizeof(kEllipsis) - 1), n += sizeof(kEllipsis) - 1;
    out[n] = 0;
}

// "2026-11-07T09:16:42.957Z" -> "11-07 17:16" in Beijing time (UTC+8).
inline void beijing_time(const char *iso, char *out, size_t capacity) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12) {
        if (capacity) out[0] = 0;
        return;
    }
    h += 8;
    if (h >= 24) {
        h -= 24;
        static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        if (++d > kDays[mo - 1] + (mo == 2 && leap)) { d = 1; mo = mo % 12 + 1; }
    }
    std::snprintf(out, capacity, "%02d-%02d %02d:%02d", mo, d, h, mi);
}

// Appends whole lines only: a line that does not fit ends the reply, so the
// assistant never reads half an entry.
struct Writer {
    char *text;
    size_t capacity;
    size_t length = 0;
    bool full = false;
    unsigned entries = 0;

    bool line(const char *value) {
        const size_t n = std::strlen(value);
        if (full || length + n + 2 > capacity) { full = true; return false; }
        std::memcpy(text + length, value, n);
        length += n;
        text[length++] = '\n';
        text[length] = 0;
        return true;
    }
};

inline bool is(const char *a, const char *b) { return !std::strcmp(a, b); }

// A string longer than the scanner buffer arrives as ValuePart chunks and a
// final Value with its tail. Only the leading chunk is wanted: the clip
// already marks the cut, and later chunks must not replace or repeat it.
struct LeadingChunk {
    bool tail = false;
    bool take(JsonScanner::Event event) {
        if (tail) {
            if (event == JsonScanner::Event::Value) tail = false;
            return false;
        }
        if (event == JsonScanner::Event::ValuePart) tail = true;
        return true;
    }
};

// GET /api/v1/hot-topics
struct HotCollector {
    Writer &out;
    unsigned limit;
    char title[kTitleBytes] = {};
    char time[16] = {};
    unsigned rank = 0, sources = 0;
    LeadingChunk chunk{};

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        auto &c = *static_cast<HotCollector *>(opaque);
        if (event == JsonScanner::Event::ObjectEnd) {
            if (!is(path, "items[]")) return;
            if (c.title[0] && c.out.entries < c.limit) {
                char line[kTitleBytes + 64];
                std::snprintf(line, sizeof(line), "%u. %s（%u 个信源，%s）", c.rank, c.title, c.sources, c.time);
                if (c.out.line(line)) ++c.out.entries;
            }
            c.title[0] = c.time[0] = 0;
            c.rank = c.sources = 0;
            return;
        }
        if (!c.chunk.take(event)) return;
        if (is(path, "items[].title")) clip(c.title, sizeof(c.title), value);
        else if (is(path, "items[].rank")) c.rank = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        else if (is(path, "items[].sourceCount")) c.sources = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        else if (is(path, "items[].latestAt")) beijing_time(value, c.time, sizeof(c.time));
    }
};

// GET /api/v1/items (latest selection or keyword search)
struct NewsCollector {
    Writer &out;
    unsigned limit;
    char title[kTitleBytes] = {};
    char summary[kSummaryBytes] = {};
    char source[48] = {};
    char time[16] = {};
    LeadingChunk chunk{};

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        auto &c = *static_cast<NewsCollector *>(opaque);
        if (event == JsonScanner::Event::ObjectEnd) {
            if (!is(path, "items[]")) return;
            if (c.title[0] && c.out.entries < c.limit) {
                char line[kTitleBytes + kSummaryBytes + 96];
                std::snprintf(line, sizeof(line), "%u. %s（%s，%s）%s%s", c.out.entries + 1, c.title, c.source, c.time,
                              c.summary[0] ? "：" : "", c.summary);
                if (c.out.line(line)) ++c.out.entries;
            }
            c.title[0] = c.summary[0] = c.source[0] = c.time[0] = 0;
            return;
        }
        // The leading chunk of a long summary is enough for its clipped form.
        if (!c.chunk.take(event)) return;
        if (is(path, "items[].title")) clip(c.title, sizeof(c.title), value);
        else if (is(path, "items[].summary")) clip(c.summary, sizeof(c.summary), value);
        else if (is(path, "items[].source.name")) clip(c.source, sizeof(c.source), value);
        else if (is(path, "items[].publishedAt"))
            beijing_time(value, c.time, sizeof(c.time));
    }
};

// GET /api/v1/dailies/latest: date, lead story, a few titles per section.
struct DailyCollector {
    Writer &out;
    static constexpr unsigned kPerSection = 3;
    char label[48] = {};
    char headline[kTitleBytes] = {};
    unsigned in_section = 0, flashes = 0;
    LeadingChunk chunk{};

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        auto &c = *static_cast<DailyCollector *>(opaque);
        char line[kTitleBytes + 64];
        if (event == JsonScanner::Event::ObjectEnd || !c.chunk.take(event)) return;
        if (is(path, "report.date")) {
            std::snprintf(line, sizeof(line), "AIHOT 日报 %s", value);
            c.out.line(line);
        } else if (is(path, "report.lead.title")) {
            clip(c.headline, sizeof(c.headline), value);
            std::snprintf(line, sizeof(line), "头条：%s", c.headline);
            c.out.line(line);
        } else if (is(path, "report.lead.leadParagraph")) {
            char paragraph[kSummaryBytes];
            clip(paragraph, sizeof(paragraph), value);
            c.out.line(paragraph);
        } else if (is(path, "report.sections[].label")) {
            clip(c.label, sizeof(c.label), value);
            c.in_section = 0;
        } else if (is(path, "report.sections[].items[].title") && c.in_section < kPerSection) {
            char title[kTitleBytes];
            clip(title, sizeof(title), value);
            std::snprintf(line, sizeof(line), "【%s】%s", c.label, title);
            if (c.out.line(line)) { ++c.in_section; ++c.out.entries; }
        } else if (is(path, "report.flashes[].title") && c.flashes < kPerSection) {
            char title[kTitleBytes];
            clip(title, sizeof(title), value);
            std::snprintf(line, sizeof(line), "【快讯】%s", title);
            if (c.out.line(line)) { ++c.flashes; ++c.out.entries; }
        }
    }
};

}  // namespace aihot
