#include <assert.h>
#include <stdio.h>
#include "launcher/battery_guard.h"

int main(void) {
    battery_guard_t g = {0};
    assert(battery_guard_step(&g, 50, 3900) == BATTERY_OK);
    // One warning per discharge, re-armed only after a real charge.
    assert(battery_guard_step(&g, 10, 3700) == BATTERY_WARN);
    assert(battery_guard_step(&g, 9, 3690) == BATTERY_OK);
    assert(battery_guard_step(&g, 12, 3720) == BATTERY_OK);
    assert(battery_guard_step(&g, 15, 3750) == BATTERY_OK);
    assert(battery_guard_step(&g, 8, 3680) == BATTERY_WARN);
    // A second, final warning at 5 %, once per discharge, re-armed by charge.
    assert(battery_guard_step(&g, 6, 3660) == BATTERY_OK);
    assert(battery_guard_step(&g, 5, 3650) == BATTERY_CRITICAL);
    assert(battery_guard_step(&g, 4, 3640) == BATTERY_OK);
    assert(battery_guard_step(&g, 16, 3800) == BATTERY_OK);
    assert(battery_guard_step(&g, 5, 3650) == BATTERY_CRITICAL);
    assert(battery_guard_step(&g, 9, 3690) == BATTERY_OK);
    // An unreadable gauge never warns and never shuts down.
    battery_guard_t u = {0};
    for (int i = 0; i < 5; ++i) assert(battery_guard_step(&u, -1, -1) == BATTERY_OK);
    // Empty needs two polls in a row; one sag under load is ignored.
    battery_guard_t e = {0};
    assert(battery_guard_step(&e, 20, 3400) == BATTERY_OK);
    assert(battery_guard_step(&e, 20, 3600) == BATTERY_OK);
    assert(battery_guard_step(&e, 20, 3400) == BATTERY_OK);
    assert(battery_guard_step(&e, 20, 3440) == BATTERY_SHUTDOWN);
    battery_guard_t s = {0};
    assert(battery_guard_step(&s, 3, 3520) == BATTERY_CRITICAL);
    assert(battery_guard_step(&s, 3, 3510) == BATTERY_SHUTDOWN);
    // 3 % on a charger (voltage already up) is not empty.
    battery_guard_t c = {0};
    for (int i = 0; i < 5; ++i) assert(battery_guard_step(&c, 2, 3950) != BATTERY_SHUTDOWN);
    // Gauge SOC alone (voltage unreadable) is still trusted.
    battery_guard_t v = {0};
    battery_guard_step(&v, 1, -1);
    assert(battery_guard_step(&v, 1, -1) == BATTERY_SHUTDOWN);
    // Every backlight path caps at the low-battery ceiling, never raises a dim choice.
    assert(battery_brightness(5, 90) == BATTERY_LOW_BRIGHTNESS && battery_brightness(10, 100) == BATTERY_LOW_BRIGHTNESS);
    assert(battery_brightness(5, 30) == 30 && battery_brightness(11, 90) == 90 && battery_brightness(-1, 90) == 90);
    puts("Battery guard: warn at 10 % and 5 %, re-arm on charge, two-poll empty, charger exemption, brightness cap PASS");
    return 0;
}
