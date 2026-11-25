#pragma once
// Shared top-bar Wi-Fi signal for tool pages: three bars by station RSSI,
// dimmed when offline. Same thresholds as the settings and stopwatch bars.
// Include after "ui_pixel.h" (host tests substitute their own ui_pixel mock).
#include "wifi_manager.h"

inline void ui_status_wifi_refresh(lv_obj_t *bars[3]) {
    if (!bars[0]) return;
    auto &wifi = WifiManager::GetInstance();
    int level = 0;
    uint32_t color = UI_TEXT_DIM;
    if (wifi.IsConnected()) {
        const int rssi = wifi.GetRssi();
        level = rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1;
        color = UI_TEXT;
    }
    ui_pixel_wifi_bars_set(bars, level, color);
}
