#include "apps/netease/netease_json.h"

#include <cstring>

namespace netease {

namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool JsonScanner::feed(const char *data, size_t length) {
    for (size_t i = 0; i < length && !failed_; ++i) {
        if (!step(data[i])) failed_ = true;
    }
    return !failed_;
}

void JsonScanner::set_path(uint8_t base, const char *segment, size_t length, bool dot) {
    size_t len = base;
    if (dot && len + 1 < kPathBytes) path_[len++] = '.';
    const size_t room = kPathBytes - 1 - len;
    if (length > room) length = room;
    std::memcpy(path_ + len, segment, length);
    len += length;
    path_[len] = 0;
    path_len_ = static_cast<uint8_t>(len);
}

void JsonScanner::append_value(const char *bytes, size_t n) {
    if (in_key_) {
        for (size_t i = 0; i < n && size_t(key_len_) + 1 < kKeyBytes; ++i) key_[key_len_++] = bytes[i];
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (value_len_ >= kValueBytes) {
            // Hand over everything up to the last complete UTF-8 sequence and
            // keep the unfinished tail for the next chunk.
            size_t cut = value_len_;
            size_t lead = cut;
            while (lead > 0 && (static_cast<uint8_t>(value_[lead - 1]) & 0xC0) == 0x80) --lead;
            if (lead > 0) {
                const uint8_t b = static_cast<uint8_t>(value_[lead - 1]);
                const size_t need = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
                if (cut - (lead - 1) < need) cut = lead - 1;
            }
            const char keep = value_[cut];
            value_[cut] = 0;
            sink_(context_, Event::ValuePart, path_, value_, cut);
            value_[cut] = keep;
            emitted_part_ = true;
            std::memmove(value_, value_ + cut, value_len_ - cut);
            value_len_ = static_cast<uint16_t>(value_len_ - cut);
        }
        value_[value_len_++] = bytes[i];
    }
}

void JsonScanner::append_codepoint(uint32_t cp) {
    char out[4];
    size_t n;
    if (cp < 0x80) { out[0] = static_cast<char>(cp); n = 1; }
    else if (cp < 0x800) { out[0] = static_cast<char>(0xC0 | (cp >> 6)); out[1] = static_cast<char>(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12)); out[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
        out[2] = static_cast<char>(0x80 | (cp & 63)); n = 3;
    } else {
        out[0] = static_cast<char>(0xF0 | (cp >> 18)); out[1] = static_cast<char>(0x80 | ((cp >> 12) & 63));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 63)); out[3] = static_cast<char>(0x80 | (cp & 63)); n = 4;
    }
    append_value(out, n);
}

void JsonScanner::finish_scalar() {
    value_[value_len_] = 0;
    sink_(context_, Event::Value, path_, value_, value_len_);
    value_len_ = 0;
    path_len_ = depth_ ? frames_[depth_ - 1].base : 0;
    path_[path_len_] = 0;
    state_ = depth_ ? State::AfterValue : State::Done;
}

bool JsonScanner::close_container(bool object) {
    if (!depth_ || frames_[depth_ - 1].object != object) return false;
    path_len_ = frames_[depth_ - 1].base;
    path_[path_len_] = 0;
    if (object) sink_(context_, Event::ObjectEnd, path_, "", 0);
    --depth_;
    path_len_ = depth_ ? frames_[depth_ - 1].base : 0;
    path_[path_len_] = 0;
    state_ = depth_ ? State::AfterValue : State::Done;
    return true;
}

bool JsonScanner::begin_value(char c) {
    if (depth_ && !frames_[depth_ - 1].object) {
        if (c == ']') return close_container(false);  // empty array
        set_path(frames_[depth_ - 1].base, "[]", 2, false);
    }
    if (c == '{' || c == '[') {
        if (depth_ >= kMaxDepth) return false;
        frames_[depth_++] = {c == '{', path_len_};
        state_ = c == '{' ? State::KeyOrClose : State::Value;
        return true;
    }
    value_len_ = 0;
    emitted_part_ = false;
    if (c == '"') {
        in_key_ = false;
        state_ = State::String;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
        value_[value_len_++] = c;
        state_ = State::Literal;
        return true;
    }
    return false;
}

void JsonScanner::enter_key() {
    in_key_ = true;
    key_len_ = 0;
    state_ = State::String;
}

bool JsonScanner::step(char c) {
    switch (state_) {
    case State::Value:
        if (space(c)) return true;
        return begin_value(c);
    case State::KeyOrClose:
        if (space(c)) return true;
        if (c == '}') return close_container(true);
        if (c != '"') return false;
        enter_key();
        return true;
    case State::Key:
        if (space(c)) return true;
        if (c != '"') return false;
        enter_key();
        return true;
    case State::Colon:
        if (space(c)) return true;
        if (c != ':') return false;
        state_ = State::Value;
        return true;
    case State::AfterValue:
        if (space(c)) return true;
        if (c == ',') {
            state_ = frames_[depth_ - 1].object ? State::Key : State::Value;
            return true;
        }
        if (c == '}' || c == ']') return close_container(c == '}');
        return false;
    case State::String:
        if (c == '\\') { state_ = State::Escape; return true; }
        if (c == '"') {
            high_surrogate_ = 0;
            if (in_key_) {
                key_[key_len_] = 0;
                const Frame &top = frames_[depth_ - 1];
                set_path(top.base, key_, key_len_, top.base > 0);
                in_key_ = false;
                state_ = State::Colon;
            } else {
                finish_scalar();
            }
            return true;
        }
        append_value(&c, 1);
        return true;
    case State::Escape: {
        state_ = State::String;
        char out;
        switch (c) {
        case 'n': out = '\n'; break;
        case 't': out = '\t'; break;
        case 'r': out = '\r'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'u': unicode_ = 0; unicode_digits_ = 0; state_ = State::Unicode; return true;
        default: out = c; break;  // \" \\ \/
        }
        append_value(&out, 1);
        return true;
    }
    case State::Unicode: {
        const int digit = hex(c);
        if (digit < 0) return false;
        unicode_ = (unicode_ << 4) | static_cast<uint32_t>(digit);
        if (++unicode_digits_ < 4) return true;
        state_ = State::String;
        if (unicode_ >= 0xD800 && unicode_ <= 0xDBFF) {
            high_surrogate_ = unicode_;
        } else if (unicode_ >= 0xDC00 && unicode_ <= 0xDFFF) {
            if (high_surrogate_) append_codepoint(0x10000 + ((high_surrogate_ - 0xD800) << 10) + (unicode_ - 0xDC00));
            high_surrogate_ = 0;
        } else {
            high_surrogate_ = 0;
            append_codepoint(unicode_);
        }
        return true;
    }
    case State::Literal:
        if (space(c) || c == ',' || c == '}' || c == ']') {
            finish_scalar();
            return step(c);
        }
        if (value_len_ < kValueBytes) value_[value_len_++] = c;
        return true;
    case State::Done:
        return true;
    }
    return false;
}

}  // namespace netease
