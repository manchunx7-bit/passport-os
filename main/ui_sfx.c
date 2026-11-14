#include "ui_sfx.h"
#include "bsp_audio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static atomic_bool s_enabled = true;
static atomic_bool s_stop;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_running;
static int s_pending = -1;
static int64_t s_requested_at;
static atomic_uint s_requests, s_played, s_expired, s_failures;
static bool s_alarm_active;
static atomic_bool s_alarm_done;

bool ui_sfx_enabled(void) { return atomic_load(&s_enabled); }

void ui_sfx_log_diagnostics(void) {
    ESP_LOGI("ui_sfx", "Feedback: requests=%u played=%u expired=%u failures=%u",
        atomic_load(&s_requests), atomic_load(&s_played),
        atomic_load(&s_expired), atomic_load(&s_failures));
}

bool ui_sfx_prepare_stop(void) {
    taskENTER_CRITICAL(&s_lock);
    atomic_store(&s_stop, true);
    s_pending = -1;
    if (s_task) xTaskNotifyGive(s_task);
    const bool ready = !s_running;
    taskEXIT_CRITICAL(&s_lock);
    return ready;
}

void ui_sfx_set_enabled(bool enabled) {
    atomic_store(&s_enabled, enabled);
    taskENTER_CRITICAL(&s_lock);
    if (!enabled && !s_alarm_active) {
        atomic_store(&s_stop, true);
        s_pending = -1;
        if (s_task) xTaskNotifyGive(s_task);
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void feedback_task(void *arg) {
    (void)arg;
    int16_t pcm[128];
    for (;;) {
        bool owned = false, idle_exit = false, first = true;
        if (atomic_load(&s_stop)) goto done;
        // Master mute silences clicks only; alarms use a fixed alert level.
        if (!bsp_audio_get_master_volume()) {
            taskENTER_CRITICAL(&s_lock);
            const bool alarm = s_pending == UI_SFX_ALARM;
            taskEXIT_CRITICAL(&s_lock);
            if (!alarm) goto done;
        }
        taskENTER_CRITICAL(&s_lock);
        const int first_kind = s_pending;
        const int64_t first_requested = s_requested_at;
        const bool fresh = first_kind == UI_SFX_ALARM || (first_kind >= 0 &&
            ui_sfx_enabled() && esp_timer_get_time() - first_requested < 180000);
        if (!fresh && s_pending >= 0) atomic_fetch_add(&s_expired, 1);
        s_pending = -1;
        taskEXIT_CRITICAL(&s_lock);
        if (!fresh) { idle_exit = true; goto done; }
        // Judge freshness before codec setup: cold initialization must not
        // expire a click that was ready to play when this worker accepted it.
        if (bsp_audio_init_feedback() != ESP_OK) goto failed;
        owned = true;
        if (first_kind == UI_SFX_ALARM) bsp_audio_set_alert_volume(UI_SFX_ALARM_VOLUME);
        else bsp_audio_set_volume(85);
        if (bsp_audio_set_format(UI_SFX_RATE, 16, 1) != ESP_OK) goto failed;
        while (!atomic_load(&s_stop) && (first_kind == UI_SFX_ALARM || bsp_audio_get_master_volume())) {
            int kind = first_kind;
            int64_t requested = first_requested;
            if (!first) {
                taskENTER_CRITICAL(&s_lock);
                kind = s_pending;
                requested = s_requested_at;
                s_pending = -1;
                taskEXIT_CRITICAL(&s_lock);
            }
            if (kind >= 0 && (kind == UI_SFX_ALARM || ui_sfx_enabled()) &&
                (first || esp_timer_get_time() - requested < 180000)) {
                const unsigned count = ui_sfx_samples((ui_sfx_kind_t)kind);
                bool complete = true;
                for (unsigned offset = 0; offset < count; offset += 128) {
                    // Clicks finish their envelope; a longer alarm yields at
                    // the next 8 ms chunk when an audio app needs the codec.
                    if (kind == UI_SFX_ALARM ? atomic_load(&s_stop) : !bsp_audio_get_master_volume()) {
                        complete = false; break;
                    }
                    const unsigned n = count - offset < 128 ? count - offset : 128;
                    for (unsigned i = 0; i < n; ++i) pcm[i] = ui_sfx_sample((ui_sfx_kind_t)kind, offset + i);
                    if (bsp_audio_write(pcm, n * sizeof(int16_t)) != ESP_OK) goto failed;
                }
                memset(pcm, 0, sizeof(pcm));
                if (bsp_audio_write(pcm, sizeof(pcm)) != ESP_OK) goto failed;
                if (complete) atomic_fetch_add(&s_played, 1);
                if (kind == UI_SFX_ALARM) {
                    taskENTER_CRITICAL(&s_lock);
                    if (complete && !atomic_load(&s_stop)) atomic_store(&s_alarm_done, true);
                    taskEXIT_CRITICAL(&s_lock);
                }
            } else if (kind >= 0) {
                atomic_fetch_add(&s_expired, 1);
            }
            first = false;
            if (kind == UI_SFX_ALARM) break;
            if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(700))) { idle_exit = true; break; }
        }
        goto done;
failed:
        atomic_fetch_add(&s_failures, 1);
done:
        if (owned) {
            // The compact DMA ring holds at most 40 ms. Drain before shutdown.
            vTaskDelay(pdMS_TO_TICKS(50));
            bsp_audio_deinit();
        }
        taskENTER_CRITICAL(&s_lock);
        // Retain clicks accepted during idle drain/teardown. The same worker
        // reopens the codec; app handoff, mute and failures still stop it.
        const bool retry = idle_exit && !atomic_load(&s_stop) && ui_sfx_enabled() &&
            bsp_audio_get_master_volume() && s_pending >= 0;
        if (!retry) {
            s_task = NULL;
            s_running = false;
            s_pending = -1;
            s_alarm_active = false;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (!retry) break;
    }
    vTaskDelete(NULL);
}

static void request_sound(ui_sfx_kind_t kind) {
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    if ((s_running && atomic_load(&s_stop)) || s_alarm_active ||
        (kind == UI_SFX_ALARM && s_running)) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    // The button driver and registry already emit one event per valid click.
    atomic_fetch_add(&s_requests, 1);
    s_requested_at = now;
    s_pending = kind;
    if (kind == UI_SFX_ALARM) s_alarm_active = true;
    const bool create = !s_running;
    if (create) {
        s_running = true;
        atomic_store(&s_stop, false);
    } else if (s_task) {
        xTaskNotifyGive(s_task);
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!create) return;
    // The registry lifecycle lock serializes creation with app switches.
    TaskHandle_t task = NULL;
    // Feed short audio above Home's HTTPS worker and LVGL (priority 4).
    // Writes/waits block, so button polling (6) and idle still run normally.
    if (xTaskCreate(feedback_task, "ui_sfx", 4096, NULL, 5, &task) != pdPASS) {
        atomic_fetch_add(&s_failures, 1);
        taskENTER_CRITICAL(&s_lock);
        s_running = false;
        s_pending = -1;
        s_alarm_active = false;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW("ui_sfx", "No memory for feedback; skipped");
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    if (s_running) s_task = task;
    taskEXIT_CRITICAL(&s_lock);
}

void ui_sfx_play(ui_sfx_kind_t kind) {
    if (kind < UI_SFX_NAV || kind > UI_SFX_BACK || !ui_sfx_enabled() || !bsp_audio_get_master_volume()) return;
    request_sound(kind);
}

bool ui_sfx_alarm_tick(void) {
    if (atomic_exchange(&s_alarm_done, false)) return true;
    request_sound(UI_SFX_ALARM);
    return false;
}

void ui_sfx_cancel_alarm(void) {
    taskENTER_CRITICAL(&s_lock);
    atomic_store(&s_alarm_done, false);
    if (s_alarm_active) {
        atomic_store(&s_stop, true);
        s_pending = -1;
        if (s_task) xTaskNotifyGive(s_task);
    }
    taskEXIT_CRITICAL(&s_lock);
}
