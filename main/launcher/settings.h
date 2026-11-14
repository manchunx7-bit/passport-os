#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// NVS 初始化后、Launcher 创建前调用，恢复整机设置并立即应用。
void settings_bootstrap(void);
void settings_tick(void);

// Shared by Settings and app volume keys; applies and persists only volume.
void settings_set_volume(uint8_t percent);
// Input-task APIs: persist first; failure leaves the applied setting unchanged.
bool settings_save_volume(uint8_t percent);
bool settings_save_brightness(uint8_t percent);
uint8_t settings_get_volume(void);
uint8_t settings_get_brightness(void);

extern const passport_app_t g_settings_app;

#ifdef __cplusplus
}
#endif
