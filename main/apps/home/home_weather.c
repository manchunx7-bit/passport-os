#include "apps/home/home_weather.h"
#include "apps/home/home_profile.h"
#include "apps/voice/time_sync.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "home_weather";

#define NVS_NAMESPACE "home_profile"
#define RESPONSE_MAX 1024 /* forecast?current= 响应 < 500B,留余量 */
#define HTTP_TIMEOUT_MS 30000 /* 慢网络下 DNS+TCP+TLS 可能超过 15 秒;退出首页仍可立即取消 */
#define WEATHER_HOST_TIMEOUT_MS 15000 /* 两个天气主机各占一半,总时长仍为 30 秒 */

/* api.open-meteo.com 按地区解析到不同节点,国内部分节点长时间连不上;
 * historical-forecast-api 是另一组服务器,/v1/forecast 的 current 数据相同。
 * 记住上次成功的主机,下次先试它。只由首页工作任务访问。 */
static const char *const kWeatherHosts[] = {
    "api.open-meteo.com",
    "historical-forecast-api.open-meteo.com",
};
static unsigned s_weather_host;
#define EPOCH_VALID_MIN 1700000000LL /* 与 home_clock 一致:早于此视为未校时 */

static home_weather_t s_weather;
static uint32_t s_version;
static portMUX_TYPE s_weather_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_lat[HOME_COORD_MAX], s_lon[HOME_COORD_MAX];
static home_air_quality_t s_air;
static int64_t s_air_updated_us;
static char s_air_lat[HOME_COORD_MAX], s_air_lon[HOME_COORD_MAX];
static bool s_requests_cancelled;

void home_weather_allow_requests(void) {
    portENTER_CRITICAL(&s_weather_mux);
    s_requests_cancelled = false;
    portEXIT_CRITICAL(&s_weather_mux);
}

void home_weather_cancel_requests(void) {
    portENTER_CRITICAL(&s_weather_mux);
    s_requests_cancelled = true;
    portEXIT_CRITICAL(&s_weather_mux);
}

static bool requests_cancelled(void) {
    portENTER_CRITICAL(&s_weather_mux);
    bool cancelled = s_requests_cancelled;
    portEXIT_CRITICAL(&s_weather_mux);
    return cancelled;
}

void home_weather_init(void) {
    home_weather_allow_requests();
    memset(&s_weather, 0, sizeof(s_weather));
    s_lat[0] = s_lon[0] = '\0';
    memset(&s_air, 0, sizeof(s_air));
    s_air_updated_us = 0;
    s_air_lat[0] = s_air_lon[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        int16_t temp = 0;
        int16_t code = 0;
        size_t lat_len = sizeof(s_lat), lon_len = sizeof(s_lon);
        if (nvs_get_str(handle, "wlat", s_lat, &lat_len) == ESP_OK &&
            nvs_get_str(handle, "wlon", s_lon, &lon_len) == ESP_OK &&
            nvs_get_i16(handle, "wtemp", &temp) == ESP_OK &&
            nvs_get_i16(handle, "wcode", &code) == ESP_OK) {
            s_weather.valid = true;
            s_weather.temp_c = temp;
            s_weather.code = (uint16_t)code;
            uint8_t u8 = 0;
            if (nvs_get_u8(handle, "whum", &u8) == ESP_OK) s_weather.humidity = u8;
            if (nvs_get_u8(handle, "wwind", &u8) == ESP_OK) s_weather.wind_kmh = u8;
            uint16_t dir = 0;
            if (nvs_get_u16(handle, "wdir", &dir) == ESP_OK) s_weather.wind_dir = dir;
            uint32_t ts = 0;
            if (nvs_get_u32(handle, "wts", &ts) == ESP_OK) s_weather.updated_at = ts;
        }
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Weather cache: valid=%d temp=%d code=%u",
             (int)s_weather.valid, (int)s_weather.temp_c, (unsigned)s_weather.code);
}

bool home_weather_get(home_weather_t *out) {
    if (!out) return false;
    home_profile_t profile;
    home_profile_get(&profile);
    portENTER_CRITICAL(&s_weather_mux);
    *out = s_weather;
    out->valid = s_weather.valid && s_lat[0] && s_lon[0] &&
                 !strcmp(profile.lat, s_lat) && !strcmp(profile.lon, s_lon);
    portEXIT_CRITICAL(&s_weather_mux);
    return out->valid;
}

bool home_air_quality_get(home_air_quality_t *out) {
    if (!out) return false;
    home_profile_t profile;
    home_profile_get(&profile);
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_weather_mux);
    *out = s_air;
    out->valid = s_air.valid && s_air_lat[0] && s_air_lon[0] &&
                 now_us >= s_air_updated_us && now_us - s_air_updated_us < 1800000000LL &&
                 !strcmp(profile.lat, s_air_lat) && !strcmp(profile.lon, s_air_lon);
    portEXIT_CRITICAL(&s_weather_mux);
    return out->valid;
}

void home_air_quality_label(bool connected, char *out, size_t capacity) {
    if (!out || !capacity) return;
    home_air_quality_t air;
    if (!connected || !home_air_quality_get(&air)) {
        snprintf(out, capacity, "PM2.5 --");
    } else if (air.pm25_x10 >= 10000) {
        snprintf(out, capacity, "PM2.5 %u", (unsigned)((air.pm25_x10 + 5) / 10));
    } else {
        snprintf(out, capacity, "PM2.5 %u.%u", (unsigned)(air.pm25_x10 / 10),
                 (unsigned)(air.pm25_x10 % 10));
    }
}

uint32_t home_weather_version(void) {
    portENTER_CRITICAL(&s_weather_mux);
    uint32_t v = s_version;
    portEXIT_CRITICAL(&s_weather_mux);
    return v;
}

const char *home_weather_code_text(uint16_t code) {
    // 短语只由 buddy 字体已有的字组成(雨雪雾雷冰雹晴阴多云大小中等均已验证)
    if (code == 0 || code == 1) return "晴";
    if (code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 55) return "毛毛雨";
    if (code == 56 || code == 57) return "冻雨";
    if (code == 61) return "小雨";
    if (code == 63) return "中雨";
    if (code == 65) return "大雨";
    if (code == 66 || code == 67) return "冻雨";
    if (code == 71 || code == 77) return "小雪";
    if (code == 73) return "中雪";
    if (code == 75) return "大雪";
    if (code == 80 || code == 81) return "阵雨";
    if (code == 82) return "强阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code == 95) return "雷雨";
    if (code == 96 || code == 99) return "雷雨冰雹";
    return "未知";
}

static void persist_cache(const home_weather_t *weather, const char *lat, const char *lon) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_i16(handle, "wtemp", weather->temp_c) == ESP_OK &&
        nvs_set_i16(handle, "wcode", (int16_t)weather->code) == ESP_OK &&
        nvs_set_u8(handle, "whum", weather->humidity) == ESP_OK &&
        nvs_set_u8(handle, "wwind", weather->wind_kmh) == ESP_OK &&
        nvs_set_u16(handle, "wdir", weather->wind_dir) == ESP_OK &&
        nvs_set_u32(handle, "wts", weather->updated_at) == ESP_OK &&
        nvs_set_str(handle, "wlat", lat) == ESP_OK &&
        nvs_set_str(handle, "wlon", lon) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

// 蒲福风级:km/h → 0..12 级,阈值来自国标蒲福风级表。
int home_weather_beaufort(uint8_t wind_kmh) {
    static const uint8_t kThresh[] = {1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
    int level = 0;
    while (level < 12 && wind_kmh >= kThresh[level]) ++level;
    return level;
}

// 风向 16 点位就近归到 8 方位(北=0°,顺时针)。字形 东南西北风无 均在 buddy 字库内。
const char *home_weather_wind_dir_text(uint16_t deg) {
    static const char *kDirs[8] = {
        "北风", "东北风", "东风", "东南风",
        "南风", "西南风", "西风", "西北风",
    };
    return kDirs[((deg + 23) % 360) / 45];
}

void home_weather_age_text(uint32_t updated_at, int64_t now, char *out, size_t capacity) {
    if (!out || !capacity) return;
    if (updated_at < EPOCH_VALID_MIN || now < EPOCH_VALID_MIN) {
        snprintf(out, capacity, "离线数据");
        return;
    }
    const int64_t age = now > (int64_t)updated_at ? now - (int64_t)updated_at : 0;
    if (age < 120) snprintf(out, capacity, "刚刚");
    else if (age < 3600) snprintf(out, capacity, "%d分钟前", (int)(age / 60));
    else if (age < 86400) snprintf(out, capacity, "%d小时前", (int)(age / 3600));
    else snprintf(out, capacity, "%d天前", (int)(age / 86400 > 99 ? 99 : age / 86400));
}

// 坐标所在地的 UTC 偏移(timezone=auto 时返回,含夏令时):设备时区跟着城市走,
// 出国或跨时区不用再手动改。只接受 15 分钟整倍数的合理值。
static void apply_utc_offset(const cJSON *root) {
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_seconds");
    if (!cJSON_IsNumber(offset) || !isfinite(offset->valuedouble)) return;
    const double sec = offset->valuedouble;
    if (sec < -720 * 60 || sec > 840 * 60) return;
    const int seconds = (int)sec;
    if (seconds % 900 != 0) return;
    time_sync_set_tz_offset_min(seconds / 60);
}

// Open-Meteo current 响应形如
// {"latitude":39.9,"current_units":{...},"current":{"time":"...","temperature_2m":26.1,"weather_code":2,...}}
static esp_err_t weather_parse(const char *body, const char *lat, const char *lon) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Weather JSON parse failed");
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    const cJSON *hum = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    const cJSON *wind = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
    const cJSON *dir = cJSON_GetObjectItemCaseSensitive(current, "wind_direction_10m");
    // Validate before floating-point -> integer conversion. Missing fields and
    // huge numbers must not become plausible 0% / calm wind or overflow a cast.
    if (cJSON_IsNumber(temp) && isfinite(temp->valuedouble) && temp->valuedouble >= -100 && temp->valuedouble <= 100 &&
        cJSON_IsNumber(code) && isfinite(code->valuedouble) && code->valuedouble >= 0 && code->valuedouble <= 99 &&
        cJSON_IsNumber(hum) && isfinite(hum->valuedouble) && hum->valuedouble >= 0 && hum->valuedouble <= 100 &&
        cJSON_IsNumber(wind) && isfinite(wind->valuedouble) && wind->valuedouble >= 0 && wind->valuedouble <= 254 &&
        cJSON_IsNumber(dir) && isfinite(dir->valuedouble) && dir->valuedouble >= 0 && dir->valuedouble <= 360) {
        home_weather_t next = {.valid = true};
        next.temp_c = (int16_t)((temp->valuedouble >= 0)
                                         ? (temp->valuedouble + 0.5)
                                         : (temp->valuedouble - 0.5));
        next.code = (uint16_t)code->valuedouble;
        next.humidity = cJSON_IsNumber(hum)
                                 ? (uint8_t)hum->valuedouble : 0;
        next.wind_kmh = cJSON_IsNumber(wind)
                                 ? (uint8_t)(wind->valuedouble + 0.5) : 0;
        next.wind_dir = cJSON_IsNumber(dir)
                                 ? (uint16_t)dir->valuedouble % 360 : 0;
        const time_t now = time(NULL);
        next.updated_at = now >= EPOCH_VALID_MIN ? (uint32_t)now : 0;
        apply_utc_offset(root);
        portENTER_CRITICAL(&s_weather_mux);
        s_weather = next;
        snprintf(s_lat, sizeof(s_lat), "%s", lat);
        snprintf(s_lon, sizeof(s_lon), "%s", lon);
        s_version++;
        portEXIT_CRITICAL(&s_weather_mux);
        persist_cache(&next, lat, lon);
        ESP_LOGI(TAG, "Weather updated: %dC code=%u hum=%u%% wind=%ukmh@%u",
                 (int)next.temp_c, (unsigned)next.code,
                 (unsigned)next.humidity, (unsigned)next.wind_kmh,
                 (unsigned)next.wind_dir);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Weather JSON missing fields");
    }
    cJSON_Delete(root);
    return err;
}

typedef esp_err_t (*current_parser_t)(const char *, const char *, const char *);

typedef struct {
    char body[RESPONSE_MAX];
    size_t used;
    bool overflow;
} current_response_t;

static esp_err_t receive_current(esp_http_client_event_t *event) {
    current_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        // Give TCP/TLS connection setup enough time on power-saving Wi-Fi, then
        // use short response polls so leaving Home can still cancel promptly.
        esp_http_client_set_timeout_ms(event->client, 500);
        ESP_LOGI(TAG, "Current HTTPS connected");
    }
    if (event->event_id == HTTP_EVENT_HEADER_SENT) ESP_LOGI(TAG, "Current request sent");
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if ((size_t)event->data_len > sizeof(response->body) - 1 - response->used) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->body + response->used, event->data, (size_t)event->data_len);
    response->used += (size_t)event->data_len;
    return ESP_OK;
}

// One bounded response buffer and HTTP client at a time for weather and PM2.5.
static esp_err_t fetch_current(const char *url, const char *lat, const char *lon,
                               current_parser_t parser, int timeout_ms) {
    if (requests_cancelled()) return ESP_FAIL;
    current_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 2000, /* connection poll; shortened to 500ms after TLS */
        .buffer_size = 1024, /* TLS 接收缓冲,小内存约束 */
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = receive_current,
        .user_data = &response,
        .is_async = true,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "HTTP client init failed");
        return ESP_FAIL;
    }

    const int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    esp_err_t err = ESP_ERR_HTTP_EAGAIN;
    while (!requests_cancelled() && esp_timer_get_time() < deadline && !response.overflow) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    const int status = esp_http_client_get_status_code(client);
    const bool complete = esp_http_client_is_complete_data_received(client);
    const bool ok = err == ESP_OK && status == 200 && complete && response.used &&
                    !response.overflow && !requests_cancelled();
    // Release TLS allocations before cJSON allocates its small response tree.
    esp_http_client_cleanup(client);
    if (!ok) {
        ESP_LOGW(TAG, "Current data request failed: HTTP=%d len=%u err=%d",
                 status, (unsigned)response.used, (int)err);
        return ESP_FAIL;
    }
    response.body[response.used] = '\0';
    return parser(response.body, lat, lon);
}

esp_err_t home_weather_refresh(const char *lat, const char *lon) {
    if (!home_profile_coordinates_valid(lat, lon) || !lat[0] || !lon[0]) return ESP_ERR_INVALID_ARG;
    const unsigned host_count = sizeof(kWeatherHosts) / sizeof(kWeatherHosts[0]);
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < host_count && !requests_cancelled(); ++attempt) {
        const unsigned host = (s_weather_host + attempt) % host_count;
        char url[320];
        snprintf(url, sizeof(url),
                 "https://%s/v1/forecast"
                 "?latitude=%s&longitude=%s"
                 "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m,wind_direction_10m"
                 "&timezone=auto", kWeatherHosts[host], lat, lon);
        err = fetch_current(url, lat, lon, weather_parse, WEATHER_HOST_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (host != s_weather_host) ESP_LOGI(TAG, "Weather host switched to %s", kWeatherHosts[host]);
            s_weather_host = host;
            return ESP_OK;
        }
        if (attempt + 1 < host_count && !requests_cancelled())
            ESP_LOGW(TAG, "Weather host %s failed; trying fallback", kWeatherHosts[host]);
    }
    return err;
}

static esp_err_t air_parse(const char *body, const char *lat, const char *lon) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_FAIL;
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *pm = cJSON_GetObjectItemCaseSensitive(current, "pm2_5");
    const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "current_units");
    const cJSON *unit = cJSON_GetObjectItemCaseSensitive(units, "pm2_5");
    const bool unit_ok = cJSON_IsString(unit) &&
        (!strcmp(unit->valuestring, "μg/m³") || !strcmp(unit->valuestring, "µg/m³") ||
         !strcmp(unit->valuestring, "ug/m3"));
    esp_err_t err = ESP_FAIL;
    if (unit_ok && cJSON_IsNumber(pm) && isfinite(pm->valuedouble) &&
        pm->valuedouble >= 0 && pm->valuedouble <= 10000) {
        const home_air_quality_t next = {
            .valid = true, .pm25_x10 = (uint32_t)(pm->valuedouble * 10 + 0.5)
        };
        const int64_t updated_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_weather_mux);
        s_air = next;
        s_air_updated_us = updated_us;
        snprintf(s_air_lat, sizeof(s_air_lat), "%s", lat);
        snprintf(s_air_lon, sizeof(s_air_lon), "%s", lon);
        s_version++;
        portEXIT_CRITICAL(&s_weather_mux);
        ESP_LOGI(TAG, "PM2.5 model concentration: %u.%u ug/m3",
                 (unsigned)(next.pm25_x10 / 10), (unsigned)(next.pm25_x10 % 10));
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "PM2.5 missing/invalid value or unit");
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t home_air_quality_refresh(const char *lat, const char *lon) {
    if (!home_profile_coordinates_valid(lat, lon) || !lat[0] || !lon[0]) return ESP_ERR_INVALID_ARG;
    // Do not present an old sample as current when this request fails.
    portENTER_CRITICAL(&s_weather_mux);
    s_air.valid = false;
    s_version++;
    portEXIT_CRITICAL(&s_weather_mux);
    char url[224];
    snprintf(url, sizeof(url),
             "https://air-quality-api.open-meteo.com/v1/air-quality"
             "?latitude=%s&longitude=%s&current=pm2_5&timezone=auto", lat, lon);
    return fetch_current(url, lat, lon, air_parse, HTTP_TIMEOUT_MS);
}
