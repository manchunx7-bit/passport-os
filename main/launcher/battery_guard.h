// Low-battery policy, kept free of hardware calls so the host tests cover it.
// Fed every gauge poll (10 s): soc in %, mv in millivolts, -1 when unreadable.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool warned;
    bool critical_warned;
    uint8_t critical_polls;
} battery_guard_t;

typedef enum { BATTERY_OK, BATTERY_WARN, BATTERY_CRITICAL, BATTERY_SHUTDOWN } battery_action_t;

#define BATTERY_WARN_SOC 10
// Last warning before the automatic shutdown at BATTERY_EMPTY_SOC.
#define BATTERY_CRITICAL_SOC 5
#define BATTERY_REARM_SOC 15
#define BATTERY_EMPTY_SOC 3
#define BATTERY_EMPTY_MV 3450
// The gauge lags while charging: an "empty" SOC with a charger's voltage is
// not empty. Below this voltage a 3% reading is trusted.
#define BATTERY_CHARGING_MV 3600
// Backlight ceiling while the battery is low. Saved preferences stay as set.
#define BATTERY_LOW_BRIGHTNESS 60

// Brightness to apply for a saved preference: every path that sets the
// backlight (settings, voice, outdoor restore) must respect the low-battery cap.
static inline uint8_t battery_brightness(int soc, uint8_t percent) {
    return soc >= 0 && soc <= BATTERY_WARN_SOC && percent > BATTERY_LOW_BRIGHTNESS ? BATTERY_LOW_BRIGHTNESS : percent;
}

static inline battery_action_t battery_guard_step(battery_guard_t *g, int soc, int mv) {
    const bool low_voltage = mv > 0 && mv < BATTERY_EMPTY_MV;
    const bool empty_soc = soc >= 0 && soc <= BATTERY_EMPTY_SOC && (mv <= 0 || mv < BATTERY_CHARGING_MV);
    // Two polls in a row: a Wi-Fi or speaker burst can sag one reading.
    if (low_voltage || empty_soc) {
        if (g->critical_polls < 2) ++g->critical_polls;
    } else {
        g->critical_polls = 0;
    }
    if (g->critical_polls >= 2) return BATTERY_SHUTDOWN;
    if (soc >= BATTERY_REARM_SOC) g->warned = g->critical_warned = false;
    // A first reading already at 5 % gets the final warning, not both.
    if (!g->critical_warned && soc >= 0 && soc <= BATTERY_CRITICAL_SOC) {
        g->warned = g->critical_warned = true;
        return BATTERY_CRITICAL;
    }
    if (!g->warned && soc >= 0 && soc <= BATTERY_WARN_SOC) {
        g->warned = true;
        return BATTERY_WARN;
    }
    return BATTERY_OK;
}
