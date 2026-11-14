#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ID_LAUNCHER = 0,
    APP_ID_RADIO,
    APP_ID_VOICE,
    APP_ID_XIAOZHI,
    APP_ID_WIFI,
    APP_ID_SETTINGS,
    APP_ID_HOME,
    APP_ID_GAME,
    APP_ID_TETRIS,
    APP_ID_STOPWATCH,
    APP_ID_SOUND_JUMP,
    APP_ID_MUSE,
    APP_ID_POMODORO,
    APP_ID_NETEASE,
    APP_ID_REMINDERS,
    APP_ID_LISTS,
    APP_ID_COUNT
} passport_app_id_t;

typedef enum {
    BTN_EVT_CLICK = 0,
    BTN_EVT_PRESS,
    BTN_EVT_RELEASE,
    BTN_EVT_LONG,
    BTN_EVT_LONG_UP,
    BTN_EVT_DOUBLE, // resolved by ADC task; reserved for global OK shortcut
} app_btn_event_t;

typedef struct {
    passport_app_id_t id;
    const char *name;           // 中文显示名称
    const char *en_name;        // 英文标签
    const char *desc;           // 简述
    const char *tag;            // 协议/网络特性标签 (如 "Wi-Fi", "BLE")
    uint32_t theme_color;       // 主题色 (RGB888)

    // 生命周期钩子
    void (*init)(void);         // 启动时初始化
    void (*start)(void);        // 进入前台运行
    void (*stop)(void);         // 退出前台（清理任务、注销网络、释放堆内存）
    void (*on_key)(uint8_t btn, app_btn_event_t event, uint16_t mv); // 按键事件分发
    // Request cooperative shutdown before releasing the UI or starting another
    // app. Return false while a worker still owns network/audio resources.
#ifdef __cplusplus
    bool (*prepare_stop)(void) = nullptr;
    // Consume long OK to navigate inside the app. False lets the registry exit.
    bool (*on_back)(void) = nullptr;
#else
    bool (*prepare_stop)(void);
    bool (*on_back)(void);
#endif
} passport_app_t;

#ifdef __cplusplus
}
#endif
