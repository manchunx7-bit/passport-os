#include "apps/pomodoro/pomodoro_core.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void finish(pomodoro_t *s, uint64_t *now) {
    assert(s->status == POMODORO_RUNNING);
    *now = s->deadline_us;
    assert(pomodoro_poll(s, *now));
    assert(!pomodoro_poll(s, *now + 1000000));
    assert(s->notice_pending && s->sound_pending && s->status == POMODORO_FINISHED);
}

int main(void) {
    pomodoro_t s = POMODORO_INITIALIZER;
    assert(sizeof(s) <= 32);
    assert(pomodoro_remaining_seconds(&s, 0) == 1500);
    assert(!pomodoro_poll(&s, 9000000000ULL));
    uint64_t now = 1000000;
    pomodoro_toggle(&s, now);
    assert(pomodoro_remaining_seconds(&s, now + 1) == 1500);
    assert(pomodoro_remaining_seconds(&s, now + 1000000) == 1499);
    assert(!pomodoro_select(&s, 1));
    now += 10000001;
    pomodoro_toggle(&s, now);
    const uint64_t paused = s.remaining_us;
    assert(s.status == POMODORO_PAUSED && !pomodoro_select(&s, -1));
    now += 3600000000ULL;
    assert(pomodoro_remaining_us(&s, now) == paused && !pomodoro_poll(&s, now));
    pomodoro_toggle(&s, now);
    assert(s.deadline_us == now + paused);
    /* Switching screens and light sleep do not participate in the state model. */
    now = s.deadline_us + 7200000000ULL;
    assert(pomodoro_poll(&s, now));
    assert(s.completed == 1 && !pomodoro_poll(&s, now + 86400000000ULL));
    assert(pomodoro_next_phase(&s) == POMODORO_SHORT_BREAK);
    assert(pomodoro_remaining_seconds(&s, now) == 0);
    pomodoro_toggle(&s, now);
    assert(s.phase == POMODORO_SHORT_BREAK && !s.notice_pending && !s.sound_pending);
    assert(pomodoro_remaining_seconds(&s, now) == 300);
    finish(&s, &now);
    assert(s.completed == 1 && pomodoro_next_phase(&s) == POMODORO_FOCUS);
    for (unsigned round = 2; round <= 4; ++round) {
        pomodoro_toggle(&s, now);
        assert(s.phase == POMODORO_FOCUS);
        finish(&s, &now);
        assert(s.completed == round);
        assert(pomodoro_next_phase(&s) == (round == 4 ? POMODORO_LONG_BREAK : POMODORO_SHORT_BREAK));
        pomodoro_toggle(&s, now);
        assert(pomodoro_remaining_seconds(&s, now) == (round == 4 ? 900 : 300));
        finish(&s, &now);
    }
    assert(s.completed == 4);
    pomodoro_reset(&s);
    assert(s.status == POMODORO_READY && !s.notice_pending && !s.sound_pending && s.completed == 4);
    assert(pomodoro_select(&s, -1) && s.phase == POMODORO_SHORT_BREAK);
    assert(pomodoro_select(&s, -1) && s.phase == POMODORO_FOCUS);
    assert(pomodoro_select(&s, -1) && s.phase == POMODORO_LONG_BREAK);
    assert(!pomodoro_select(&s, 0));
    s.phase = POMODORO_FOCUS; s.completed = UINT32_MAX;
    pomodoro_reset(&s); pomodoro_toggle(&s, now); finish(&s, &now);
    assert(s.completed == UINT32_MAX);
    /* Voice commands must not behave like a toggle or restart on retry. */
    s = (pomodoro_t)POMODORO_INITIALIZER;
    assert(pomodoro_command(&s,POMODORO_RESUME,0,now)==POMODORO_COMMAND_CONFLICT);
    assert(pomodoro_command(&s,POMODORO_START,POMODORO_FOCUS,now)==POMODORO_COMMAND_OK);
    uint64_t deadline=s.deadline_us;
    now+=10000000;
    assert(pomodoro_command(&s,POMODORO_START,POMODORO_FOCUS,now)==POMODORO_COMMAND_OK && s.deadline_us==deadline);
    assert(pomodoro_command(&s,POMODORO_START,POMODORO_SHORT_BREAK,now)==POMODORO_COMMAND_CONFLICT && s.deadline_us==deadline);
    assert(pomodoro_command(&s,POMODORO_PAUSE,0,now)==POMODORO_COMMAND_OK);
    const uint64_t voice_paused=s.remaining_us;
    now+=60000000;
    assert(pomodoro_command(&s,POMODORO_PAUSE,0,now)==POMODORO_COMMAND_OK && s.remaining_us==voice_paused);
    assert(pomodoro_command(&s,POMODORO_START,0,now)==POMODORO_COMMAND_CONFLICT);
    assert(pomodoro_command(&s,POMODORO_RESUME,0,now)==POMODORO_COMMAND_OK && s.deadline_us==now+voice_paused);
    deadline=s.deadline_us;now+=10000000;
    assert(pomodoro_command(&s,POMODORO_RESUME,0,now)==POMODORO_COMMAND_OK && s.deadline_us==deadline);
    now=deadline;
    assert(pomodoro_command(&s,POMODORO_PAUSE,0,now)==POMODORO_COMMAND_CONFLICT);
    assert(s.status==POMODORO_FINISHED && s.completed==1 && s.sound_pending && s.notice_pending);
    assert(pomodoro_command(&s,POMODORO_QUERY,0,now)==POMODORO_COMMAND_OK && s.completed==1);
    assert(pomodoro_command(&s,POMODORO_RESET,0,now)==POMODORO_COMMAND_OK && s.completed==1 && !s.sound_pending);
    assert(pomodoro_command(&s,POMODORO_START,3,now)==POMODORO_COMMAND_INVALID && s.status==POMODORO_READY);
    assert(pomodoro_command(&s,(pomodoro_action_t)99,0,now)==POMODORO_COMMAND_INVALID);
    for (uint8_t phase=0;phase<3;++phase) {
        assert(pomodoro_command(&s,POMODORO_START,phase,now)==POMODORO_COMMAND_OK);
        assert(s.phase==phase && s.remaining_us==pomodoro_duration_us(phase));
        assert(pomodoro_command(&s,POMODORO_RESET,0,now)==POMODORO_COMMAND_OK);
    }
    printf("Pomodoro: %zu-byte state, background/sleep, exact completion, phases and idempotent voice commands PASS\n", sizeof(s));
}
