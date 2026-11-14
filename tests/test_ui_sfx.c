#include "game_audio_mocks/platform.h"
#include "../main/ui_sfx.c"
#include "../main/ui_sfx_tone.c"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void (*worker)(void *);
static int64_t now = 1000000;
static int creates, deletes, inits, deinits, writes, notifications;
static size_t written_bytes;
static int volume = 80, init_result, format_result, write_result;
static bool create_failure, stop_on_write, refresh_on_wait, expire_init;
static bool request_on_drain, request_on_deinit, stop_on_deinit;
static bool request_on_init;
static unsigned notification_pending;
static unsigned refresh_after_us = 100000;
int64_t esp_timer_get_time(void) { return now; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, TaskHandle_t *out) {
    (void)name; (void)stack; (void)arg;
    assert(prio > 4 && prio < 6);
    if (create_failure) return 0;
    worker = fn; *out = (void *)1; notification_pending = 0; ++creates; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(!task); ++deletes; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task == (void *)1); ++notifications; ++notification_pending; }
void vTaskDelay(unsigned ticks) {
    now += ticks * 1000;
    if (request_on_drain) {
        request_on_drain = false;
        ui_sfx_play(UI_SFX_CONFIRM);
    }
}
unsigned ulTaskNotifyTake(int clear, unsigned ticks) {
    (void)clear;
    if (refresh_on_wait) {
        refresh_on_wait = false;
        now += refresh_after_us;
        ui_sfx_play(UI_SFX_CONFIRM);
    }
    if (notification_pending) { const unsigned n = notification_pending; notification_pending = 0; return n; }
    if (atomic_load(&s_stop)) return 1;
    now += ticks * 1000; return 0;
}
int bsp_audio_init_feedback(void) {
    ++inits;
    if (expire_init) now += 200000;
    if (request_on_init) { request_on_init = false; now += 60000; ui_sfx_play(UI_SFX_CONFIRM); }
    return init_result;
}
int bsp_audio_set_format(unsigned rate, uint8_t bits, uint8_t ch) {
    assert(rate == 16000 && bits == 16 && ch == 1); return format_result;
}
static int alert_levels;
void bsp_audio_set_volume(uint8_t v) { assert(v <= 85); }
void bsp_audio_set_alert_volume(uint8_t v) { assert(v == UI_SFX_ALARM_VOLUME); ++alert_levels; }
uint8_t bsp_audio_get_master_volume(void) { return volume; }
void bsp_audio_deinit(void) {
    ++deinits;
    if (request_on_deinit) {
        request_on_deinit = false;
        ui_sfx_play(UI_SFX_CONFIRM);
    }
    if (stop_on_deinit) { stop_on_deinit = false; assert(!ui_sfx_prepare_stop()); }
}
int bsp_audio_write(const void *pcm, size_t bytes) {
    assert(pcm && bytes <= 256); ++writes; written_bytes += bytes;
    if (stop_on_write) { stop_on_write = false; assert(!ui_sfx_prepare_stop()); }
    return write_result;
}
static void play(void) { now += 1000000; ui_sfx_play(UI_SFX_NAV); }
static void finish(void) { assert(s_running); worker(NULL); assert(!s_running && !s_task && s_pending == -1); }
int main(void) {
    for (int kind = 0; kind <= UI_SFX_BACK; ++kind) {
        const unsigned n = ui_sfx_samples(kind);
        assert(n >= 560 && n <= 800);
        int peak = 0, previous = 0; long sum = 0;
        double head_energy = 0, tail_energy = 0;
        for (unsigned i = 0; i < n; ++i) {
            const int sample = ui_sfx_sample(kind, i);
            assert(abs(sample - previous) < 16000);
            if (abs(sample) > peak) peak = abs(sample);
            sum += sample; previous = sample;
            if (i < n / 3) head_energy += (double)sample * sample;
            if (i >= n * 2 / 3) tail_energy += (double)sample * sample;
        }
        assert(peak > 15000 && peak < 20000 && labs(sum) < (long)n * 80);
        assert(head_energy > 5 * tail_energy);
        assert(!ui_sfx_sample(kind, 0) && !ui_sfx_sample(kind, n - 1));
        assert(abs(ui_sfx_sample(kind, n - 2)) <= 2);
        // A crisp tone should not carry broadband hiss above 4 kHz.
        double total_energy = 0, high_energy = 0;
        for (unsigned i = 0; i < n; ++i) {
            const double sample = ui_sfx_sample(kind, i);
            total_energy += sample * sample;
        }
        for (unsigned bin = n / 4; bin <= n / 2; ++bin) {
            double real = 0, imag = 0;
            for (unsigned i = 0; i < n; ++i) {
                const double angle = 6.28318530718 * bin * i / n;
                real += ui_sfx_sample(kind, i) * cos(angle);
                imag += ui_sfx_sample(kind, i) * sin(angle);
            }
            high_energy += real * real + imag * imag;
        }
        assert(2 * high_energy / (n * total_energy) < .001);
    }
    assert(!ui_sfx_samples(-1) && !ui_sfx_samples(UI_SFX_ALARM + 1));
    assert(!ui_sfx_sample(-1, 0) && !ui_sfx_sample(UI_SFX_ALARM + 1, 0));
    assert(!ui_sfx_sample(UI_SFX_NAV, ui_sfx_samples(UI_SFX_NAV)));
    // Separate valid clicks may be dispatched only 60 ms apart after a UI stall.
    refresh_after_us = 60000; refresh_on_wait = true;
    size_t bytes_before = written_bytes;
    play(); finish();
    const size_t two_tones = (ui_sfx_samples(UI_SFX_NAV) + ui_sfx_samples(UI_SFX_CONFIRM)) * sizeof(int16_t) + 512;
    const bool rapid_ok = written_bytes - bytes_before == two_tones;
    // A click arriving in the idle drain or driver teardown must survive cleanup.
    bytes_before = written_bytes; request_on_drain = true; play(); finish();
    const bool drain_ok = written_bytes - bytes_before == two_tones;
    bytes_before = written_bytes; request_on_deinit = true; play(); finish();
    const bool teardown_ok = written_bytes - bytes_before == two_tones;
    bytes_before = written_bytes; request_on_init = true; play(); finish();
    const bool startup_ok = written_bytes - bytes_before == two_tones;
    fprintf(stderr, "Click delivery: 60ms=%d idle-drain=%d driver-teardown=%d cold-start=%d\n", rapid_ok, drain_ok, teardown_ok, startup_ok);
    assert(rapid_ok && drain_ok && teardown_ok && startup_ok);
    // An audio-app handoff must cancel a click queued during teardown.
    bytes_before = written_bytes;
    const int handoff_inits = inits;
    request_on_deinit = true; stop_on_deinit = true; play(); finish();
    assert(inits == handoff_inits + 1);
    assert(written_bytes - bytes_before == ui_sfx_samples(UI_SFX_NAV) * sizeof(int16_t) + 256);
    int old_creates = creates;
    volume = 0; play(); assert(!s_running && creates == old_creates); volume = 80;
    ui_sfx_set_enabled(false); play(); assert(!s_running); ui_sfx_set_enabled(true);
    create_failure = true; play(); assert(!s_running); create_failure = false;
    play(); for (int i = 0; i < 100; ++i) ui_sfx_play(UI_SFX_CONFIRM);
    assert(creates == old_creates + 1 && s_pending == UI_SFX_CONFIRM); finish(); assert(writes > 0);
    int before = writes;
    int before_inits = inits, before_deinits = deinits;
    play(); assert(!ui_sfx_prepare_stop()); finish(); assert(inits == before_inits && writes == before);
    init_result = ESP_FAIL; play(); finish(); assert(deinits == before_deinits); init_result = 0;
    format_result = ESP_FAIL; play(); finish(); assert(deinits == before_deinits + 1); format_result = 0;
    // Cold codec setup must not expire a request that was fresh when accepted.
    expire_init = true; before = writes; play(); finish(); assert(writes > before); expire_init = false;
    before = writes; before_inits = inits; play(); now += 200000; finish();
    assert(writes == before && inits == before_inits);
    const size_t before_bytes = written_bytes;
    stop_on_write = true; play(); finish();
    assert(written_bytes - before_bytes == ui_sfx_samples(UI_SFX_NAV) * sizeof(int16_t) + 256);
    write_result = ESP_FAIL; before = writes; play(); finish(); assert(writes - before == 1); write_result = 0;
    refresh_on_wait = true; before = writes; play(); finish(); assert(writes - before >= 9);
    play(); ui_sfx_set_enabled(false); before = writes; finish(); assert(writes == before); ui_sfx_set_enabled(true);
    play(); volume = 0; before = writes; finish(); assert(writes == before); volume = 80;
    for (int i = 0; i < 30; ++i) { play(); finish(); }
    // Alarm has six loud beeps in three groups, even with clicks disabled.
    assert(ui_sfx_samples(UI_SFX_ALARM) == 24000);
    for (unsigned group = 0; group < 3; ++group) {
        const unsigned base = group * 8000;
        int peak = 0, previous = 0; long sum = 0; double energy = 0;
        for (unsigned i = 0; i < 8000; ++i) {
            const int sample = ui_sfx_sample(UI_SFX_ALARM, base + i);
            const bool beep = i < UI_SFX_ALARM_BEEP ||
                (i >= UI_SFX_ALARM_BEEP + UI_SFX_ALARM_GAP && i < UI_SFX_ALARM_BEEP * 2 + UI_SFX_ALARM_GAP);
            if (!beep) assert(!sample);
            // Ramped edges: no gate click at the start or end of each beep.
            if (i == 0 || i == UI_SFX_ALARM_BEEP - 1 || i == UI_SFX_ALARM_BEEP + UI_SFX_ALARM_GAP) assert(!sample);
            assert(abs(sample - previous) < 30000);
            if (abs(sample) > peak) peak = abs(sample);
            sum += sample; previous = sample; energy += (double)sample * sample;
        }
        // Near full scale but below clipping; far louder than a 48 ms click.
        assert(peak >= 24000 && peak <= 28000 && labs(sum) < 8000);
        assert(energy > 1.0e12);
    }
    ui_sfx_set_enabled(false);
    size_t alarm_before = written_bytes;
    assert(!ui_sfx_alarm_tick());
    ui_sfx_set_enabled(false); // Changing the click setting cannot cancel it.
    ui_sfx_play(UI_SFX_NAV); assert(s_pending == UI_SFX_ALARM);
    assert(!ui_sfx_alarm_tick()); finish();
    assert(written_bytes - alarm_before == 48000 + 256);
    assert(alert_levels == 1);
    assert(ui_sfx_alarm_tick() && !s_running);
    // Failure, cancellation and app handoff remain retryable, without falsely
    // reporting delivery; master mute does not silence the reminder.
    create_failure = true; assert(!ui_sfx_alarm_tick() && !s_running); create_failure = false;
    init_result = ESP_FAIL; assert(!ui_sfx_alarm_tick()); finish();
    assert(!atomic_load(&s_alarm_done)); init_result = 0;
    write_result = ESP_FAIL; assert(!ui_sfx_alarm_tick()); finish();
    assert(!atomic_load(&s_alarm_done)); write_result = 0;
    assert(!ui_sfx_alarm_tick()); stop_on_write = true; alarm_before = written_bytes; finish();
    assert(written_bytes - alarm_before == 512 && !atomic_load(&s_alarm_done));
    assert(!ui_sfx_alarm_tick()); ui_sfx_cancel_alarm(); finish(); assert(!atomic_load(&s_alarm_done));
    assert(!ui_sfx_alarm_tick()); finish(); assert(ui_sfx_alarm_tick());
    volume = 0; alarm_before = written_bytes; assert(!ui_sfx_alarm_tick()); finish();
    assert(written_bytes - alarm_before == 48000 + 256 && ui_sfx_alarm_tick()); volume = 80;
    ui_sfx_set_enabled(true);
    assert(creates == deletes && s_lock == 0 && ui_sfx_prepare_stop());
    puts("UI feedback: clean harmonic PCM, smooth envelope, mute, burst coalescing, expired requests, ownership refusal, IO failures, cancellation and 30 sessions PASS");
}
