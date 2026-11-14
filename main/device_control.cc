#include "device_control.h"
#include "launcher/app_registry.h"
#include "launcher/settings.h"
#include "bsp_display.h"
#include "apps/reminders/reminder_service.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <algorithm>

namespace device_control {
namespace {
enum Phase { Idle, Writing, Waiting, Running, Done, Abandoned };
std::atomic<int> phase{Idle};
Request request;
Result result;
// All fields below are input-task-owned except the acknowledgement mailbox.
passport_app_id_t open_target = APP_ID_COUNT;
uint64_t open_token = 0;
int64_t open_expires = 0, open_at = 0;
bool open_handover = false;  // the pending open carries an armed media request
// ESP32-C3 has native 32-bit atomics. Split token publication uses a mailbox
// phase, not a 64-bit atomic that may need a non-lock-free runtime helper.
std::atomic<int> ack_phase{0};
uint64_t ack_token = 0;
uint64_t waiting_response_token = 0; // controller-task-owned
std::atomic<bool> cancel{false};
}

Result execute(const Request &value) {
    int expected = Idle;
    if (!phase.compare_exchange_strong(expected, Writing)) return {Error::Busy};
    if (value.action == Action::Open || value.action == Action::Handover) waiting_response_token = value.token;
    request = value;
    phase.store(Waiting, std::memory_order_release);
    const int64_t deadline = esp_timer_get_time() + 2500000;
    while (esp_timer_get_time() < deadline) {
        if (phase.load(std::memory_order_acquire) == Done) {
            const Result completed = result;
            phase.store(Idle, std::memory_order_release);
            return completed;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Cancel before execution. A flash operation already running cannot be
    // undone; report an unknown outcome and let the input task release it.
    expected = Waiting;
    if (phase.compare_exchange_strong(expected, Idle)) return {Error::Timeout};
    expected = Running;
    if (phase.compare_exchange_strong(expected, Abandoned)) return {Error::Timeout};
    if (phase.load(std::memory_order_acquire) == Done) {
        const Result completed = result;
        phase.store(Idle, std::memory_order_release);
        return completed;
    }
    return {Error::Timeout};
}

void response_sent(uint64_t token, bool sent) {
    if (!sent || !token || token != waiting_response_token) return;
    int expected = 0;
    if (!ack_phase.compare_exchange_strong(expected, 1)) return;
    ack_token = token;
    ack_phase.store(2, std::memory_order_release);
}

void cancel_session() { waiting_response_token=0; cancel.store(true, std::memory_order_release); }

bool pending() {
    return phase.load(std::memory_order_acquire) != Idle ||
           ack_phase.load(std::memory_order_acquire) != 0 ||
           open_target != APP_ID_COUNT;
}

namespace {
// A dropped open must not leave playback to start on a later manual visit.
void drop_open() {
    if (open_handover) voice_intent::clear();
    open_target = APP_ID_COUNT;
    open_token = 0;
    open_handover = false;
}

// Voice navigation remains visible even if the idle timer blanked the panel
// during conversation. On UI contention the next poll retries, and the tool
// request handled after it in the same poll is not delayed.
bool wake_display() {
    if (!bsp_lvgl_lock(20)) return false;
    const bool visible = app_registry_wake_for_notice();
    bsp_lvgl_unlock();
    return visible;
}
}

void poll() {
    const int64_t now = esp_timer_get_time();
    if (cancel.exchange(false)) drop_open();
    if (ack_phase.load(std::memory_order_acquire) == 2) {
        if (open_token && ack_token == open_token) open_at = now + 50000;
        ack_phase.store(0, std::memory_order_release);
    }
    if (open_target != APP_ID_COUNT) {
        if (now > open_expires || app_registry_get_active() != APP_ID_XIAOZHI || app_registry_switch_pending() || reminders::alarm_active()) {
            drop_open();
        } else if (open_at && now >= open_at && wake_display()) {
            const auto target = open_target;
            open_target = APP_ID_COUNT;
            open_token = 0;
            open_handover = false;  // the armed request now belongs to the switch
            ESP_LOGI("device_control", "Opening app=%d after tool response", int(target));
            app_registry_switch_to(target);
        }
    }
    int expected = Waiting;
    if (!phase.compare_exchange_strong(expected, Running)) return;
    const Request command = request;
    Result completed;
    if (app_registry_get_active() != APP_ID_XIAOZHI || app_registry_switch_pending()) completed.error = Error::Inactive;
    else if (command.action == Action::Volume || command.action == Action::Brightness) {
        const bool volume = command.action == Action::Volume;
        const int current = volume ? settings_get_volume() : settings_get_brightness();
        const int requested = command.relative ? current + command.value : command.value;
        const auto percent = static_cast<uint8_t>(std::clamp(requested, volume ? 0 : 10, 100));
        const bool saved = volume ? settings_save_volume(percent) : settings_save_brightness(percent);
        if (!saved) completed.error = Error::Storage;
    } else if (command.action == Action::Pomodoro) {
        pomodoro_command_result_t timer_result = POMODORO_COMMAND_OK;
        if (!pomodoro_control(command.timer_action, command.timer_phase, &completed.timer, &timer_result))
            completed.error = Error::Busy;
        else if (timer_result == POMODORO_COMMAND_CONFLICT) completed.error = Error::TimerConflict;
        else if (timer_result != POMODORO_COMMAND_OK) completed.error = Error::TimerInvalid;
    } else if (command.action == Action::Open || command.action == Action::Handover) {
        const bool handover = command.action == Action::Handover;
        const passport_app_id_t app = handover ? command.handover.app : command.app;
        if (reminders::alarm_active()) completed.error = Error::Alarm;
        else if (!command.token || (app != APP_ID_LAUNCHER && !app_registry_get_by_id(app)))
            completed.error = Error::UnknownApp;
        else if (app == APP_ID_XIAOZHI) completed.error = Error::AlreadyOpen;
        else {
            drop_open();
            if (handover) voice_intent::arm(command.handover);
            open_target = app;
            open_token = command.token;
            open_handover = handover;
            open_expires = now + 30000000;
            open_at = 0; // never switch before the matching response is sent
        }
    }
    completed.volume = settings_get_volume();
    completed.brightness = settings_get_brightness();
    completed.applied_brightness = bsp_display_get_master_brightness();
    completed.app = app_registry_get_active();
    result = completed;
    expected = Running;
    if (!phase.compare_exchange_strong(expected, Done, std::memory_order_release)) {
        // The caller timed out while flash was in progress. An abandoned open
        // request has no acknowledgement and must not navigate later.
        if (command.action == Action::Open || command.action == Action::Handover) drop_open();
        phase.store(Idle, std::memory_order_release);
    }
}
}
