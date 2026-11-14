#include "apps/netease/netease_lyrics.h"

#include <cstring>

namespace netease {

namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }

// "[mm:ss.xx]" or "[mm:ss.xxx]" or "[mm:ss]"; returns the tag length or 0.
size_t parse_tag(const char *s, size_t n, uint32_t *ms) {
    if (n < 6 || s[0] != '[') return 0;
    size_t i = 1;
    uint32_t minutes = 0, seconds = 0, fraction = 0, scale = 1;
    if (!digit(s[i])) return 0;
    while (i < n && digit(s[i])) minutes = minutes * 10 + (s[i++] - '0');
    if (i >= n || s[i++] != ':') return 0;
    if (i >= n || !digit(s[i])) return 0;
    while (i < n && digit(s[i])) seconds = seconds * 10 + (s[i++] - '0');
    if (i < n && (s[i] == '.' || s[i] == ':')) {
        ++i;
        while (i < n && digit(s[i])) {
            if (scale < 1000) { fraction = fraction * 10 + (s[i] - '0'); scale *= 10; }
            ++i;
        }
    }
    if (i >= n || s[i] != ']') return 0;
    *ms = (minutes * 60 + seconds) * 1000 + (scale > 1 ? fraction * 1000 / scale : 0);
    return i + 1;
}
}  // namespace

void Lyrics::clear() {
    count_ = 0;
    text_start_ = capacity_;
    line_len_ = 0;
    sorted_ = true;
}

void Lyrics::add(uint32_t ms, const char *text, size_t length) {
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\r')) --length;
    while (length && *text == ' ') { ++text; --length; }
    const size_t index_end = (count_ + 1) * sizeof(Line);
    // An identical text (repeated chorus with several tags) shares one copy.
    uint16_t offset = 0;
    bool shared = false;
    if (count_) {
        const Line &last = lines()[count_ - 1];
        if (last.length == length && std::memcmp(storage_ + last.offset, text, length) == 0) {
            offset = last.offset;
            shared = true;
        }
    }
    if (!shared) {
        if (index_end + length > text_start_) return;
        text_start_ -= length;
        std::memcpy(storage_ + text_start_, text, length);
        offset = static_cast<uint16_t>(text_start_);
    } else if (index_end > text_start_) {
        return;
    }
    if (count_ && lines()[count_ - 1].ms > ms) sorted_ = false;
    lines()[count_++] = {ms, offset, static_cast<uint16_t>(length)};
}

void Lyrics::parse_line() {
    const char *s = line_;
    size_t n = line_len_;
    uint32_t stamps[8];
    size_t tags = 0;
    for (;;) {
        uint32_t ms;
        const size_t used = parse_tag(s, n, &ms);
        if (!used) break;
        if (tags < 8) stamps[tags++] = ms;
        s += used;
        n -= used;
    }
    for (size_t i = 0; i < tags; ++i) add(stamps[i], s, n);
    line_len_ = 0;
}

void Lyrics::feed(const char *text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '\n') { parse_line(); continue; }
        if (line_len_ < kLineBytes) line_[line_len_++] = c;
    }
}

void Lyrics::finish() {
    if (line_len_) parse_line();
    if (sorted_) return;
    // Few dozen lines: insertion sort keeps the code and stack tiny.
    Line *l = lines();
    for (size_t i = 1; i < count_; ++i) {
        const Line key = l[i];
        size_t j = i;
        while (j > 0 && l[j - 1].ms > key.ms) { l[j] = l[j - 1]; --j; }
        l[j] = key;
    }
    sorted_ = true;
}

int Lyrics::line_at(uint32_t ms) const {
    int lo = 0, hi = static_cast<int>(count_) - 1, found = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (lines()[mid].ms <= ms) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return found;
}

void Lyrics::text(size_t index, char *out, size_t capacity) const {
    if (!capacity) return;
    out[0] = 0;
    if (index >= count_) return;
    const Line &line = lines()[index];
    const size_t n = line.length < capacity - 1 ? line.length : capacity - 1;
    std::memcpy(out, storage_ + line.offset, n);
    out[n] = 0;
}

}  // namespace netease
