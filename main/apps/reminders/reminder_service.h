#pragma once
#include "reminder_core.h"
#include <string>

namespace reminders {
void initialize();
// Runs on the input task (full stack), outside the registry/LVGL locks.
void poll();
size_t list(Record *out, size_t capacity);
bool get(uint32_t id, Record *out);
bool create(const char *title, int64_t due, uint32_t *id, const char **error);
bool create_repeating(const char *title, int64_t due, const RepeatRule &rule, uint32_t *id, const char **error);
// Null title / zero due / null rule retain the corresponding existing field.
bool update(uint32_t id, const char *title, int64_t due, const RepeatRule *rule, const char **error);
bool change(uint32_t id, bool erase, int64_t snooze_until, const char **error, int64_t expected_due = 0);
bool clock_ready();
uint32_t revision();
bool sound_pending();
bool alarm_active();
// Input task only: hide/quiet this occurrence without completing/deleting it.
// Its normal timeout still records a miss or advances a recurring reminder.
void dismiss_for_navigation();
// Only the music worker may call this, AFTER releasing its codec/decoder.
void play_music_notice(bool (*cancelled)());
// Called under registry lock; never persists here. All gestures are consumed
// while the alert is visible. Actual actions are queued to poll().
bool intercept_key(uint8_t button, int event);
void show_alert(const Record *record, const char *error = nullptr);
std::string mcp_result(const char *method, const char *params, bool *rpc_error, uint64_t token = 0);
}
