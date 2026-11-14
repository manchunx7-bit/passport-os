// Streaming JSON field scanner for NetEase API responses.
//
// The responses are large (a user's playlist list is ~56 KB) while the fields
// the device needs are a few hundred bytes. Bytes are fed as they arrive from
// the socket; every scalar is reported with a compact path such as
// "playlist[].name" or "songs[].ar[].name", and every closed object with its
// own path ("songs[]"), so callers can collect one record at a time. Memory is
// fixed: no tree, no response buffer.
#pragma once

#include <cstddef>
#include <cstdint>

namespace netease {

class JsonScanner {
public:
    enum class Event : uint8_t {
        Value,      // complete scalar (string, number, true/false/null)
        ValuePart,  // leading chunk of a string longer than kValueBytes
        ObjectEnd,  // an object closed; path is the object's own path
    };
    // value is NUL-terminated (strings unescaped to UTF-8, literals verbatim).
    using Sink = void (*)(void *context, Event event, const char *path, const char *value, size_t length);

    static constexpr size_t kPathBytes = 96;
    static constexpr size_t kKeyBytes = 32;
    static constexpr size_t kValueBytes = 256;
    static constexpr size_t kMaxDepth = 16;

    JsonScanner(Sink sink, void *context) : sink_(sink), context_(context) {}

    // Returns false once the input is malformed or nested too deeply.
    bool feed(const char *data, size_t length);
    bool failed() const { return failed_; }
    // The root value has been closed.
    bool complete() const { return state_ == State::Done; }

private:
    enum class State : uint8_t {
        Value, KeyOrClose, Key, Colon, AfterValue, String, Escape, Unicode, Literal, Done,
    };
    struct Frame { bool object; uint8_t base; };

    bool step(char c);
    bool begin_value(char c);
    void enter_key();
    void finish_scalar();
    bool close_container(bool object);
    void append_value(const char *bytes, size_t n);
    void append_codepoint(uint32_t cp);
    void set_path(uint8_t base, const char *segment, size_t length, bool dot);

    Sink sink_;
    void *context_;
    State state_ = State::Value;
    bool failed_ = false;
    bool in_key_ = false;
    bool emitted_part_ = false;
    Frame frames_[kMaxDepth]{};
    uint8_t depth_ = 0;
    char path_[kPathBytes]{};
    uint8_t path_len_ = 0;
    char key_[kKeyBytes]{};
    uint8_t key_len_ = 0;
    char value_[kValueBytes + 1]{};
    uint16_t value_len_ = 0;
    uint32_t unicode_ = 0;
    uint8_t unicode_digits_ = 0;
    uint32_t high_surrogate_ = 0;
};

}  // namespace netease
