#include "apps/home/home_upload.h"

#include "apps/home/home_profile.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <esp_random.h>      // esp_fill_random():每次开启上传窗口换一个一次性 token
#include "wifi_manager.h"
#include "passport_muse.h"
#include "ui/ui_palette.h"

#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <atomic>
#include <algorithm>
#include <new>
#include <string>
#include "ssid_manager.h"
#include "home_wifi_selection.h"
#include <mutex>
#include "esp_timer.h"
#include "phone_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "home_upload";

static httpd_handle_t s_server;
static char s_url[32];

// 2026-09-13 修复(B8):上传接口原先完全无鉴权,同一局域网任何人都能 POST
// /api/profile 或 /api/avatar 改写名片与人像。现在每次开启上传窗口生成一个
// 一次性 token,页面里通过 window.__T 带上,三个 /api/* 接口都校验 ?t=。
// 这不是强认证(设备没有账号体系),但足以挡住"随手扫一下端口就能改"的滥用。
static uint32_t s_token = 0;

// 从 req->uri 的 query 里取 t= 并比对。URL 形如 /api/profile?t=123456
static bool token_ok(httpd_req_t *req) {
    if (s_token == 0) return false;
    char query[96], token[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "t", token, sizeof(token)) != ESP_OK) return false;
    for (const char *p = token; *p; ++p) if (*p < '0' || *p > '9') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(token, &end, 10);
    return token[0] && end && !*end && errno != ERANGE && value <= UINT32_MAX &&
           static_cast<uint32_t>(value) == s_token;
}

// ---------------------------------------------------------------------------
// Phone-side crop, pixel grid and four-tone quantization; upload packed 2bpp indices.
// ---------------------------------------------------------------------------
extern const char phone_page_start[] asm("_binary_phone_setup_html_gz_start");
extern const char phone_page_end[] asm("_binary_phone_setup_html_gz_end");
extern const char cities_start[] asm("_binary_cities_json_gz_start");
extern const char cities_end[] asm("_binary_cities_json_gz_end");
static std::atomic<int> s_network_state{0}; // 0 idle, 1 connecting, 2 verified, 3 failed
static std::atomic<int64_t> s_finish_at{0};
static bool s_had_saved = false;
static std::string s_target_ssid;
static std::mutex s_network_mutex;
static std::vector<SsidItem> s_saved_networks;

// ---------------------------------------------------------------------------
// 处理器
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, int status, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_status(req, status == 200 ? "200 OK" :
                              status == 401 ? "401 Unauthorized" : "400 Bad Request");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_captive(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t handler_index(httpd_req_t *req) {
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host, "192.168.4.1") && strcmp(host, "192.168.4.1:80")))
        return handler_captive(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // 页面在 flash 中已 gzip,无法再原样注入;一次性 token 改由 cookie 交给页面脚本
    // (页面读出后仍走 t= 参数鉴权,cookie 本身不参与鉴权,不引入 CSRF)。
    char cookie[48];
    std::snprintf(cookie, sizeof(cookie), "pt=%lu; Path=/; SameSite=Strict",
                  (unsigned long)s_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return httpd_resp_send(req, phone_page_start, phone_page_end - phone_page_start);
}

static esp_err_t handler_cities(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // Browser decompresses; serve straight from flash, never inflate in C3 RAM.
    return httpd_resp_send(req, cities_start, cities_end - cities_start);
}

static esp_err_t handler_profile_get(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    home_profile_t profile;
    home_profile_get(&profile);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", profile.name);
    cJSON_AddStringToObject(root, "signature", profile.signature);
    cJSON_AddStringToObject(root, "city", profile.city);
    cJSON_AddStringToObject(root, "address", profile.address);
    cJSON_AddStringToObject(root, "lat", profile.lat);
    cJSON_AddStringToObject(root, "lon", profile.lon);
    cJSON_AddStringToObject(root, "region", profile.region);
    cJSON_AddStringToObject(root, "radio_city", profile.radio_city);
    cJSON_AddStringToObject(root, "country_code", profile.country_code);
    cJSON_AddBoolToObject(root, "avatar", home_profile_has_avatar());
    cJSON *palette = cJSON_AddArrayToObject(root, "portrait_palette");
    for (uint8_t level = 0; level < 4; ++level)
        cJSON_AddItemToArray(palette, cJSON_CreateNumber(ui_theme_portrait_color(level)));
    cJSON *subject = cJSON_AddArrayToObject(root, "portrait_subject_palette");
    for (uint8_t level = 0; level < 4; ++level)
        cJSON_AddItemToArray(subject, cJSON_CreateNumber(ui_theme_portrait_subject_color(level)));
    cJSON_AddBoolToObject(root, "share_image", home_profile_has_share_image());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_json(req, 400, "{\"error\":\"oom\"}");
    esp_err_t err = send_json(req, 200, json);
    cJSON_free(json);
    return err;
}

static esp_err_t handler_profile_post(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    char body[1024];
    const size_t len = (size_t)req->content_len;
    if (len == 0 || len > sizeof(body) - 1) {
        return send_json(req, 400, "{\"error\":\"bad size\"}");
    }
    int received = 0;
    while (received < (int)len) {
        int n = httpd_req_recv(req, body + received, len - received);
        if (n <= 0) return send_json(req, 400, "{\"error\":\"recv\"}");
        received += n;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_json(req, 400, "{\"error\":\"json\"}");

    // 只接受网页会填的字段;缺省按空串处理,等于允许清空资料
    home_profile_t in = {};
    const auto grab = [&](const char *key, char *dst, size_t cap) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        const char *value = cJSON_IsString(item) ? item->valuestring : "";
        if (std::strlen(value) >= cap) return false;
        std::memcpy(dst, value, std::strlen(value) + 1);
        return true;
    };
    const bool valid = grab("name", in.name, HOME_NAME_MAX) &&
        grab("signature", in.signature, HOME_SIGNATURE_MAX) &&
        grab("city", in.city, HOME_CITY_MAX) &&
        grab("address", in.address, HOME_ADDRESS_MAX) &&
        grab("lat", in.lat, HOME_COORD_MAX) &&
        grab("lon", in.lon, HOME_COORD_MAX) &&
        grab("region", in.region, HOME_REGION_MAX) &&
        grab("radio_city", in.radio_city, HOME_CITY_MAX) &&
        grab("country_code", in.country_code, sizeof(in.country_code));
    cJSON_Delete(root);
    if (!valid) return send_json(req, 400, "{\"error\":\"text too long\"}");

    esp_err_t err = home_profile_set_text(&in);
    return send_json(req, err == ESP_OK ? 200 : 400,
                     err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"invalid\"}");
}

static esp_err_t send_muse_config(httpd_req_t *req) {
    bool token_set = false, paired = false;
    passport_muse_config_status(&token_set, &paired);
    char proxy_host[16]; uint16_t proxy_port = 0;
    passport_muse_proxy_config(proxy_host, sizeof(proxy_host), &proxy_port);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddBoolToObject(obj, "token_set", token_set);
    cJSON_AddBoolToObject(obj, "paired", paired);
    cJSON_AddStringToObject(obj, "proxy_host", proxy_host);
    cJSON_AddNumberToObject(obj, "proxy_port", proxy_port);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return send_json(req, 500, "{\"error\":\"no_memory\"}");
    esp_err_t err = send_json(req, 200, json);
    cJSON_free(json);
    return err;
}
static esp_err_t handler_muse_get(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    return send_muse_config(req);
}
static esp_err_t handler_muse_post(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    char body[512];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) return send_json(req, 400, "{\"error\":\"bad size\"}");
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return send_json(req, 400, "{\"error\":\"recv\"}");
        received += n;
    }
    body[received] = 0;
    cJSON *obj = cJSON_Parse(body);
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(obj, "sdk_token");
    const cJSON *proxy_host = cJSON_GetObjectItemCaseSensitive(obj, "proxy_host");
    const cJSON *proxy_port = cJSON_GetObjectItemCaseSensitive(obj, "proxy_port");
    bool proxy_valid = !proxy_host || (cJSON_IsString(proxy_host) && cJSON_IsNumber(proxy_port) &&
        proxy_port->valuedouble >= 0 && proxy_port->valuedouble <= 65535 &&
        proxy_port->valuedouble == proxy_port->valueint &&
        passport_muse_proxy_valid(proxy_host->valuestring, (uint16_t)proxy_port->valueint));
    esp_err_t err = proxy_valid ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && token) err = cJSON_IsString(token) ? passport_muse_set_sdk_token(token->valuestring) : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && proxy_host) err = passport_muse_set_proxy(proxy_host->valuestring, (uint16_t)proxy_port->valueint);
    if (!token && !proxy_host) err = ESP_ERR_INVALID_ARG;
    cJSON_Delete(obj);
    std::memset(body, 0, sizeof(body));
    if (err == ESP_OK) return send_muse_config(req);
    return send_json(req, 400, err == ESP_ERR_INVALID_STATE ? "{\"error\":\"muse_active\"}" : "{\"error\":\"muse_config\"}");
}

static esp_err_t handler_avatar_get(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    auto *data = static_cast<uint8_t *>(std::malloc(HOME_AVATAR_BYTES));
    if (!data) return send_json(req, 400, "{\"error\":\"oom\"}");
    const esp_err_t loaded = home_profile_read_avatar(data, HOME_AVATAR_BYTES);
    if (loaded != ESP_OK) {
        std::free(data);
        return send_json(req, 400, "{\"error\":\"avatar\"}");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    const esp_err_t sent = httpd_resp_send(req, reinterpret_cast<const char *>(data), HOME_AVATAR_BYTES);
    std::free(data);
    return sent;
}

static esp_err_t send_avatar_upload_error(httpd_req_t *req, const char *error, esp_err_t code) {
    char json[128];
    std::snprintf(json, sizeof(json), "{\"error\":\"%s\",\"code\":\"%s\"}",
                  error, esp_err_to_name(code));
    return send_json(req, 400, json);
}

static esp_err_t handler_avatar_post(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    const size_t total = (size_t)req->content_len;
    if (!home_portrait_upload_size(total)) {
        ESP_LOGW(TAG, "Avatar upload wrong content-length %d", (int)req->content_len);
        return send_avatar_upload_error(req, "avatar_size", ESP_ERR_INVALID_SIZE);
    }

    // httpd_req_recv 的缓冲是 char*,而落盘接口要 uint8_t*;在调用处做一次转换,
    // 比把整个缓冲声明成 uint8_t 再在 recv 上强转更安全(C++ 下 recv 不会接受 uint8_t*)。
    char chunk[512];
    size_t remaining = total;
    bool first = true;
    // Bound retries; an in-flight read can still take the configured 5s socket timeout.
    const int64_t deadline = esp_timer_get_time() + 10000000;
    while (remaining > 0) {
        int n = esp_timer_get_time() < deadline
            ? httpd_req_recv(req, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk))
            : HTTPD_SOCK_ERR_TIMEOUT;
        if (n == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline) continue;
        if (n <= 0) {
            home_profile_abort_avatar_upload();
            ESP_LOGW(TAG, "Avatar recv aborted: received=%u/%u socket=%d",
                     (unsigned)(total - remaining), (unsigned)total, n);
            return send_avatar_upload_error(req, "avatar_recv", n == HTTPD_SOCK_ERR_TIMEOUT
                ? ESP_ERR_TIMEOUT : ESP_FAIL);
        }
        // 2026-09-13 修复(B3):原来直接 remaining -= n。若对端发来超过剩余
        // 长度的字节(异常/分块编码/代理行为),size_t 下溢成巨大值 →
        // while 死循环,永久占住这个 httpd 会话与 6KB 栈任务。
        if ((size_t)n > remaining) {
            ESP_LOGW(TAG, "Avatar oversized: got %d, only %u left; truncating",
                     n, (unsigned)remaining);
            n = (int)remaining;
        }
        remaining -= (size_t)n;
        esp_err_t err = home_profile_write_avatar_chunk(reinterpret_cast<const uint8_t *>(chunk),
                                                        (size_t)n, first, remaining == 0, total);
        first = false;
        if (err != ESP_OK) {
            home_profile_abort_avatar_upload();
            ESP_LOGW(TAG, "Avatar store rejected: received=%u/%u err=%s",
                     (unsigned)(total - remaining), (unsigned)total, esp_err_to_name(err));
            return send_avatar_upload_error(req, "avatar_store", err);
        }
    }
    return send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t handler_share_post(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    char query[96], value[12];
    unsigned slot = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "slot", value, sizeof(value)) == ESP_OK) {
        if (std::strlen(value) != 1 || value[0] < '0' || value[0] >= '0' + HOME_SHARE_MAX_IMAGES)
            return send_json(req, 400, "{\"error\":\"card_slot\"}");
        slot = static_cast<unsigned>(value[0] - '0');
    }
    if (req->content_len <= 0 || (size_t)req->content_len > HOME_SHARE_MAX_BYTES) {
        ESP_LOGW(TAG, "Share image invalid content-length %d (max %u)",
                 (int)req->content_len, (unsigned)HOME_SHARE_MAX_BYTES);
        return send_json(req, 400, "{\"error\":\"size\"}");
    }

    char chunk[512];
    size_t remaining = (size_t)req->content_len;
    bool first = true;
    while (remaining > 0) {
        int n = httpd_req_recv(req, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk));
        if (n <= 0) {
            home_profile_abort_card_upload();
            ESP_LOGW(TAG, "Share image recv aborted at remaining %u", (unsigned)remaining);
            return send_json(req, 400, "{\"error\":\"recv\"}");
        }
        if ((size_t)n > remaining) n = (int)remaining;
        remaining -= (size_t)n;
        esp_err_t err = home_profile_write_card_chunk(slot,
            reinterpret_cast<const uint8_t *>(chunk), (size_t)n, first, remaining == 0);
        first = false;
        if (err != ESP_OK) {
            home_profile_abort_card_upload();
            return send_json(req, 400, err == ESP_ERR_NO_MEM ? "{\"error\":\"storage_full\"}" :
                err == ESP_ERR_INVALID_ARG ? "{\"error\":\"card_image\"}" : "{\"error\":\"store\"}");
        }
    }
    return send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t handler_cards_get(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    home_share_gallery_t gallery;
    if (home_profile_gallery_get(&gallery) != ESP_OK) return send_json(req, 400, "{\"error\":\"store\"}");
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_json(req, 400, "{\"error\":\"oom\"}");
    cJSON_AddNumberToObject(root, "max", HOME_SHARE_MAX_IMAGES);
    cJSON_AddNumberToObject(root, "per_image_bytes", HOME_SHARE_MAX_BYTES);
    cJSON_AddNumberToObject(root, "storage_free", gallery.storage_free);
    cJSON_AddNumberToObject(root, "storage_reserve", HOME_SHARE_STORAGE_RESERVE);
    cJSON *images = cJSON_AddArrayToObject(root, "images");
    for (unsigned i = 0; images && i < gallery.count; ++i) {
        cJSON *card = cJSON_CreateObject();
        if (!card) { cJSON_Delete(root); return send_json(req, 400, "{\"error\":\"oom\"}"); }
        cJSON_AddNumberToObject(card, "slot", gallery.slots[i]);
        cJSON_AddNumberToObject(card, "bytes", gallery.bytes[i]); cJSON_AddItemToArray(images, card);
    }
    char *json = images ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (!json) return send_json(req, 400, "{\"error\":\"oom\"}");
    esp_err_t err = send_json(req, 200, json); cJSON_free(json); return err;
}

static esp_err_t handler_cards_post(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    if (req->content_len <= 0 || req->content_len >= 128) return send_json(req, 400, "{\"error\":\"size\"}");
    char body[128]; size_t got = 0;
    while (got < static_cast<size_t>(req->content_len)) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return send_json(req, 400, "{\"error\":\"recv\"}");
        got += n;
    }
    body[got] = 0;
    cJSON *root = cJSON_Parse(body), *order = cJSON_GetObjectItemCaseSensitive(root, "order");
    uint8_t slots[HOME_SHARE_MAX_IMAGES]; unsigned count = 0; bool valid = cJSON_IsArray(order);
    cJSON *item;
    cJSON_ArrayForEach(item, order) {
        if (count >= HOME_SHARE_MAX_IMAGES || !cJSON_IsNumber(item) || item->valuedouble < 0 ||
            item->valuedouble >= HOME_SHARE_MAX_IMAGES || item->valuedouble != item->valueint) { valid = false; break; }
        slots[count++] = static_cast<uint8_t>(item->valueint);
    }
    esp_err_t err = valid ? home_profile_gallery_order(slots, count) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    return send_json(req, err == ESP_OK ? 200 : 400,
        err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"card_order\"}");
}

static esp_err_t handler_share_get(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    char query[96], value[12], path[HOME_SHARE_PATH_MAX];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", value, sizeof(value)) != ESP_OK ||
        std::strlen(value) != 1 || value[0] < '0' || value[0] >= '0' + HOME_SHARE_MAX_IMAGES ||
        !home_profile_share_path_for_slot(value[0] - '0', path, sizeof(path)))
        return send_json(req, 404, "{\"error\":\"card_slot\"}");
    FILE *fp = std::fopen(path, "rb"); if (!fp) return send_json(req, 404, "{\"error\":\"store\"}");
    httpd_resp_set_type(req, "image/jpeg"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[512]; size_t n; esp_err_t err = ESP_OK;
    while ((n = std::fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        err = httpd_resp_send_chunk(req, chunk, n); if (err != ESP_OK) break;
    }
    if (std::ferror(fp)) err = ESP_FAIL;
    std::fclose(fp);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

// ---------------------------------------------------------------------------

struct NetworkJob { std::vector<SsidItem> candidates; };

static void network_worker(void *arg) {
    auto *job = static_cast<NetworkJob *>(arg);
    bool ok = false;
    for (auto &candidate : job->candidates) {
        {
            std::lock_guard<std::mutex> lock(s_network_mutex);
            s_target_ssid = candidate.ssid;
        }
        if (WifiManager::GetInstance().ConfigureNetwork(candidate.ssid, candidate.password, true)) {
            ok = true;
            break;
        }
    }
    // Do not log credentials; dispose of the request before publishing completion.
    for (auto &candidate : job->candidates)
        std::fill(candidate.password.begin(), candidate.password.end(), '\0');
    delete job;
    s_network_state.store(ok ? 2 : 3);
    vTaskDelete(nullptr);
}

static esp_err_t reply_object(httpd_req_t *req, cJSON *root) {
    if (!root) return send_json(req, 400, "{\"error\":\"oom\"}");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_json(req, 400, "{\"error\":\"oom\"}");
    auto err = send_json(req, 200, json);
    cJSON_free(json);
    return err;
}

static esp_err_t handler_setup(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    auto *root = cJSON_CreateObject();
    const char *states[] = {"idle", "connecting", "connected", "failed"};
    cJSON_AddStringToObject(root, "state", states[s_network_state.load()]);
    cJSON_AddBoolToObject(root, "saved_network", s_had_saved);
    auto *saved = cJSON_AddArrayToObject(root, "saved_networks");
    for (const auto &item : s_saved_networks)
        cJSON_AddItemToArray(saved, cJSON_CreateString(item.ssid.c_str()));
    cJSON_AddBoolToObject(root, "finishing", s_finish_at.load() != 0);
    {
        std::lock_guard<std::mutex> lock(s_network_mutex);
        cJSON_AddStringToObject(root, "ssid", s_target_ssid.c_str());
    }
    cJSON_AddStringToObject(root, "hotspot", WifiManager::GetInstance().GetApSsid().c_str());
    return reply_object(req, root);
}

static esp_err_t handler_networks(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    auto aps = WifiManager::GetInstance().GetConfigAccessPoints();
    std::sort(aps.begin(), aps.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });
    auto *root = cJSON_CreateArray();
    std::vector<std::string> seen;
    for (const auto &ap : aps) {
        const std::string ssid(reinterpret_cast<const char *>(ap.ssid), strnlen(reinterpret_cast<const char *>(ap.ssid), 32));
        if (ssid.empty() || std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
        if (seen.size() >= 20) break;
        seen.push_back(ssid);
        auto *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", ap.rssi);
        cJSON_AddBoolToObject(item, "secure", ap.authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(root, item);
    }
    return reply_object(req, root);
}

static esp_err_t handler_connect(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    if (s_network_state.load() == 1 || s_finish_at.load()) return send_json(req, 400, "{\"error\":\"busy\"}");
    char body[768];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) return send_json(req, 400, "{\"error\":\"size\"}");
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return send_json(req, 400, "{\"error\":\"recv\"}");
        received += n;
    }
    body[received] = 0;
    auto *root = cJSON_Parse(body);
    if (!root) return send_json(req, 400, "{\"error\":\"json\"}");
    auto *job = new (std::nothrow) NetworkJob;
    if (!job) { cJSON_Delete(root); return send_json(req, 400, "{\"error\":\"oom\"}"); }
    bool valid = false;
    const bool reuse = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "saved"));
    if (reuse) {
        const auto *selected = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        if (!selected || cJSON_IsString(selected)) {
            job->candidates = home_wifi_candidates(s_saved_networks,
                WifiManager::GetInstance().GetConfigAccessPoints(), selected ? selected->valuestring : "");
            valid = !job->candidates.empty();
        }
    } else {
        const auto *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const auto *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (cJSON_IsString(ssid) && cJSON_IsString(password)) {
            const std::string name = ssid->valuestring, key = password->valuestring;
            const auto n = key.size();
            const bool hex_key = n == 64 && key.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
            valid = !name.empty() && name.size() <= 32 && (n == 0 || (n >= 8 && n <= 63) || hex_key);
            if (valid) job->candidates.push_back({name, key});
        }
    }
    if (valid) wifi_phone_time_apply(root);
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    if (!valid) { delete job; return send_json(req, 400, reuse ? "{\"error\":\"saved_not_found\"}" : "{\"error\":\"credentials\"}"); }
    s_network_state = 1;
    if (xTaskCreate(network_worker, "phone_wifi", 4096, job, 4, nullptr) != pdPASS) {
        delete job; s_network_state = 3;
        return send_json(req, 400, "{\"error\":\"oom\"}");
    }
    return send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t handler_finish(httpd_req_t *req) {
    if (!token_ok(req)) return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    const auto state = s_network_state.load();
    if (state == 1 || (!s_had_saved && state != 2)) return send_json(req, 400, "{\"error\":\"network_required\"}");
    // Let the phone receive confirmation before the system task tears down HTTP/AP.
    s_finish_at = esp_timer_get_time() + 3500000;
    return send_json(req, 200, "{\"ok\":true}");
}

esp_err_t home_upload_start(void) {
    if (s_server) return ESP_OK;
    if (!WifiManager::GetInstance().IsConfigMode()) return ESP_ERR_INVALID_STATE;
    s_network_state = 0; s_finish_at = 0; s_target_ssid.clear();
    s_saved_networks = SsidManager::GetInstance().GetSsidList();
    s_had_saved = !s_saved_networks.empty();

    // 每个上传窗口换一个 token(见 token_ok 注释)。用 esp_fill_random 而非
    // esp_random():后者在同批 IDF 头文件组合下出现过未声明的编译问题,
    // 而 xiaozhi 侧已稳定使用 esp_fill_random。
    esp_fill_random(&s_token, sizeof(s_token));
    s_token &= 0x7fffffffu;
    if (s_token == 0) s_token = 1;   /* 0 被 token_ok 当作"未启用" */

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 17;
    // 10 lwIP sockets total: leave room for DNS, SNTP, control/listen sockets,
    // and a new accept before LRU eviction. Phone OS probes also use HTTP.
    config.max_open_sockets = 3;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t index_uri = {"/", HTTP_GET, handler_index, NULL};
    const httpd_uri_t cities_uri = {"/api/cities", HTTP_GET, handler_cities, NULL};
    const httpd_uri_t profile_get = {"/api/profile", HTTP_GET, handler_profile_get, NULL};
    const httpd_uri_t profile_post = {"/api/profile", HTTP_POST, handler_profile_post, NULL};
    const httpd_uri_t avatar_post = {"/api/avatar", HTTP_POST, handler_avatar_post, NULL};
    const httpd_uri_t avatar_get = {"/api/avatar", HTTP_GET, handler_avatar_get, NULL};
    const httpd_uri_t share_post = {"/api/share", HTTP_POST, handler_share_post, NULL};
    const httpd_uri_t share_get = {"/api/share", HTTP_GET, handler_share_get, NULL};
    const httpd_uri_t cards_get = {"/api/cards", HTTP_GET, handler_cards_get, NULL};
    const httpd_uri_t cards_post = {"/api/cards", HTTP_POST, handler_cards_post, NULL};
    const httpd_uri_t setup_uri = {"/api/setup", HTTP_GET, handler_setup, NULL};
    const httpd_uri_t networks_uri = {"/api/networks", HTTP_GET, handler_networks, NULL};
    const httpd_uri_t connect_uri = {"/api/connect", HTTP_POST, handler_connect, NULL};
    const httpd_uri_t finish_uri = {"/api/finish", HTTP_POST, handler_finish, NULL};
    const httpd_uri_t muse_get = {"/api/muse", HTTP_GET, handler_muse_get, NULL};
    const httpd_uri_t muse_post = {"/api/muse", HTTP_POST, handler_muse_post, NULL};
    const httpd_uri_t captive_uri = {"/*", HTTP_GET, handler_captive, NULL};
    // 2026-09-13 修复(B8):注册返回值必须检查。原来四个 handler 全丢返回值,
    // 注册失败时页面直接 404 且日志里一行痕迹都没有。
    struct { const httpd_uri_t *uri; const char *name; } routes[] = {
        {&index_uri, "/"}, {&cities_uri, "GET /api/cities"}, {&profile_get, "GET /api/profile"},
        {&profile_post, "POST /api/profile"}, {&avatar_post, "POST /api/avatar"}, {&avatar_get, "GET /api/avatar"},
        {&share_post, "POST /api/share"},
        {&share_get, "GET /api/share"}, {&cards_get, "GET /api/cards"}, {&cards_post, "POST /api/cards"},
        {&setup_uri, "GET /api/setup"}, {&networks_uri, "GET /api/networks"},
        {&connect_uri, "POST /api/connect"}, {&finish_uri, "POST /api/finish"},
        {&muse_get, "GET /api/muse"}, {&muse_post, "POST /api/muse"},
        {&captive_uri, "captive portal"},
    };
    for (const auto &r : routes) {
        esp_err_t e = httpd_register_uri_handler(s_server, r.uri);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", r.name, esp_err_to_name(e));
            httpd_stop(s_server);
            s_server = nullptr;
            s_token = 0;
            return e;
        }
    }

    snprintf(s_url, sizeof(s_url), "http://%s",
             "192.168.4.1");
    ESP_LOGI(TAG, "Upload server started at %s", s_url);
    return ESP_OK;
}

void home_upload_stop(void) {
    if (!s_server) return;
    httpd_stop(s_server); // 内部 join 处理器任务后释放
    home_profile_abort_avatar_upload();
    home_profile_abort_card_upload();
    s_server = NULL;
    while (s_network_state.load() == 1) vTaskDelay(pdMS_TO_TICKS(20));
    for (auto &item : s_saved_networks) std::fill(item.password.begin(), item.password.end(), '\0');
    s_saved_networks.clear();
    s_finish_at = 0;
    s_token = 0;          // 窗口关闭即作废,旧链接立刻失效
    ESP_LOGI(TAG, "Upload server stopped");
}

bool home_upload_active(void) { return s_server != NULL; }

const char *home_upload_url(void) { return s_server ? s_url : ""; }

bool home_upload_busy(void) { return s_network_state.load() == 1; }
bool home_upload_network_verified(void) { return s_network_state.load() == 2; }
bool home_upload_finished(void) {
    const auto deadline = s_finish_at.load();
    return deadline != 0 && esp_timer_get_time() >= deadline;
}
