#include "pomodoro_core.h"
#include <limits.h>

uint64_t pomodoro_duration_us(uint8_t phase) {
    static const uint8_t minutes[] = {25, 5, 15};
    return (uint64_t)minutes[phase < POMODORO_PHASE_COUNT ? phase : POMODORO_FOCUS] * 60000000ULL;
}

uint64_t pomodoro_remaining_us(const pomodoro_t *s, uint64_t now_us) {
    if (s->status != POMODORO_RUNNING) return s->remaining_us;
    return now_us < s->deadline_us ? s->deadline_us - now_us : 0;
}

uint32_t pomodoro_remaining_seconds(const pomodoro_t *s, uint64_t now_us) {
    /* Round up: 25:00 must not become 24:59 immediately after pressing start. */
    uint64_t us = pomodoro_remaining_us(s, now_us);
    return (uint32_t)(us / 1000000ULL + (us % 1000000ULL != 0));
}

bool pomodoro_poll(pomodoro_t *s, uint64_t now_us) {
    if (s->status != POMODORO_RUNNING || pomodoro_remaining_us(s, now_us)) return false;
    s->status = POMODORO_FINISHED;
    s->remaining_us = 0;
    if (s->phase == POMODORO_FOCUS && s->completed < UINT32_MAX) ++s->completed;
    s->notice_pending = true;
    s->sound_pending = true;
    return true;
}

uint8_t pomodoro_next_phase(const pomodoro_t *s) {
    if (s->phase != POMODORO_FOCUS) return POMODORO_FOCUS;
    return s->completed && s->completed % 4 == 0 ? POMODORO_LONG_BREAK : POMODORO_SHORT_BREAK;
}

void pomodoro_toggle(pomodoro_t *s, uint64_t now_us) {
    pomodoro_poll(s, now_us);
    if (s->status == POMODORO_RUNNING) {
        s->remaining_us = pomodoro_remaining_us(s, now_us);
        s->status = POMODORO_PAUSED;
        return;
    }
    if (s->status == POMODORO_FINISHED) {
        s->phase = pomodoro_next_phase(s);
        s->remaining_us = pomodoro_duration_us(s->phase);
    }
    s->deadline_us = now_us + s->remaining_us;
    s->notice_pending = false;
    s->sound_pending = false;
    s->status = POMODORO_RUNNING;
}

bool pomodoro_select(pomodoro_t *s, int direction) {
    if (!direction || (s->status != POMODORO_READY && s->status != POMODORO_FINISHED)) return false;
    s->phase = (uint8_t)((s->phase + (direction > 0 ? 1 : POMODORO_PHASE_COUNT - 1)) % POMODORO_PHASE_COUNT);
    pomodoro_reset(s);
    return true;
}

void pomodoro_reset(pomodoro_t *s) {
    s->deadline_us = 0;
    s->remaining_us = pomodoro_duration_us(s->phase);
    s->status = POMODORO_READY;
    s->notice_pending = false;
    s->sound_pending = false;
}

pomodoro_command_result_t pomodoro_command(pomodoro_t *s, pomodoro_action_t action,
                                           uint8_t phase, uint64_t now_us) {
    if (action < POMODORO_QUERY || action > POMODORO_RESET ||
        (action == POMODORO_START && phase >= POMODORO_PHASE_COUNT))
        return POMODORO_COMMAND_INVALID;
    pomodoro_poll(s, now_us);
    switch (action) {
    case POMODORO_QUERY: return POMODORO_COMMAND_OK;
    case POMODORO_START:
        if (s->status == POMODORO_RUNNING)
            return s->phase == phase ? POMODORO_COMMAND_OK : POMODORO_COMMAND_CONFLICT;
        if (s->status == POMODORO_PAUSED) return POMODORO_COMMAND_CONFLICT;
        s->phase = phase;
        pomodoro_reset(s);
        pomodoro_toggle(s, now_us);
        break;
    case POMODORO_PAUSE:
        if (s->status == POMODORO_PAUSED) return POMODORO_COMMAND_OK;
        if (s->status != POMODORO_RUNNING) return POMODORO_COMMAND_CONFLICT;
        pomodoro_toggle(s, now_us);
        break;
    case POMODORO_RESUME:
        if (s->status == POMODORO_RUNNING) return POMODORO_COMMAND_OK;
        if (s->status != POMODORO_PAUSED) return POMODORO_COMMAND_CONFLICT;
        pomodoro_toggle(s, now_us);
        break;
    case POMODORO_RESET: pomodoro_reset(s); break;
    }
    return POMODORO_COMMAND_OK;
}
