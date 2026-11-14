#include "launcher/launcher.h"
#include "launcher/timer_resume.h"
#include "launcher/app_registry.h"
#include "launcher/ok_gesture.h"
#include "boot_chime.h"
#include "ui_sfx.h"
#include "boot_splash.h"
#include "launcher/settings.h"
#include "launcher/wifi_setup.h"
#include "launcher/power.h"
#include "apps/home/home_profile.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_battery.h"
#include "esp_pm.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "bsp_i2c.h"
#include "ui/ui_capture.h"
#include "apps/pomodoro/app_pomodoro.h"
#include "apps/reminders/reminder_service.h"
#include "device_control.h"
#include "apps/lists/list_service.h"
#include "esp_log.h"
#include "passport_muse.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "bsp_memory.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !defined(CONFIG_PM_ENABLE) || !defined(CONFIG_FREERTOS_USE_TICKLESS_IDLE)
#error "Passport OS requires PM and tickless idle; regenerate sdkconfig from sdkconfig.defaults before building."
#endif

static const char *TAG = "main";

struct KeyMessage {
    uint8_t button;
    app_btn_event_t event;
    uint16_t mv;
};

static constexpr uint8_t kSerialCommand = 0xff;
// Power requests run on the input task: its stack fits LVGL and Wi-Fi teardown.
static constexpr uint8_t kPowerCommand = 0xfe;
static void handle_serial_command(int c);
static void dispatch_input_message(const KeyMessage &msg) {
    if (msg.button == kSerialCommand) handle_serial_command(msg.mv);
    else if (msg.button == kPowerCommand) power_hold(static_cast<uint8_t>(msg.mv));
    else {
        // Phone hotspot just switched on? Any key while offline rescans now.
        if (msg.event == BTN_EVT_PRESS && !WifiManager::GetInstance().IsConnected()) {
            WifiManager::GetInstance().ScanNow();
        }
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    }
}

static QueueHandle_t s_key_queue = nullptr;
// These tasks live for the entire boot. Static storage keeps their stacks out
// of holes left by the first Home/setup screen: late heap allocation pinned
// those holes and prevented Xiaozhi from obtaining a contiguous decoder block.
// ESP-IDF task stack sizes are in bytes. The serial reader only queues bytes;
// all app entry, capture and diagnostics execute on the existing input stack.
alignas(16) static StackType_t s_button_stack[2048 / sizeof(StackType_t)];
alignas(16) static StackType_t s_input_stack[8192 / sizeof(StackType_t)];
alignas(16) static StackType_t s_serial_stack[2048 / sizeof(StackType_t)];
alignas(16) static StackType_t s_monitor_stack[2560 / sizeof(StackType_t)];
static StaticTask_t s_system_tasks[4];
static StaticQueue_t s_key_queue_control;
static uint8_t s_key_queue_storage[16 * sizeof(KeyMessage)];

// 2026-09-13 修复(D5):原来 11 处 xQueueSend 全都不看返回值。队列满时
// (消费端 input_task 卡在 LVGL 锁或应用切换上)按键被静默丢弃,表现为
// "按了没反应"却查不到任何线索。集中到这一个函数:失败时限频告警,
// 一眼能看出是按键侧丢还是消费端卡住。
static void key_post(const KeyMessage &msg) {
    if (xQueueSend(s_key_queue, &msg, 0) == pdTRUE) return;
    static TickType_t s_last_warn = 0;
    const TickType_t now = xTaskGetTickCount();
    if (now - s_last_warn >= pdMS_TO_TICKS(2000)) {
        s_last_warn = now;
        ESP_LOGW(TAG, "key queue full: dropped btn=%u evt=%u", msg.button, (unsigned)msg.event);
    }
}

// 串口调试命令走同一条队列;这里允许短暂等待,避免手敲命令被瞬间突发挤掉。
static void key_post_wait(const KeyMessage &msg) {
    if (xQueueSend(s_key_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "key queue busy: dropped btn=%u evt=%u", msg.button, (unsigned)msg.event);
    }
}

static int button_from_mv(int millivolts) {
    static constexpr uint16_t ranges[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;
    for (int button = 0; button < BSP_BTN_COUNT; ++button) {
        if (millivolts >= ranges[button][0] && millivolts < ranges[button][1]) return button;
    }
    return -1;
}

static void button_task(void *arg) {
    (void)arg;
    int candidate = -1;
    int stable = -1;
    uint8_t samples = 0;
    TickType_t pressed_at = 0;
    bool long_sent = false;
    uint8_t power_stage = 0;
    uint16_t last_mv = 0;
    OkGesture ok_gesture;
    const auto emit = [](uint8_t button, app_btn_event_t event, uint16_t mv) {
        key_post({button, event, mv});
    };
    const auto gesture_post = [&](const KeyMessage &msg) {
        ok_gesture.event(msg.button, msg.event, msg.mv, esp_timer_get_time(),
                         app_registry_get_active(), emit);
    };

    // A key still held from the power-on gesture is not input.
    for (int i = 0; i < 250 && bsp_button_read_mv() < 2500; ++i) vTaskDelay(pdMS_TO_TICKS(20));

    while (true) {
        int mv = bsp_button_read_mv();
        const int current = button_from_mv(mv);
        if (current >= 0) {
            last_mv = static_cast<uint16_t>(mv);
        }

        if (current == candidate) {
            if (samples < 3) ++samples;
        } else {
            candidate = current;
            samples = 1;
        }

        if (samples >= 3 && candidate != stable) {
            const int previous = stable;
            stable = candidate;
            ESP_LOGI(TAG, "Key Transition: stable=%d (prev=%d), mv=%u", stable, previous, last_mv);
            if (stable >= 0) {
                // 按下瞬间 (PRESS)
                pressed_at = xTaskGetTickCount();
                long_sent = false;
                KeyMessage msg = {
                    .button = static_cast<uint8_t>(stable),
                    .event = BTN_EVT_PRESS,
                    .mv = last_mv
                };
                gesture_post(msg);
            } else if (previous >= 0) {
                // 松开瞬间
                if (!long_sent) {
                    KeyMessage msg_click = {
                        .button = static_cast<uint8_t>(previous),
                        .event = BTN_EVT_CLICK,
                        .mv = last_mv
                    };
                    gesture_post(msg_click);
                } else {
                    KeyMessage msg_up = {
                        .button = static_cast<uint8_t>(previous),
                        .event = BTN_EVT_LONG_UP,
                        .mv = last_mv
                    };
                    gesture_post(msg_up);
                }
                KeyMessage msg_rel = {
                    .button = static_cast<uint8_t>(previous),
                    .event = BTN_EVT_RELEASE,
                    .mv = last_mv
                };
                gesture_post(msg_rel);
                if (power_stage) key_post({kPowerCommand, BTN_EVT_CLICK, 0});
            }
            power_stage = 0;
        }

        // Hold OK: 2 s shows "keep holding", 5 s powers off. The 0.8 s long
        // press has already gone back one level by then, which is harmless.
        if (stable == BSP_BTN_OK && power_stage < 2) {
            const TickType_t held = xTaskGetTickCount() - pressed_at;
            const uint8_t stage = held >= pdMS_TO_TICKS(5000) ? 2 : held >= pdMS_TO_TICKS(2000) ? 1 : 0;
            if (stage > power_stage) {
                power_stage = stage;
                key_post({kPowerCommand, BTN_EVT_CLICK, stage});
            }
        }

        // 长按判定 (800ms)
        if (stable >= 0 && !long_sent &&
            (xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(800))) {
            KeyMessage msg_long = {
                .button = static_cast<uint8_t>(stable),
                .event = BTN_EVT_LONG,
                .mv = last_mv
            };
            gesture_post(msg_long);
            long_sent = true;
        }

        ok_gesture.poll(esp_timer_get_time(), app_registry_get_active(), emit);

        // Keep short taps detectable while the panel is off: three 50 ms
        // samples would otherwise miss a normal 80-100 ms waking press.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void input_task(void *arg) {
    (void)arg;
    KeyMessage msg;
    while (true) {
        // Keys and serial commands arrive through the queue and wake this task at
        // once. The timeout only paces the polls below: use 20 ms while Xiaozhi or
        // a voice/cooperative switch is active so MCP handovers do not wait on a
        // 100 ms tick, 100 ms for normal foreground, and 500 ms while asleep/idle.
        const bool fast = app_registry_switch_pending() || device_control::pending() ||
                          (app_registry_get_active() == APP_ID_XIAOZHI && !app_registry_background_idle());
        const bool prompt = fast || !app_registry_background_idle() || reminders::alarm_active();
        if (xQueueReceive(s_key_queue, &msg, pdMS_TO_TICKS(fast ? 20 : prompt ? 100 : 500)) == pdTRUE) {
            dispatch_input_message(msg);
        }
        device_control::poll();
        app_registry_process_pending_switch();
        reminders::poll();
        settings_tick();
    }
}

static void system_monitor_task(void *arg) {
    (void)arg;
    unsigned battery_tick = 0;
    bool scan_idle_applied = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bsp_memory_sample();
        if (++battery_tick >= 10) {
            battery_tick = 0;
            bsp_battery_poll();
            if (power_battery_tick()) key_post_wait({kPowerCommand, BTN_EVT_CLICK, 3});
        }
        wifi_setup_tick();

        const bool scan_idle = app_registry_background_idle();
        if (scan_idle != scan_idle_applied) {
            scan_idle_applied = scan_idle;
            WifiManager::GetInstance().SetScanIdle(scan_idle);
        }

        app_registry_tick();
        pomodoro_service_tick();
        if (app_registry_get_active() == APP_ID_LAUNCHER && app_registry_screen_is_on()) {
            bsp_lvgl_lock(-1);
            if (app_registry_screen_is_on()) launcher_ui_update_status();
            bsp_lvgl_unlock();
        }
    }
}

static void handle_serial_command(int c) {
    if (c == 'R') { app_registry_switch_to(APP_ID_REMINDERS); return; }
    if (c == 'N') { app_registry_switch_to(APP_ID_NETEASE); return; }
    // Diagnostic equivalent of the resolved physical double-click. It uses
    // the same wake/lifecycle/one-shot conversation path, not a direct mic call.
    if (c == 'x') { app_registry_dispatch_key(BSP_BTN_OK, BTN_EVT_DOUBLE, 600); return; }
    if (c == 'u' || c == 'U' || c == '+') {
        ESP_LOGI(TAG, "[SerialCmd] UP (+) button clicked");
        KeyMessage msg = {.button = BSP_BTN_UP, .event = BTN_EVT_CLICK, .mv = 15};
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    } else if (c == 'd' || c == 'D' || c == '-') {
        ESP_LOGI(TAG, "[SerialCmd] DOWN (-) button clicked");
        KeyMessage msg = {.button = BSP_BTN_DOWN, .event = BTN_EVT_CLICK, .mv = 1060};
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    } else if (c == 'o' || c == 'O' || c == '\r' || c == '\n') {
        ESP_LOGI(TAG, "[SerialCmd] OK button clicked");
        KeyMessage msg = {.button = BSP_BTN_OK, .event = BTN_EVT_CLICK, .mv = 2070};
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    } else if (c == 'l' || c == 'L' || c == 'b' || c == 'B') {
        ESP_LOGI(TAG, "[SerialCmd] LONG OK / Back button triggered");
        KeyMessage msg = {.button = BSP_BTN_OK, .event = BTN_EVT_LONG, .mv = 2070};
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    } else if (c == 'k' || c == 'K') {
        // 调试用:长按下键。首页的上传模式(手机配网/传头像)绑在这个手势上,
        // 没有串口映射就无法脱离真机按键验证。
        ESP_LOGI(TAG, "[SerialCmd] LONG DOWN (-) button triggered");
        KeyMessage msg = {.button = BSP_BTN_DOWN, .event = BTN_EVT_LONG, .mv = 1060};
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    } else if (c == '[' || c == ']') {
        // Diagnostic PTT pair: same event queue and handler as the physical UP key.
        ESP_LOGI(TAG, "[SerialCmd] UP %s", c == '[' ? "press" : "release");
        KeyMessage msg = {.button = BSP_BTN_UP,
            .event = c == '[' ? BTN_EVT_PRESS : BTN_EVT_RELEASE,
            .mv = static_cast<uint16_t>(c == '[' ? 15 : 2900)};
        app_registry_dispatch_key(msg.button, msg.event, msg.mv);
    } else if (c == 'p' || c == 'P') {
        // 真机 UI 截图：按横带渲染当前活动屏，把 RGB565 像素以十六进制从
        // console 吐出（主机端 _diag/capture_ui.py 收流并还原成 PNG）。
        // 必须持 LVGL 锁调用（lv_obj_redraw 要在 LVGL 上下文里跑），
        // 整个过程 ~1s，期间其它任务的 LVGL 操作会排队。
        ESP_LOGI(TAG, "[SerialCmd] screenshot requested");
        if (bsp_lvgl_lock(pdMS_TO_TICKS(2000))) {
            ui_capture_dump_hex();
            bsp_lvgl_unlock();
        } else {
            ESP_LOGW(TAG, "[SerialCmd] screenshot aborted: LVGL lock busy");
        }
    } else if (c == 'v') {
        bsp_battery_poll();
        bsp_battery_log_status();
        app_registry_log_display_state();
        ui_sfx_log_diagnostics();
        bsp_memory_dump();
        bsp_memory_dump_recent_failures();
        ESP_LOGI(TAG, "Input stacks: input_free=%u serial_free=%u",
            (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("input_disp")),
            (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("ser_cmd")));
        esp_pm_config_t config{};
        if (esp_pm_get_configuration(&config) == ESP_OK)
            ESP_LOGI(TAG, "Power: min=%d max=%d auto_sleep=%d screen_on=%d free_heap=%u largest=%u minimum=%u heap_ok=%d",
                config.min_freq_mhz, config.max_freq_mhz, config.light_sleep_enable,
                app_registry_screen_is_on(), (unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)esp_get_minimum_free_heap_size(),
                heap_caps_check_integrity_all(true));
    } else if (c == 'h') {
        // 诊断版专用:逐块打印内部堆及其所属任务,用于比对应用退出前后的残留。
        bsp_memory_map();
    } else if (c == 'j') {
        app_registry_switch_to(APP_ID_SOUND_JUMP);
    } else if (c == 't') {
        app_registry_switch_to(APP_ID_POMODORO);
    } else if (c == 'm') {
        app_registry_switch_to(APP_ID_MUSE);
    } else if (c == 'M') {
        if (app_registry_get_active() == APP_ID_MUSE) passport_muse_diagnostic_ble_cycle();
    } else if (c == '?') {
        passport_muse_log_diagnostics();
    } else if (c >= '1' && c <= '9') {
        int target_id = c - '0';
        ESP_LOGI(TAG, "[SerialCmd] Direct switch to App ID %d", target_id);
        app_registry_switch_to(static_cast<passport_app_id_t>(target_id));
    }
}

static void serial_cmd_task(void *arg) {
    (void)arg;
    while (true) {
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (c <= 0) continue;
            // A single FIFO orders physical keys and diagnostic commands.
            KeyMessage msg = {.button = kSerialCommand, .event = BTN_EVT_CLICK,
                              .mv = static_cast<uint16_t>(c)};
            key_post_wait(msg);
        }
        // The console is non-blocking: every poll is a wake-up. Drain the buffer
        // above, then poll at 4 Hz while the screen is off and nothing streams.
        vTaskDelay(pdMS_TO_TICKS(app_registry_background_idle() ? 250 : 50));
    }
}

extern "C" void app_main(void) {
    power_boot_check();    // 注:上电瞬间的白屏由 bootloader_components/bl_early 在二级 bootloader 里
    bsp_memory_init();
    // 就把 GPIO21 拉低解决(已实测 out=0/oe=1 一路保持到 app_main)。
    // 详见该目录下的注释。
    // 最早时刻把背光脚拉成确定低电平:上电到 LEDC 初始化之间该脚悬空,
    // 背光电路会被"点着",而面板此时是未写入的白屏 —— 这就是开机白闪。
    // (第二级 bootloader 阶段仍无法覆盖,那一小段由硬件上拉/下拉决定。)
    {
        gpio_config_t bl = {
            .pin_bit_mask = 1ULL << BSP_LCD_BL,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl);
        gpio_set_level((gpio_num_t)BSP_LCD_BL, 0);
        // Power off holds this pin low through deep sleep; LEDC needs it back.
        gpio_hold_dis((gpio_num_t)BSP_LCD_BL);
    }

    ESP_LOGI(TAG, "=== FoloToy AI Passport OS Starting ===");
    const esp_pm_config_t pm_config = { .max_freq_mhz = 160, .min_freq_mhz = 40,
                                        .light_sleep_enable = true };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Power management: DFS 40-160MHz, automatic light sleep enabled");

    // 1. NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    reminders::initialize();
    lists::initialize();

    // 头像/名片分区在后台挂载;残留其他固件时在此格式化(十余秒),
    // 不让手机上传请求承担这段耗时。堆此时最完整,SPIFFS 缓冲也放在低地址。
    xTaskCreate([](void *) {
        home_profile_storage_prepare();
        vTaskDelete(nullptr);
    }, "storage_prep", 3584, nullptr, tskIDLE_PRIORITY + 1, nullptr);

    // 2. 硬件总线与外设初始化
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_display_init());
    if (bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "LVGL initialization failed");
        return;
    }
    settings_bootstrap();
    // 背光不在这里点亮:此时面板 GRAM 尚未写入任何帧,亮背光会闪白屏。
    // 背光由 boot_splash 在首帧渲染完成后渐亮(见 boot_splash.c)。

    bsp_battery_init();
    // 仅初始化 ADC 采样硬件，不启动有死锁风险的 timer 回调
    ESP_ERROR_CHECK(bsp_button_init(nullptr, nullptr));

    // 3. 初始化 Launcher 与应用注册表 (仅载入启动器，不预加载子 App)
    app_registry_init();
    // 磷光开机动画(~0.9s,与开机音同步);不再提前初始化/加载 Launcher ——
    // 旧路径会先闪一两帧应用页再跳首页。抽屉首次进入时才会自建界面。
    boot_splash_run();


    // 4. 初始化全局 Wi-Fi 管理器；若已存有配置则在后台自动拉起 Station 连接
    WifiManagerConfig wifi_cfg = {};
    wifi_cfg.ssid_prefix = "Passport-WiFi";
    wifi_cfg.language = "zh-CN";
    WifiManager::GetInstance().Initialize(wifi_cfg);
    // 待机省电默认档:MAX modem sleep(首页/桌面/设置等非流媒体前台)。
    // 进 radio/voice/xiaozhi 时由 app_registry 切回 BALANCED,上传模式由首页恢复。
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);

    // 为整理堆而软重启时(见 app_registry 的 clean launch),直接进入目标应用,
    // 不先构建首页,也不播开机音:否则又会在新堆上留下首页与音频的碎片。
    const passport_app_id_t clean_target = app_registry_take_clean_launch();
    timers_restore_after_restart();
    bool chime = true;
    if (!SsidManager::GetInstance().GetSsidList().empty()) {
        ESP_LOGI(TAG, "Stored Wi-Fi found, starting station auto-connect in background...");
        WifiManager::GetInstance().StartStation();
        if (clean_target != APP_ID_COUNT) {
            ESP_LOGI(TAG, "Clean launch into app %d", (int)clean_target);
            chime = false;
        }
        app_registry_switch_to(clean_target != APP_ID_COUNT ? clean_target : APP_ID_HOME);
    } else {
        ESP_LOGI(TAG, "First boot: starting phone setup");
        wifi_setup_require_initial();
        app_registry_switch_to(APP_ID_WIFI);
    }

    boot_splash_end();
    // Reserve the codec before accepting keys; a fast first app cannot have
    // its I2S handles torn down by the chime's late cleanup.
    if (chime) boot_chime_start();

    // 5. 创建按键事件队列与输入分发任务 (系统就绪后正式接收用户输入)
    //    input_disp 保留 8192，串口命令也由它执行 app_registry_switch_to()；
    //    应用启动里最深的是 Home 天气图标 JPEG 解码 —— LVGL TJPEG
    //    decoder_info(文件源)在栈上放 4KB 工作缓冲(lv_tjpgd.c:110),
    //    3584/4096 栈实测爆栈(Stack protection fault,2026-09-13)。
    s_key_queue = xQueueCreateStatic(16, sizeof(KeyMessage), s_key_queue_storage, &s_key_queue_control);
    xTaskCreateStatic(button_task, "btn_poll", sizeof(s_button_stack), NULL, 6,
                      s_button_stack, &s_system_tasks[0]);
    xTaskCreateStatic(input_task, "input_disp", sizeof(s_input_stack), NULL, 5,
                      s_input_stack, &s_system_tasks[1]);
    xTaskCreateStatic(serial_cmd_task, "ser_cmd", sizeof(s_serial_stack), NULL, 4,
                      s_serial_stack, &s_system_tasks[2]);

    // 6. 启动系统监控心跳任务 (状态栏与统一息屏管理)
    xTaskCreateStatic(system_monitor_task, "sys_mon", sizeof(s_monitor_stack), NULL, 1,
                      s_monitor_stack, &s_system_tasks[3]);

    ESP_LOGI(TAG, "Passport OS Boot Complete. Initial Free Heap: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
}
