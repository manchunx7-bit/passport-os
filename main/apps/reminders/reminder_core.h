#pragma once
#include <cstddef>
#include <cstdint>

namespace reminders {
constexpr unsigned kCapacity = 20;
constexpr unsigned kTitleBytes = 96;
constexpr int64_t kMaxAhead = 366LL * 86400;
enum class Status : uint32_t { Pending = 1, Done = 2, Missed = 3 };
struct RepeatRule {
    uint32_t local_second;
    int16_t utc_offset_minutes;
    uint8_t days; // Monday bit 0 .. Sunday bit 6; zero means one-time.
    uint8_t version;
};
struct Record {
    uint32_t id;
    Status status;
    int64_t due;
    char title[kTitleBytes];
    RepeatRule repeat;
};
static_assert(sizeof(Record) == 120);
constexpr size_t kLegacyRecordBytes = 112;
bool valid_rule(const RepeatRule &rule);
bool make_rule(uint8_t days, int64_t due, int offset_seconds, RepeatRule *out);
int64_t next_occurrence(const Record &record, int64_t after);
const char *repeat_name(uint8_t days);
bool valid_title(const char *title);
bool valid_record(const Record &record);
// Strict local YYYY-MM-DD HH:MM[:SS], converted with an explicit UTC offset.
bool parse_local(const char *text, int offset_seconds, int64_t *utc);
void format_local(int64_t utc, int offset_seconds, char *out, size_t cap);
const char *status_text(Status status);
}
