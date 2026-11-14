#pragma once
#include "pomodoro_core.h"

struct PomodoroSnapshot {
    uint32_t remaining_seconds = 0, completed = 0;
    uint8_t phase = POMODORO_FOCUS, status = POMODORO_READY;
};
// Serializes access with UI and heartbeat. False means UI lock was busy;
// no command was executed. Does not create a page or change the foreground.
bool pomodoro_control(pomodoro_action_t action, uint8_t phase,
                      PomodoroSnapshot *out, pomodoro_command_result_t *result);

/* Existing 1 Hz system heartbeat; no new background task or timer. */
void pomodoro_service_tick(void);
