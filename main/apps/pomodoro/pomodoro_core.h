#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POMODORO_FOCUS = 0,
    POMODORO_SHORT_BREAK,
    POMODORO_LONG_BREAK,
    POMODORO_PHASE_COUNT
} pomodoro_phase_t;

typedef enum {
    POMODORO_READY = 0,
    POMODORO_RUNNING,
    POMODORO_PAUSED,
    POMODORO_FINISHED
} pomodoro_status_t;

/* No heap, task, tick counter or wall-clock dependency. Retained until reboot. */
typedef struct {
    uint64_t deadline_us;
    uint64_t remaining_us;
    uint32_t completed;
    uint8_t phase;
    uint8_t status;
    bool notice_pending;
    bool sound_pending;
} pomodoro_t;

#define POMODORO_FOCUS_US (25ULL * 60 * 1000000)
#define POMODORO_INITIALIZER {0, POMODORO_FOCUS_US, 0, POMODORO_FOCUS, POMODORO_READY, false, false}

uint64_t pomodoro_duration_us(uint8_t phase);
uint64_t pomodoro_remaining_us(const pomodoro_t *s, uint64_t now_us);
uint32_t pomodoro_remaining_seconds(const pomodoro_t *s, uint64_t now_us);
/* True once per real completion; delayed polling never invents extra sessions. */
bool pomodoro_poll(pomodoro_t *s, uint64_t now_us);
uint8_t pomodoro_next_phase(const pomodoro_t *s);
void pomodoro_toggle(pomodoro_t *s, uint64_t now_us);
bool pomodoro_select(pomodoro_t *s, int direction); /* Ready/finished only. */
void pomodoro_reset(pomodoro_t *s); /* Current interval, keeps completed count. */

typedef enum {
    POMODORO_QUERY = 0, POMODORO_START, POMODORO_PAUSE,
    POMODORO_RESUME, POMODORO_RESET
} pomodoro_action_t;
typedef enum {
    POMODORO_COMMAND_OK = 0, POMODORO_COMMAND_CONFLICT, POMODORO_COMMAND_INVALID
} pomodoro_command_result_t;
/* Explicit commands are idempotent; START never overwrites a live interval.
 * phase is used by START only. No dynamic allocation or new timer. */
pomodoro_command_result_t pomodoro_command(pomodoro_t *s, pomodoro_action_t action,
                                           uint8_t phase, uint64_t now_us);

#ifdef __cplusplus
}
#endif
