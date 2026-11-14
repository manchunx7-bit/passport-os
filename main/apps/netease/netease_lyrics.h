// LRC lyrics in a fixed caller-owned block: line index grows from the front,
// text from the back. Lines that do not fit are dropped, never reallocated.
#pragma once

#include <cstddef>
#include <cstdint>

namespace netease {

class Lyrics {
public:
    Lyrics(uint8_t *storage, size_t capacity) : storage_(storage), capacity_(capacity) { clear(); }

    void clear();
    // Feed raw LRC text in any chunking; call finish() after the last chunk.
    void feed(const char *text, size_t length);
    void finish();

    size_t count() const { return count_; }
    // Index of the line playing at ms, or -1 before the first line.
    int line_at(uint32_t ms) const;
    // Copies the line into out (NUL-terminated); empty for a bad index.
    void text(size_t index, char *out, size_t capacity) const;
    static constexpr size_t kLineBytes = 160;

private:
    struct Line { uint32_t ms; uint16_t offset; uint16_t length; };

    void parse_line();
    void add(uint32_t ms, const char *text, size_t length);
    Line *lines() const { return reinterpret_cast<Line *>(storage_); }

    uint8_t *storage_;
    size_t capacity_;
    size_t count_ = 0;
    size_t text_start_ = 0;  // text pool occupies [text_start_, capacity_)
    char line_[kLineBytes + 1]{};
    size_t line_len_ = 0;
    bool sorted_ = true;
};

}  // namespace netease
