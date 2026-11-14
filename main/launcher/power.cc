#include "launcher/power.h"

#include "launcher/app_registry.h"
#include "launcher/battery_guard.h"
#include "launcher/system_overlay.h"
#include "apps/reminders/reminder_service.h"
#include "ui_sfx.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>

namespace {

constexpr char kTag[] = "power";
constexpr gpio_num_t kWakePin = GPIO_NUM_0;  // shared button ADC node
constexpr uint16_t kButtonRanges[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;
bool s_hold_hint;
// Battery warning beep; retried each 10 s gauge poll while an audio app or
// switch blocks it, then dropped so it cannot hold the shared alarm slot.
uint8_t s_beep_polls;

void wake_pin_input() {
    const gpio_config_t pin = {
        .pin_bit_mask = 1ULL << kWakePin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,  // external 10k pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pin);
}

[[noreturn]] void sleep_now() {
    // LEDC stops in deep sleep and the pin would float; a floating backlight
    // pin lights the LED. Hold it low; app_main releases the hold at boot.
    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BSP_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);
    gpio_set_level((gpio_num_t)BSP_LCD_BL, 0);
    gpio_hold_en((gpio_num_t)BSP_LCD_BL);
    gpio_deep_sleep_hold_en();
    wake_pin_input();
    esp_deep_sleep_enable_gpio_wakeup(1ULL << kWakePin, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

void show_notice(const char *text, uint32_t ms) {
    if (bsp_lvgl_lock(500)) {
        app_registry_wake_for_notice();
        bsp_lvgl_unlock();
    }
    system_overlay_show(text, ms);
}

}  // namespace

void power_boot_check(void) {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) return;
    wake_pin_input();
    // Boot already took ~0.3 s of the hold; ask for another 0.8 s.
    for (int i = 0; i < 40; ++i) {
        if (gpio_get_level(kWakePin)) {
            ESP_LOGI(kTag, "Wake key released early; back to sleep");
            sleep_now();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(kTag, "Powered on by UP key");
}

void power_off(const char *message) {
    ESP_LOGW(kTag, "Power off: %s", message ? message : "(silent)");
    bsp_audio_set_master_volume(0);
    // Alarms ignore master volume; stop any ringing before shutting down.
    reminders::dismiss_for_navigation();
    ui_sfx_cancel_alarm();
    if (message) {
        show_notice(message, 0);
        vTaskDelay(pdMS_TO_TICKS(1800));
    }
    esp_wifi_stop();
    // Keep the LVGL lock: nothing may flush to the panel after SLPIN.
    bsp_lvgl_lock(1000);
    bsp_display_backlight(0);
    bsp_display_power(false);
    // Wait for the shared button node to return high before arming wakeup.
    for (int released = 0, waited = 0; released < 5 && waited < 500; ++waited) {
        released = bsp_button_read_mv() < kButtonRanges[BSP_BTN_COUNT - 1][1] ? 0 : released + 1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bsp_audio_prepare_sleep();
    sleep_now();
}

bool power_battery_tick(void) {
    static battery_guard_t guard;
    if (s_beep_polls) {
        // Done (or refused) on a later poll after the request; give up after 3.
        if (app_registry_sound_notice()) s_beep_polls = 0;
        else if (!--s_beep_polls && !reminders::alarm_active()) ui_sfx_cancel_alarm();
    }
    const int soc = bsp_battery_soc();
    switch (battery_guard_step(&guard, soc, bsp_battery_mv())) {
        case BATTERY_WARN:
        case BATTERY_CRITICAL: {
            const bool critical = soc <= BATTERY_CRITICAL_SOC;
            ESP_LOGW(kTag, "Low battery: %d%%", soc);
            // Session-only dimming; the saved brightness returns after a charge and reboot.
            bsp_display_set_master_brightness(battery_brightness(soc, bsp_display_get_master_brightness()));
            show_notice(critical ? "电量低于 5%\n即将自动关机，请立即充电"
                                 : "电量低于 10%\n已调暗屏幕，请尽快充电", 4000);
            // Request now; later polls collect completion (or retry a stale one).
            s_beep_polls = 3;
            app_registry_sound_notice();
            return false;
        }
        case BATTERY_SHUTDOWN:
            ESP_LOGW(kTag, "Battery empty: %d%% %dmV", soc, bsp_battery_mv());
            return true;
        default:
            return false;
    }
}

void power_hold(uint8_t stage) {
    switch (stage) {
        case 0:
            if (s_hold_hint) system_overlay_hide();
            s_hold_hint = false;
            break;
        case 1:
            s_hold_hint = true;
            show_notice("继续按住关机\n松开取消", 0);
            break;
        case 2:
            power_off("已关机\n按住上键 1 秒开机");
        default:
            power_off("电量耗尽，已关机\n充电后按住上键开机");
    }
}
