#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5

// Keep DHCP's main resolver (including local names), and any supplied backups.
// A router can hand out its own address while its DNS forwarder is unavailable.
// Fill only empty backup slots so lwIP can retry without reconnecting Wi-Fi.
static void ensure_backup_dns(esp_netif_t *netif) {
    if (!netif) return;
    struct Backup { esp_netif_dns_type_t slot; const char *address; };
    const Backup backups[] = {{ESP_NETIF_DNS_BACKUP, "223.5.5.5"},
                              {ESP_NETIF_DNS_FALLBACK, "119.29.29.29"}};
    for (const auto &backup : backups) {
        esp_netif_dns_info_t info{};
        if (esp_netif_get_dns_info(netif, backup.slot, &info) != ESP_OK) continue;
        if (info.ip.type != ESP_IPADDR_TYPE_V4 || info.ip.u_addr.ip4.addr != 0) continue;
        if (esp_netif_str_to_ip4(backup.address, &info.ip.u_addr.ip4) != ESP_OK) continue;
        const esp_err_t result = esp_netif_set_dns_info(netif, backup.slot, &info);
        if (result == ESP_OK) ESP_LOGI(TAG, "Empty backup DNS slot %d set to %s", (int)backup.slot, backup.address);
        else ESP_LOGW(TAG, "Backup DNS setup failed: %s", esp_err_to_name(result));
    }
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Unregister event handlers FIRST to prevent scan done from triggering connect
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // Stop timer
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    // Now safe to stop scan, disconnect and stop WiFi (no event callbacks will fire)
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    // Reset was_connected_ flag to prevent stale state from affecting subsequent sessions
    was_connected_ = false;
    connect_queue_.clear();
    reconnect_count_ = 0;

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    
    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void(int reason)> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    
    // Create or reuse the default WiFi station interface
    station_netif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station_netif_ == nullptr) {
        station_netif_ = esp_netif_create_default_wifi_sta();
    }
    if (!hostname_.empty()) {
        esp_err_t err = esp_netif_set_hostname(station_netif_, hostname_.c_str());
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Station DHCP hostname: %s", hostname_.c_str());
        } else {
            ESP_LOGW(TAG, "Failed to set station DHCP hostname: %s", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Setup the timer to scan WiFi.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<WifiStation *>(arg)->StartScan();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
    // STA_START can arrive immediately; its scan/error path needs the timer.
    connect_queue_.clear();
    reconnect_count_ = 0;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
    ESP_ERROR_CHECK(esp_wifi_start());
    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::StartScan() {
    if (IsConnected()) return;
    const esp_err_t result = esp_wifi_scan_start(nullptr, false);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan deferred: %s", esp_err_to_name(result));
        ScheduleNextScan();
        return;
    }
    if (on_scan_begin_) on_scan_begin_();
}

void WifiStation::ScheduleNextScan() {
    if (!timer_handle_) return;
    esp_timer_stop(timer_handle_);
    int64_t delay = scan_current_interval_microseconds_;
    if (scan_idle_.load(std::memory_order_relaxed))
        delay = std::max<int64_t>(delay, kIdleScanIntervalSeconds * 1000LL * 1000LL);
    esp_timer_start_once(timer_handle_, delay);
    UpdateScanInterval();
}

void WifiStation::HandleScanResult() {
    // An unrelated/late scan must not interrupt a working audio connection.
    if (IsConnected()) {
        esp_wifi_clear_ap_list();
        return;
    }
    connect_queue_.clear();
    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) {
        esp_wifi_clear_ap_list();
        ScheduleNextScan();
        return;
    }
    const auto &saved_networks = SsidManager::GetInstance().GetSsidList();
    // Consume the driver's list one record at a time. Keep only the best AP for
    // each saved SSID (at most ten), even in a crowded location with many APs.
    for (uint16_t i = 0; i < ap_num; ++i) {
        wifi_ap_record_t ap_record = {};
        if (esp_wifi_scan_get_ap_record(&ap_record) != ESP_OK) break;
        for (const auto &saved : saved_networks) {
            if (network_allowed_ && !network_allowed_(saved.ssid)) continue;
            if (std::strncmp(reinterpret_cast<char *>(ap_record.ssid), saved.ssid.c_str(), sizeof(ap_record.ssid)) != 0) continue;
            auto existing = std::find_if(connect_queue_.begin(), connect_queue_.end(),
                [&saved](const WifiApRecord &candidate) { return candidate.ssid == saved.ssid; });
            if (existing != connect_queue_.end() && existing->rssi >= ap_record.rssi) break;
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = saved.ssid,
                .password = saved.password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0},
                .rssi = ap_record.rssi
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            if (existing == connect_queue_.end()) connect_queue_.push_back(std::move(record));
            else *existing = std::move(record);
            break;
        }
    }
    esp_wifi_clear_ap_list();
    std::stable_sort(connect_queue_.begin(), connect_queue_.end(),
        [](const WifiApRecord &a, const WifiApRecord &b) { return a.rssi > b.rssi; });

    // Hidden saved SSIDs cannot be ranked by RSSI. Try them after visible APs,
    // without pinning an unknown channel/BSSID. The driver scans by SSID.
    for (const auto &saved : saved_networks) {
        if (network_allowed_ && !network_allowed_(saved.ssid)) continue;
        if (std::none_of(connect_queue_.begin(), connect_queue_.end(),
            [&saved](const WifiApRecord &ap) { return ap.ssid == saved.ssid; }))
            connect_queue_.push_back({saved.ssid, saved.password, 0, WIFI_AUTH_OPEN, {0}, -127});
    }
    const auto preferred = preferred_ssid_ ? preferred_ssid_() : std::string{};
    const auto first = std::find_if(connect_queue_.begin(), connect_queue_.end(),
        [&preferred](const WifiApRecord &ap) { return ap.channel != 0 && ap.ssid == preferred; });
    if (first != connect_queue_.end()) std::rotate(connect_queue_.begin(), first, first + 1);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        ScheduleNextScan();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    if (connect_queue_.empty()) {
        ScheduleNextScan();
        return;
    }
    if (timer_handle_) esp_timer_stop(timer_handle_);
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;
    ESP_LOGI(TAG, "Auto-select saved WiFi: %s, RSSI=%d", ssid_.c_str(), ap_record.rssi);

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());

    if (remember_bssid_ && ap_record.channel != 0) {
        // Explicit opt-in: pin to this exact AP (BSSID + channel) for the fastest
        // reconnect. This intentionally disables roaming between same-SSID APs.
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    } else {
        // Default: do not lock a BSSID. Ask the driver to scan every channel and
        // connect to the same-SSID AP with the strongest signal, instead of the
        // first match found by the default fast scan (which may be a weaker
        // same-name hotspot). Because no BSSID is pinned, a later reconnect (e.g.
        // after moving to another room) will scan again and roam to whichever AP
        // is strongest at that time.
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        // Retry the strongest AP this many times before falling back to a weaker
        // same-SSID AP. Requires WIFI_ALL_CHANNEL_SCAN (set above). Without this
        // the driver gives up after a single auth failure and immediately moves on,
        // which is why we saw it connect to a -79 dBm AP even though a -73 dBm one
        // was available (the stronger AP rejected the first attempt).
        wifi_config.sta.failure_retry_cnt = failure_retry_cnt_;
    }
    wifi_config.sta.listen_interval = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}


void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;  // Maximum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER (MAX_MODEM)");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    // station 可能处于启动/停止过渡态,set_ps 会报错;记日志即可,期望档位由
    // WifiManager 记忆并在 StartStation 后重放,不允许这里 abort 拖死整机。
    esp_err_t ps_err = esp_wifi_set_ps(ps_type);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(%d) failed: %s", (int)ps_type, esp_err_to_name(ps_err));
    }
}

void WifiStation::ScanNow() {
    if (!timer_handle_ || IsConnected()) return;
    const int64_t now = esp_timer_get_time();
    if (last_scan_now_us_ != 0 && now - last_scan_now_us_ < 15 * 1000 * 1000) return;
    last_scan_now_us_ = now;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
    // Timer not armed: a scan or connect is in flight and will reschedule
    // itself from the minimum interval set above.
    if (!esp_timer_is_active(timer_handle_)) return;
    ESP_LOGI(TAG, "User activity: scanning now");
    esp_timer_stop(timer_handle_);
    esp_timer_start_once(timer_handle_, 100 * 1000);
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        this_->StartScan();
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        
        // Notify disconnected callback only once when transitioning from connected to disconnected
        bool was_connected = this_->was_connected_;
        this_->was_connected_ = false;
        wifi_event_sta_disconnected_t* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", event->reason);
        if (was_connected && this_->on_disconnected_) {
            this_->on_disconnected_(event->reason);
        }
        if (was_connected) {
            // Re-rank all saved networks after losing a working connection,
            // rather than retrying the old SSID five times with a stale list.
            this_->connect_queue_.clear();
            this_->reconnect_count_ = 0;
            this_->StartScan();
            return;
        }
        
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, next scan in %d seconds", 
                 this_->scan_current_interval_microseconds_ / 1000 / 1000);
        this_->ScheduleNextScan();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());

    ensure_backup_dns(this_->station_netif_);
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    if (this_->timer_handle_) esp_timer_stop(this_->timer_handle_);
    this_->was_connected_ = true;  // Mark as connected for disconnect notification
    SsidManager::GetInstance().MarkUsed(this_->ssid_);
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    
    // Reset scan interval to minimum for fast reconnect if disconnected later
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;
}
