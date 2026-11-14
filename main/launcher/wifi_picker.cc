#include "launcher/wifi_picker.h"
#include "apps/radio/radio_ui.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_display.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {
enum class Page { List, Keyboard, Connecting, Saved, Delete, Success };
struct Picker {
    Page page = Page::List;
    WifiKeyboardPage keyboard = WifiKeyboardPage::Lower;
    std::vector<WifiNetworkView> networks;
    std::string ssid, password;
    size_t selected = 0, saved_index = 0;
    bool secure = false;
    const char *error = nullptr;
    std::atomic<int> result{0}; // 1: worker owns credentials; 2/3: completed
    lv_timer_t *timer = nullptr;
};
Picker *s_picker;

void draw() {
    auto &p = *s_picker;
    switch (p.page) {
        case Page::List:
            radio_ui_show_wifi_list(p.networks.data(), p.networks.size(), p.selected, p.networks.size() == 2, nullptr, true); break;
        case Page::Keyboard:
            radio_ui_show_wifi_keyboard(p.ssid.c_str(), p.password.size(), p.keyboard, p.selected, p.error); break;
        case Page::Connecting:
            radio_ui_show_wifi_connecting(p.ssid.c_str(), "正在连接，请稍等"); break;
        case Page::Success:
            radio_ui_show_wifi_success(p.ssid.c_str()); break;
        case Page::Saved: {
            const auto &saved = SsidManager::GetInstance().GetSsidList();
            const char *names[10];
            const size_t count = std::min<size_t>(saved.size(), 10);
            for (size_t i = 0; i < count; ++i) names[i] = saved[i].ssid.c_str();
            radio_ui_show_saved_wifi(names, count, p.selected); break;
        }
        case Page::Delete: radio_ui_show_wifi_delete_confirm(p.ssid.c_str(), p.selected); break;
    }
}

void refresh_scan() {
    auto &p = *s_picker;
    auto aps = WifiManager::GetInstance().GetConfigAccessPoints();
    std::sort(aps.begin(), aps.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });
    const size_t old_count = p.networks.size();
    const bool saved_selected = p.selected == 0;
    const bool return_selected = old_count > 1 && p.selected == old_count - 1;
    const std::string selected = !saved_selected && !return_selected && p.selected < old_count
                                     ? p.networks[p.selected].ssid : "";
    p.networks.clear();
    WifiNetworkView saved{};
    saved.rssi = -128;
    std::snprintf(saved.ssid, sizeof(saved.ssid), "%s", "已存网络");
    p.networks.push_back(saved);
    for (const auto &ap : aps) {
        if (!ap.ssid[0]) continue;
        if (std::any_of(p.networks.begin() + 1, p.networks.end(), [&](const auto &n) {
            return std::strncmp(n.ssid, reinterpret_cast<const char *>(ap.ssid), 32) == 0;
        })) continue;
        WifiNetworkView item{};
        std::memcpy(item.ssid, ap.ssid, 32); item.rssi = ap.rssi; item.secure = ap.authmode != WIFI_AUTH_OPEN;
        p.networks.push_back(item);
    }
    WifiNetworkView back{};
    back.rssi = -128;
    std::snprintf(back.ssid, sizeof(back.ssid), "%s", "返回");
    p.networks.push_back(back);
    p.selected = return_selected ? p.networks.size() - 1 : 0;
    if (!saved_selected && !return_selected) {
        for (size_t i = 1; i + 1 < p.networks.size(); ++i) {
            if (selected == p.networks[i].ssid) { p.selected = i; break; }
        }
    }
    draw();
}

void connect() {
    auto &p = *s_picker;
    if (p.secure && p.password.size() < 8) { p.error = "密码至少 8 位"; draw(); return; }
    p.page = Page::Connecting; p.result = 1; draw();
    if (xTaskCreate([](void *arg) {
        auto *p = static_cast<Picker *>(arg);
        const bool ok = WifiManager::GetInstance().ConfigureNetwork(p->ssid, p->password, true);
        p->result.store(ok ? 2 : 3); // last access before the UI may release p
        vTaskDelete(nullptr);
    }, "wifi_picker", 4096, &p, 3, nullptr) != pdPASS) p.result = 3;
}
}

bool wifi_picker_open() {
    if (s_picker) return true;
    s_picker = new (std::nothrow) Picker;
    if (!s_picker) return false;
    WifiManager::GetInstance().StartConfigAp(true, true);
    if (!radio_ui_init()) { wifi_picker_close(); return false; }
    bsp_lvgl_lock(-1);
    refresh_scan();
    s_picker->timer = lv_timer_create([](lv_timer_t *) {
        auto &p = *s_picker;
        if (p.page == Page::List) refresh_scan();
        const int result = p.result.load();
        if (p.page != Page::Connecting || result < 2) return;
        if (result == 2) { p.page = Page::Success; std::fill(p.password.begin(), p.password.end(), '\0'); p.password.clear(); }
        else {
            const int reason = WifiManager::GetInstance().LastConfigDisconnectReason();
            p.error = reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT
                ? "密码错误，请修改" : reason == WIFI_REASON_NO_AP_FOUND ? "未找到网络，请重试" : "连接超时，请重试";
            p.page = Page::Keyboard; p.selected = 0;
        }
        draw();
    }, 1000, nullptr);
    bsp_lvgl_unlock();
    return true;
}

bool wifi_picker_active() { return s_picker != nullptr; }
bool wifi_picker_busy() { return s_picker && s_picker->result.load() == 1; }
void wifi_picker_close() {
    if (!s_picker || wifi_picker_busy()) return;
    bsp_lvgl_lock(-1);
    if (s_picker->timer) lv_timer_delete(s_picker->timer);
    radio_ui_deinit();
    std::fill(s_picker->password.begin(), s_picker->password.end(), '\0');
    delete s_picker; s_picker = nullptr;
    bsp_lvgl_unlock();
    auto &wifi = WifiManager::GetInstance();
    wifi.StopConfigAp(); wifi.StartStation();
}

void wifi_picker_key(uint8_t btn, app_btn_event_t event) {
    if (!s_picker || wifi_picker_busy()) return;
    bsp_lvgl_lock(-1);
    auto &p = *s_picker;
    if (btn == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        p.page = Page::List; p.selected = 0; p.error = nullptr; refresh_scan();
    } else if (event == BTN_EVT_CLICK) {
        size_t count = 1;
        if (p.page == Page::List) count = p.networks.size();
        if (p.page == Page::Keyboard) count = radio_ui_wifi_key_count(p.keyboard);
        if (p.page == Page::Saved) count = SsidManager::GetInstance().GetSsidList().size() + 1;
        if (p.page == Page::Delete) count = 2;
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) p.selected = (p.selected + count + (btn == BSP_BTN_UP ? -1 : 1)) % count;
        else if (btn == BSP_BTN_OK) {
            if (p.page == Page::List) {
                if (p.selected == count - 1) { bsp_lvgl_unlock(); wifi_picker_close(); return; }
                if (p.selected == 0) { p.page = Page::Saved; p.selected = 0; }
                else {
                    const auto n = p.networks[p.selected]; p.ssid = n.ssid; p.secure = n.secure;
                    std::fill(p.password.begin(), p.password.end(), '\0'); p.password.clear(); p.error = nullptr;
                    p.selected = 0; p.keyboard = WifiKeyboardPage::Lower;
                    p.page = Page::Keyboard; if (!p.secure) connect();
                }
            } else if (p.page == Page::Keyboard) {
                const char *key = radio_ui_wifi_key(p.keyboard, p.selected);
                if (std::strlen(key) == 1) { if (p.password.size() < 63) p.password += key; p.error = nullptr; }
                else if (!std::strcmp(key, "DEL")) { if (!p.password.empty()) p.password.pop_back(); }
                else if (!std::strcmp(key, "GO")) connect();
                else {
                    p.keyboard = !std::strcmp(key,"ABC") ? WifiKeyboardPage::Upper : !std::strcmp(key,"abc") ? WifiKeyboardPage::Lower : !std::strcmp(key,"123") ? WifiKeyboardPage::Symbols : WifiKeyboardPage::MoreSymbols;
                    p.selected = 0;
                }
            } else if (p.page == Page::Saved) {
                if (p.selected == count - 1) { p.page = Page::List; p.selected = 0; }
                else { p.saved_index = p.selected; p.ssid = SsidManager::GetInstance().GetSsidList()[p.selected].ssid; p.page = Page::Delete; p.selected = 0; }
            } else if (p.page == Page::Delete) {
                if (p.selected == 1) SsidManager::GetInstance().RemoveSsid(p.saved_index);
                p.page = Page::Saved; p.selected = 0;
            } else if (p.page == Page::Success) { bsp_lvgl_unlock(); wifi_picker_close(); return; }
        }
        draw();
    }
    bsp_lvgl_unlock();
}
