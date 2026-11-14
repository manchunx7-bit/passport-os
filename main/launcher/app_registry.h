#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化应用注册表
void app_registry_init(void);

// 获取应用数量（不含 Launcher）
size_t app_registry_get_count(void);

// 获取指定索引的应用描述
const passport_app_t *app_registry_get_by_index(size_t index);

// 获取指定 ID 的应用描述
const passport_app_t *app_registry_get_by_id(passport_app_id_t id);

// 获取当前处于前台激活的应用 ID
passport_app_id_t app_registry_get_active(void);
// True while another navigation is already retiring or starting an app.
bool app_registry_switch_pending(void);

// 开机时调用一次:若上次是为进入重型应用而软重启,返回该应用 ID(否则
// APP_ID_COUNT)。记录读后即清除。
passport_app_id_t app_registry_take_clean_launch(void);

// 切换前台应用（若传入 APP_ID_LAUNCHER 则返回主菜单）
void app_registry_switch_to(passport_app_id_t id);
// Run from the input task's full-size stack, never from sys_mon or LVGL.
void app_registry_process_pending_switch(void);
// Phone setup's background completion must not override a user shortcut.
void app_registry_complete_setup(void);

// 全局按键统一分发（双击 OK 打开小智、长按返回与熄屏唤醒保护）
void app_registry_dispatch_key(uint8_t btn, app_btn_event_t event, uint16_t mv);

// 由系统心跳调用；统一处理跨 App 的息屏，不额外创建常驻任务。
void app_registry_tick(void);
void app_registry_set_screen_timeout(uint16_t seconds);
uint16_t app_registry_get_screen_timeout(void);
bool app_registry_screen_is_on(void);
// Screen off and the foreground app is not streaming (radio/music/voice/Xiaozhi/
// Muse): background polling may slow down without anyone noticing.
bool app_registry_background_idle(void);
void app_registry_log_display_state(void);

// 活动(app 活动)续期:非按键驱动的应用(如声控游戏)在持续运行时调用,
// 抑制息屏计时。只刷新计时,不会点亮已熄灭的屏幕。
void app_registry_keep_awake(void);
// Caller holds LVGL lock. Wake without switching apps; false during handoff.
bool app_registry_wake_for_notice(void);
// Caller holds LVGL lock. Never wait for the lifecycle lock or seize audio.
bool app_registry_sound_notice(void);

#ifdef __cplusplus
}
#endif
