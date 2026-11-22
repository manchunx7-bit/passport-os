#pragma once
#include "registry_platform.h"
#include <cstdint>
#include <map>
#include <string>

using esp_err_t = int;
constexpr int ESP_ERR_NVS_NOT_FOUND = 0x1102;
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1

inline std::map<std::string, uint8_t> mock_nvs;
inline bool mock_nvs_fail = false;

inline esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) {
    *handle = 1;
    return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_commit(nvs_handle_t) { return mock_nvs_fail ? 1 : ESP_OK; }
inline esp_err_t nvs_get_u8(nvs_handle_t, const char *key, uint8_t *value) {
    const auto entry = mock_nvs.find(key);
    if (entry == mock_nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
    *value = entry->second;
    return ESP_OK;
}
inline esp_err_t nvs_set_u8(nvs_handle_t, const char *key, uint8_t value) {
    if (mock_nvs_fail) return 1;
    mock_nvs[key] = value;
    return ESP_OK;
}
