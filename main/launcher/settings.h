#pragma once

#include "apps/app_base.h"
#include "ui/ui_palette.h"

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
bool settings_save_theme(ui_theme_t theme);
bool settings_save_timeout(uint16_t seconds);
bool settings_save_key_sound(bool enabled);
uint8_t settings_get_volume(void);
uint8_t settings_get_brightness(void);
uint16_t settings_get_timeout(void);
ui_theme_t settings_get_theme(void);
bool settings_get_key_sound(void);

extern const passport_app_t g_settings_app;

#ifdef __cplusplus
}
#endif
