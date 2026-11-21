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

typedef struct passport_app_s {
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

    // 插件元数据:注册表、设置页和设备控制都只读这些字段,
    // 不再按 app id 写 switch,新增应用只改自己的描述符。
#ifdef __cplusplus
    const char *key = nullptr;  // 持久化与 MCP 使用的稳定标识
    bool in_launcher = false;   // 出现在应用列表
    bool openable = false;      // 允许设备控制打开
    bool optional = false;      // 用户可在设置页开关
    bool default_enabled = false; // 无 NVS 记录时的初始状态
    bool allow_toast = false;   // 前台按键反馈音
    bool streaming = false;     // 息屏后仍需保持低延迟网络/音频
    uint16_t heap_largest_kb = 0; // 进入前要求单块连续内存,0 表示轻量应用
    uint16_t heap_free_kb = 0;  // 进入前要求总空闲内存
#else
    const char *key;
    bool in_launcher;
    bool openable;
    bool optional;
    bool default_enabled;
    bool allow_toast;
    bool streaming;
    uint16_t heap_largest_kb;
    uint16_t heap_free_kb;
#endif
} passport_app_t;

#ifdef __cplusplus
}
#endif
