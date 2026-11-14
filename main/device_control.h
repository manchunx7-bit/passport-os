#pragma once
#include "apps/app_base.h"
#include "voice_intent.h"
#include "apps/pomodoro/app_pomodoro.h"
#include <cstddef>
#include <cstdint>
#include <string>
struct cJSON;

namespace device_control {
enum class Action { Status, Volume, Brightness, Open, Pomodoro, Handover };
enum class Error { None, Busy, Inactive, Storage, Timeout, UnknownApp, AlreadyOpen, Alarm, TimerConflict, TimerInvalid };
struct Request {
    Action action = Action::Status;
    int value = 0;
    bool relative = false;
    passport_app_id_t app = APP_ID_COUNT;
    uint64_t token = 0;
    pomodoro_action_t timer_action = POMODORO_QUERY;
    uint8_t timer_phase = POMODORO_FOCUS;
    voice_intent::Request handover{};  // Handover: opens handover.app with it
};
struct Result {
    Error error = Error::None;
    uint8_t volume = 0, brightness = 0;
    uint8_t applied_brightness = 0;  // below brightness while the battery is low
    passport_app_id_t app = APP_ID_COUNT;
    PomodoroSnapshot timer{};
};
// Controller submits bounded work; the input task performs settings writes.
Result execute(const Request &request);
void poll();
bool pending();
void response_sent(uint64_t token, bool sent);
void cancel_session();
size_t tool_count();
const char *tool_schema(size_t index);
// Returns an MCP result object, or empty for a tool owned by another module.
std::string call(const char *name, const cJSON *arguments, uint64_t token);
}
