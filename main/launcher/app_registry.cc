#include "launcher/app_registry.h"
#include "launcher/timer_resume.h"
#include "bsp_memory.h"
#include "bsp_memory.h"
#include "lwip/sockets.h"
#include "launcher/launcher.h"
#include "launcher/wifi_setup.h"
#include "launcher/wake_gate.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "app_fonts.h"
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "boot_chime.h"
#include "ui_sfx.h"
#include "apps/reminders/reminder_service.h"
#include "apps/xiaozhi/app_xiaozhi.h"
#include "nvs.h"
#include <atomic>
#include <vector>

static const char *TAG = "app_registry";

// 外部 App 实例前向声明
extern const passport_app_t g_radio_app;
extern const passport_app_t g_voice_app;
extern const passport_app_t g_xiaozhi_app;
extern const passport_app_t g_wifi_app;
extern const passport_app_t g_settings_app;
extern const passport_app_t g_home_app;
extern const passport_app_t g_game_app;
extern const passport_app_t g_tetris_app;
extern const passport_app_t g_stopwatch_app;
extern const passport_app_t g_sound_jump_app;
extern const passport_app_t g_muse_app;
extern const passport_app_t g_pomodoro_app;
extern const passport_app_t g_netease_app;
extern const passport_app_t g_reminders_app;
extern const passport_app_t g_lists_app;

namespace {

// 链接期插件表:所有应用只在这里登记一次,行为差异写在各自的 passport_app_t
// 元数据里。新增/隐藏应用不需要改注册表、设置页或设备控制的任何 switch。
// 首页与配网是系统底座,标记为不出现在应用列表。
const passport_app_t *s_apps[] = {
    &g_xiaozhi_app,
    &g_radio_app,
    &g_netease_app,
    &g_voice_app,
    &g_muse_app,
    &g_pomodoro_app,
    &g_stopwatch_app,
    &g_reminders_app,
    &g_lists_app,
    &g_tetris_app,
    &g_game_app,
    &g_sound_jump_app,
    &g_settings_app,   // 设置放最后(应用惯用收尾位置)
    &g_home_app,
    &g_wifi_app,
};

constexpr size_t kAppCount = sizeof(s_apps) / sizeof(s_apps[0]);
constexpr char kFlagsNamespace[] = "app_flags";

std::atomic<passport_app_id_t> s_active_id{APP_ID_LAUNCHER};
wake_gate_t s_wake_gate{};
bool s_initialized[APP_ID_COUNT] = {};
bool s_enabled[APP_ID_COUNT] = {};

const passport_app_t *find_app(passport_app_id_t id) {
    for (const passport_app_t *app : s_apps) {
        if (app && app->id == id) return app;
    }
    return nullptr;
}

bool initial_enabled(const passport_app_t *app) {
    return app && (!app->toggleable || app->default_enabled);
}

void load_enabled() {
    nvs_handle_t handle = 0;
    const bool stored = nvs_open(kFlagsNamespace, NVS_READONLY, &handle) == ESP_OK;
    for (const passport_app_t *app : s_apps) {
        if (!app || app->id >= APP_ID_COUNT) continue;
        bool enabled = initial_enabled(app);
        if (app->toggleable && app->key && app->key[0] && stored) {
            uint8_t value = 0;
            if (nvs_get_u8(handle, app->key, &value) == ESP_OK) enabled = value != 0;
        }
        s_enabled[app->id] = enabled;
    }
    if (stored) nvs_close(handle);
    s_enabled[APP_ID_LAUNCHER] = true;
}

bool store_enabled(const passport_app_t *app, bool enabled) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kFlagsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, app->key, enabled ? 1 : 0);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save %s: %d", app->key, (int)result);
        return false;
    }
    return true;
}

bool feedback_allowed(passport_app_id_t id) {
    if (id == APP_ID_LAUNCHER) return true;
    const passport_app_t *app = find_app(id);
    return app && app->allow_toast;
}

// 2026-09-13:switch_to() 会被三个不同任务调用(input_task 按键/长按、
// main.cc 的串口命令任务、wifi_setup 的 connect_worker),而它内部是
// "stop 旧 App → start 新 App" 的多步序列 —— 并发执行会出现双 start、
// 双建任务、s_active_id 撕裂。这里用互斥量把整个切换串行化。
// 取锁用 3s 超时而不是死等:宁可丢一次切换,也不能把调用方永久挂住
// (参考 A2:C3 上任何无上界等待最终都会变成"按键没反应")。
SemaphoreHandle_t s_switch_mutex = nullptr;

struct SwitchGuard {
    bool owned = false;
    bool try_take(TickType_t wait = pdMS_TO_TICKS(3000)) {
        if (!s_switch_mutex) return false;
        owned = xSemaphoreTakeRecursive(s_switch_mutex, wait) == pdTRUE;
        return owned;
    }
    ~SwitchGuard() {
        if (owned && s_switch_mutex) xSemaphoreGiveRecursive(s_switch_mutex);
    }
};

// 按前台应用选择 Wi-Fi 省电档:流媒体/语音需要即时下行,保持 MIN modem sleep;
// 桌面/首页/设置等待机前台放宽到 MAX modem sleep(下行延迟可到 ~1s,对
// 天气/SNTP/上传服务无感),是待机电流的最大单项优化。
WifiPowerSaveLevel power_save_level_for(passport_app_id_t id) {
    const passport_app_t *app = find_app(id);
    return app && app->streaming ? WifiPowerSaveLevel::BALANCED
                                 : WifiPowerSaveLevel::LOW_POWER;
}

// 收音机/小智/Muse 的 TLS、解码与音频缓冲需要大块连续内存。C3 无 PSRAM,
// 这些应用退出后残留的小块(lwIP、Wi-Fi、启动器界面)会把堆切碎,再次进入时
// 总量够而最大连续块不够,表现为连接失败或运行中崩溃。碎片无法原地整理:
// 只有当前堆确实放不下目标应用,或上一次重型会话已出现过分配失败时,
// 才记下目标后软重启,开机直接进入该应用。
constexpr uint32_t kCleanLaunchMagic = 0x434c4e31; // "CLN1"
constexpr uint32_t kCleanTalkMagic = 0x54414c4b; // "TALK", same two RTC words
RTC_NOINIT_ATTR uint32_t s_clean_launch[2];
bool s_start_xiaozhi_conversation = false; // lifecycle mutex owns this intent

struct HeapNeed {
    size_t largest; // 单块连续需求加余量
    size_t free;    // 整个会话的总占用加余量
};
// rc.51 实测(堆追踪诊断版):收音机 MP3 解码器一次申请约 22.9 KiB,会话约占 34 KiB;
// 小智最大单块 12 KiB(任务栈);2026-11-07 实测(设备工具、提醒、清单后)会话峰值约 58 KiB,
// 会话内音乐搜索另有自身预检(24 KiB);Muse 工作任务栈 12 KiB 加 TLS 8 KiB。
HeapNeed heap_need(passport_app_id_t id) {
    const passport_app_t *app = find_app(id);
    if (!app) return {0, 0};
    return {static_cast<size_t>(app->heap_largest_kb) * 1024,
            static_cast<size_t>(app->heap_free_kb) * 1024};
}

// 进入重型应用时记下分配失败计数;离开时若计数增加,说明该会话已在缺内存
// 状态下挣扎过,下一次进入重型应用直接换一个干净的堆。
unsigned s_failures_at_entry = 0;
bool s_heap_strained = false;
// 刚为某应用重启过:堆已是最干净的状态,即使仍不达标也不再重启,避免循环。
bool s_fresh_heap = false;

bool heavy_app(passport_app_id_t id) {
    const HeapNeed need = heap_need(id);
    return need.largest > 0 || need.free > 0;
}

// 在旧应用 stop() 之后调用:此时测到的才是新应用可用的堆。
bool needs_clean_launch(passport_app_id_t id, size_t largest, size_t free) {
    if (!heavy_app(id)) return false;
    const HeapNeed need = heap_need(id);
    return s_heap_strained || largest < need.largest || free < need.free;
}

void show_clean_launch_notice(passport_app_id_t id);

[[noreturn]] void clean_launch(passport_app_id_t id) {
    const uint32_t magic = id == APP_ID_XIAOZHI && s_start_xiaozhi_conversation
        ? kCleanTalkMagic : kCleanLaunchMagic;
    s_clean_launch[0] = magic;
    s_clean_launch[1] = static_cast<uint32_t>(id) ^ magic;
    ESP_LOGW(TAG, "Restarting for a defragmented heap: target app=%d", (int)id);
    show_clean_launch_notice(id);
#ifdef ESP_PLATFORM
    timers_save_for_restart();
#endif
    esp_restart();
}

// 息屏与背光管理
std::atomic<bool> s_screen_on{true};
std::atomic<passport_app_id_t> s_pending_target{APP_ID_COUNT};
esp_pm_lock_handle_t s_display_awake_lock = nullptr;
std::atomic<int64_t> s_last_activity_us{0};
std::atomic<uint16_t> s_screen_timeout_seconds{60};

// 长按 OK 强制返回标志
bool s_ok_long_triggered = false;

// Keep the last image in the panel while releasing the old LVGL tree. This
// empty screen exists only to keep LVGL's active pointer valid; never draw it.
lv_obj_t *s_handoff_screen = nullptr;
std::atomic<bool> s_building_target{false};
uint32_t s_handoff_count = 0;
uint32_t s_handoff_flushes = 0;
uint32_t s_panel_flushes = 0;
// One-shot timing of the first visible frame after each switch (serial log).
std::atomic<int64_t> s_switch_started_us{0};
std::atomic<int> s_first_frame_stage{0}; // 0 idle, 1 waiting first flush, 2 waiting refresh end

void resume_target_refresh(lv_display_t *display) {
    if (!s_screen_on.load() || s_building_target.load() ||
        (s_handoff_screen && lv_screen_active() == s_handoff_screen)) return;
    if (auto *timer = lv_display_get_refr_timer(display)) {
        // A full invalidation may already be cached while the holder is active.
        // Re-invalidating then emits no REFR_REQUEST, so explicitly resume.
        lv_timer_resume(timer);
        lv_timer_ready(timer);
    }
}

void display_refresh_guard(lv_event_t *event) {
    auto *display = static_cast<lv_display_t *>(lv_event_get_target(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_REFR_REQUEST) {
        // LVGL's default REFR_REQUEST handler resumes this timer on every
        // invalidation. Pausing it once is insufficient; keep holding until a
        // real target is loaded, including targets built by another task.
        if (!s_screen_on.load() || s_building_target.load() ||
            (s_handoff_screen && lv_screen_active() == s_handoff_screen)) {
            if (auto *timer = lv_display_get_refr_timer(display)) lv_timer_pause(timer);
        }
    } else if (code == LV_EVENT_SCREEN_LOADED && !s_building_target.load() &&
               lv_screen_active() != s_handoff_screen) {
        // Async destinations can arrive after switch_to() has returned.
        resume_target_refresh(display);
    } else if (code == LV_EVENT_FLUSH_START) {
        ++s_panel_flushes;
        if (s_handoff_screen && lv_screen_active() == s_handoff_screen) {
            ++s_handoff_flushes;
            ESP_LOGE(TAG, "Unexpected empty handoff flush");
        } else if (s_first_frame_stage == 1) {
            s_first_frame_stage = 2;
            ESP_LOGI(TAG, "First target flush: +%lld ms",
                     (long long)((esp_timer_get_time() - s_switch_started_us) / 1000));
        }
    } else if (code == LV_EVENT_REFR_READY && s_first_frame_stage == 2) {
        s_first_frame_stage = 0;
        ESP_LOGI(TAG, "First target frame drawn: +%lld ms",
                 (long long)((esp_timer_get_time() - s_switch_started_us) / 1000));
    }
}

bool begin_screen_handoff() {
    if (!bsp_lvgl_lock(500)) return false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    if (screen) {
        s_building_target = true;
        auto *old_handoff = s_handoff_screen;
        s_handoff_screen = screen;
        lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        if (auto *timer = lv_display_get_refr_timer(nullptr)) lv_timer_pause(timer);
        lv_screen_load(screen);
        if (old_handoff) lv_obj_delete(old_handoff);
        ++s_handoff_count;
    }
    bsp_lvgl_unlock();
    return screen != nullptr;
}

void finish_screen_handoff() {
    // Arm here: an old-app frame can still be in flight right after handoff.
    s_first_frame_stage = 1;
    s_building_target = false;
    if (!bsp_lvgl_lock(500)) return;
    if (s_handoff_screen && lv_screen_active() != s_handoff_screen) {
        lv_obj_delete(s_handoff_screen);
        s_handoff_screen = nullptr;
        // Request a full target frame on the LVGL task, never render from the
        // input task or during the old application's slow network teardown.
        lv_obj_invalidate(lv_screen_active());
        resume_target_refresh(lv_display_get_default());
    } else {
        ESP_LOGI(TAG, "Keeping panel image until asynchronous target is loaded");
    }
    bsp_lvgl_unlock();
}

// Without this the panel freezes on the old app and then goes black, which
// reads as a crash. Draw one plain frame on the LVGL task and give the DMA
// flush time to land before the restart. The target is already recorded, so
// even a failure here still ends in the intended clean launch.
void show_clean_launch_notice(passport_app_id_t id) {
    if (!s_screen_on.load() || !bsp_lvgl_lock(500)) return;
    const passport_app_t *app = app_registry_get_by_id(id);
    s_building_target = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    if (screen) {
        lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        char text[64];
        snprintf(text, sizeof(text), "正在整理内存\n马上打开 %s", app ? app->name : "应用");
        lv_obj_t *label = ui_pixel_label(screen, text, &buddy_font_16, UI_TEXT);
        lv_obj_set_width(label, 216);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, 6, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lv_screen_load(screen);
        if (s_handoff_screen) { lv_obj_delete(s_handoff_screen); s_handoff_screen = nullptr; }
        if (auto *timer = lv_display_get_refr_timer(nullptr)) { lv_timer_resume(timer); lv_timer_ready(timer); }
    }
    bsp_lvgl_unlock();
    if (screen) vTaskDelay(pdMS_TO_TICKS(400));
}

} // namespace

void app_registry_init(void) {
    load_enabled();
    ESP_LOGI(TAG, "Initializing app registry with %u plugins, %u visible...",
             (unsigned)kAppCount, (unsigned)app_registry_get_count());
    if (!s_switch_mutex) s_switch_mutex = xSemaphoreCreateRecursiveMutex();
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display_on", &s_display_awake_lock) == ESP_OK)
        esp_pm_lock_acquire(s_display_awake_lock);
    bsp_lvgl_lock(-1);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_REFR_REQUEST, nullptr);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_REFR_READY, nullptr);
    bsp_lvgl_unlock();
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
    s_active_id = APP_ID_LAUNCHER;
    // 不在这里初始化/加载 Launcher 界面:开机直接由 main 走开机动画后进首页,
    // 提前加载会让应用页在开机时闪现一两帧。抽屉进入时 launcher_ui_show() 自建。
}

size_t app_registry_get_count(void) {
    size_t count = 0;
    for (const passport_app_t *app : s_apps) {
        if (app && app->in_launcher && s_enabled[app->id]) ++count;
    }
    return count;
}

const passport_app_t *app_registry_get_by_index(size_t index) {
    for (const passport_app_t *app : s_apps) {
        if (!app || !app->in_launcher || !s_enabled[app->id]) continue;
        if (index == 0) return app;
        --index;
    }
    return nullptr;
}

const passport_app_t *app_registry_get_by_id(passport_app_id_t id) {
    return find_app(id);
}

bool app_registry_enabled(passport_app_id_t id) {
    if (id == APP_ID_LAUNCHER) return true;
    if (id <= APP_ID_LAUNCHER || id >= APP_ID_COUNT) return false;
    return s_enabled[id];
}

bool app_registry_set_enabled(passport_app_id_t id, bool enabled) {
    const passport_app_t *app = find_app(id);
    if (!app || !app->toggleable || !app->key || !app->key[0]) return false;
    if (!enabled && s_active_id.load() == id) return false;
    if (!store_enabled(app, enabled)) return false;
    s_enabled[id] = enabled;
    ESP_LOGI(TAG, "%s: %s", app->key, enabled ? "enabled" : "disabled");
    return true;
}

bool app_registry_reset_toggles(void) {
    bool saved = true;
    for (const passport_app_t *app : s_apps) {
        if (!app || !app->toggleable || !app->key || !app->key[0]) continue;
        if (store_enabled(app, app->default_enabled)) {
            s_enabled[app->id] = app->default_enabled;
        } else {
            saved = false;
        }
    }
    return saved;
}

size_t app_registry_toggle_count(void) {
    size_t count = 0;
    for (const passport_app_t *app : s_apps) {
        if (app && app->toggleable && app->key && app->key[0]) ++count;
    }
    return count;
}

const passport_app_t *app_registry_get_toggleable(size_t index) {
    for (const passport_app_t *app : s_apps) {
        if (!app || !app->toggleable || !app->key || !app->key[0]) continue;
        if (index == 0) return app;
        --index;
    }
    return nullptr;
}

size_t app_registry_openable_count(void) {
    size_t count = 0;
    for (const passport_app_t *app : s_apps) {
        if (app && app->openable && s_enabled[app->id]) ++count;
    }
    return count;
}

const passport_app_t *app_registry_get_openable(size_t index) {
    for (const passport_app_t *app : s_apps) {
        if (!app || !app->openable || !s_enabled[app->id]) continue;
        if (index == 0) return app;
        --index;
    }
    return nullptr;
}

const char *app_registry_key(passport_app_id_t id) {
    if (id == APP_ID_LAUNCHER) return "launcher";
    const passport_app_t *app = find_app(id);
    return app && app->key && app->key[0] ? app->key : "unknown";
}

passport_app_id_t app_registry_take_clean_launch(void) {
    const uint32_t magic = s_clean_launch[0];
    const auto id = static_cast<passport_app_id_t>(s_clean_launch[1] ^ magic);
    s_clean_launch[0] = s_clean_launch[1] = 0; // 一次性:目标应用崩溃时下次开机回首页
    if ((magic != kCleanLaunchMagic && magic != kCleanTalkMagic) ||
        esp_reset_reason() != ESP_RST_SW || !heavy_app(id) ||
        !app_registry_enabled(id) ||
        (magic == kCleanTalkMagic && id != APP_ID_XIAOZHI))
        return APP_ID_COUNT;
    s_start_xiaozhi_conversation = magic == kCleanTalkMagic;
    s_fresh_heap = true;
    return id;
}

passport_app_id_t app_registry_get_active(void) {
    return s_active_id;
}

bool app_registry_switch_pending(void) {
    return s_pending_target.load() != APP_ID_COUNT || s_building_target.load();
}

void app_registry_switch_to(passport_app_id_t id) {
    // 串行化:见 s_switch_mutex 注释。取不到锁直接放弃本次切换并留日志,
    // 不做无上界等待。
    SwitchGuard guard;
    if (!guard.try_take()) {
        ESP_LOGE(TAG, "switch_to(%d): registry busy; switch dropped", (int)id);
        return;
    }

    // Another caller may have completed this same switch while we waited.
    // A request for the current app can arrive while it is already stopping
    // for a different destination. Honour that latest request by restarting it
    // after shutdown, rather than silently navigating to the abandoned target.
    if (s_active_id == id && s_pending_target == APP_ID_COUNT) return;

    if (id != APP_ID_WIFI && !wifi_setup_can_leave()) return;

    // Reject unknown or disabled destinations before touching the current screen.
    const passport_app_t *next = app_registry_get_by_id(id);
    if (id != APP_ID_LAUNCHER && (next == nullptr || !app_registry_enabled(id))) {
        ESP_LOGE(TAG, "switch_to(%d): app is missing or disabled; switch ignored", (int)id);
        return;
    }

    if (id != APP_ID_XIAOZHI) s_start_xiaozhi_conversation = false;
    const passport_app_t *current = app_registry_get_by_id(s_active_id);
    if (boot_chime_busy() || (!feedback_allowed(id) && !ui_sfx_prepare_stop()) ||
        (current && current->prepare_stop && !current->prepare_stop())) {
        if (s_pending_target.exchange(id) == APP_ID_COUNT)
            ESP_LOGI(TAG, "Deferring switch until current worker releases resources");
        return;
    }
    ESP_LOGI(TAG, "Switching app: %d -> %d", (int)s_active_id.load(), id);

    // Free old UI safely without replacing the physical panel image with a
    // loading/return page. The real destination supplies the next visible frame.
    const int64_t switch_started_us = esp_timer_get_time();
    s_switch_started_us = switch_started_us;
    if (!begin_screen_handoff()) {
        s_pending_target = id;
        ESP_LOGE(TAG, "Unable to prepare screen handoff; app switch cancelled safely");
        return;
    }
    s_pending_target = APP_ID_COUNT;

    // 1. 退出前一个 App 并清理资源
    if (s_active_id != APP_ID_LAUNCHER) {
        const passport_app_t *prev = app_registry_get_by_id(s_active_id);
        if (prev && prev->stop) {
            ESP_LOGI(TAG, "Stopping app: %s", prev->name);
            prev->stop();
        }
    } else {
        launcher_ui_hide();
    }

    // Socket calls made by this long-lived caller lazily create a thread-local
    // semaphore. Once synchronous teardown has returned, no call is waiting on
    // it. Release it here so a tiny allocation cannot pin a session-sized hole.
    // Each background network task owns its own semaphore independently.
    lwip_socket_thread_cleanup();

    // 2. 打印当前可用空闲内存，确认清理效果
    uint32_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap after app stop: %u KB largest=%u minimum=%u",
             (unsigned)(free_heap / 1024),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)esp_get_minimum_free_heap_size());

    if (heavy_app(s_active_id) && bsp_memory_failures() != s_failures_at_entry) {
        s_heap_strained = true;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (!s_fresh_heap && needs_clean_launch(id, largest, free_heap)) clean_launch(id);
    s_fresh_heap = false;
    if (heavy_app(id)) s_failures_at_entry = bsp_memory_failures();

    bsp_memory_phase(next ? next->name : "launcher");
    s_active_id = id;
    // manager 未初始化时只记忆档位,StartStation 后自动生效(见 wifi_manager)
    WifiManager::GetInstance().SetPowerSaveLevel(power_save_level_for(id));

    // 3. 启动新目标
    if (id == APP_ID_LAUNCHER) {
        launcher_ui_show();
    } else if (next) {
        ESP_LOGI(TAG, "Starting app: %s", next->name);
        if (next->init && !s_initialized[id]) {
            next->init();
            s_initialized[id] = true;
        }
        if (next->start) next->start();
    }
    if (id == APP_ID_XIAOZHI && s_start_xiaozhi_conversation) {
        s_start_xiaozhi_conversation = false;
        xiaozhi_app_request_conversation();
    }
    finish_screen_handoff();
    lwip_socket_thread_cleanup();
    ESP_LOGI(TAG, "Target ready: app=%d elapsed_ms=%lld free=%u", id,
             (long long)((esp_timer_get_time() - switch_started_us) / 1000),
             (unsigned)esp_get_free_heap_size());
}

void app_registry_process_pending_switch(void) {
    SwitchGuard guard;
    if (!guard.try_take()) return;
    const auto target = s_pending_target.load();
    if (target != APP_ID_COUNT) app_registry_switch_to(target);
}

void app_registry_complete_setup(void) {
    SwitchGuard guard;
    if (!guard.try_take()) return;
    if (s_active_id == APP_ID_WIFI && s_pending_target == APP_ID_COUNT)
        app_registry_switch_to(APP_ID_HOME);
}

void app_registry_dispatch_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    // Hold the same lifecycle lock as stop/start while calling app->on_key.
    // Recursive because a key handler may itself request an app switch.
    SwitchGuard guard;
    if (!guard.try_take()) return;
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "Dispatch Key: btn=%d, evt=%d, mv=%u, cur_active=%d", btn, event, mv, (int)s_active_id.load());

    // DOUBLE is already resolved from two physical short presses. It may
    // include the waking press and must not be swallowed with its tail.
    const bool xiaozhi_shortcut = btn == BSP_BTN_OK && event == BTN_EVT_DOUBLE;
    if (xiaozhi_shortcut) s_wake_gate = {};
    if (wake_gate_consume(&s_wake_gate, event == BTN_EVT_PRESS)) return;

    // 1. 熄屏唤醒护栏：如果屏幕已休眠，第一按只点亮屏幕，不透传给业务
    if (!s_screen_on.load(std::memory_order_acquire)) {
        if (event == BTN_EVT_PRESS || event == BTN_EVT_CLICK || xiaozhi_shortcut) {
            // 先唤醒面板(SLPOUT+DISPON,内部含 120ms 稳定等待),再开背光,
            // 恢复 LVGL 刷新并补一次整屏重绘(睡眠期间的刷屏被面板丢弃过)。
            wake_gate_arm(&s_wake_gate);
            if (!bsp_lvgl_lock(500)) return;
            if (s_display_awake_lock) esp_pm_lock_acquire(s_display_awake_lock);
            bsp_display_power(true);
            bsp_display_backlight(100);
            s_screen_on.store(true, std::memory_order_release);
            s_last_activity_us.store(now, std::memory_order_release);
            {
                resume_target_refresh(lv_display_get_default());
                lv_obj_t *scr = lv_screen_active();
                if (scr) lv_obj_invalidate(scr);
                bsp_lvgl_unlock();
            }
            ESP_LOGI(TAG, "Screen woken up by key press");
            if (!xiaozhi_shortcut) return;
            s_wake_gate = {};
        }
    }

    s_last_activity_us.store(now, std::memory_order_release);

    if (xiaozhi_shortcut) {
        s_ok_long_triggered = false;
        reminders::dismiss_for_navigation();
        // Already in Xiaozhi: request listening without toggling an active
        // conversation. A pending exit still goes through orderly teardown.
        if (s_active_id == APP_ID_XIAOZHI && s_pending_target == APP_ID_COUNT) {
            xiaozhi_app_request_conversation();
            return;
        }
        // Keep the request even while phone setup is committing data. The
        // input task retries through the same cooperative lifecycle gate.
        s_start_xiaozhi_conversation = true;
        s_pending_target = APP_ID_XIAOZHI;
        app_registry_switch_to(APP_ID_XIAOZHI);
        return;
    }

    if (reminders::intercept_key(btn,event)) return;
    const auto source = s_active_id.load();
    auto feedback = [&](ui_sfx_kind_t kind) {
        if (s_pending_target == APP_ID_COUNT && !boot_chime_busy() &&
            feedback_allowed(source) && feedback_allowed(s_active_id.load())) ui_sfx_play(kind);
    };

    // 2. 全局拦截:长按 OK = 整机统一的"返回",沿层级上移:
    //    子应用 → 应用板块 → 首页。首页已是顶层,长按无动作。
    //    Apps with internal pages may consume Back before the app-level exit.
    if (btn == BSP_BTN_OK && event == BTN_EVT_LONG) {
        const passport_app_t *app = app_registry_get_by_id(source);
        if (app && app->on_back) {
            s_ok_long_triggered = true;
            if (s_pending_target.load() != APP_ID_COUNT) return;
            if (app->on_back()) {
                feedback(UI_SFX_BACK);
                return;
            }
        }
        if (s_active_id == APP_ID_WIFI) {
            s_ok_long_triggered = true;
            if (wifi_setup_can_leave()) {
                feedback(UI_SFX_BACK);
                app_registry_switch_to(APP_ID_HOME);
            }
            return;
        }
        if (s_active_id == APP_ID_LAUNCHER) {
            ESP_LOGI(TAG, "Back: Launcher -> Home");
            s_ok_long_triggered = true;
            feedback(UI_SFX_BACK);
            app_registry_switch_to(APP_ID_HOME);
            return;
        }
        if (s_active_id != APP_ID_HOME) {
            ESP_LOGI(TAG, "Back: App -> Launcher");
            s_ok_long_triggered = true;
            feedback(UI_SFX_BACK);
            app_registry_switch_to(APP_ID_LAUNCHER);
            return;
        }
        // 首页是顶层:不拦不吞,首页 on_key 本身不消费长按 OK。
    }
    if (btn == BSP_BTN_OK && (event == BTN_EVT_RELEASE || event == BTN_EVT_LONG_UP)) {
        if (s_ok_long_triggered) {
            s_ok_long_triggered = false;
            return;
        }
    }

    // The old worker is already cancelling. Do not start another operation in
    // it while the input task waits for its cooperative shutdown.
    if (s_pending_target.load() != APP_ID_COUNT) return;

    const bool click = (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) && event == BTN_EVT_CLICK;
    const auto kind = btn == BSP_BTN_OK ? UI_SFX_CONFIRM : UI_SFX_NAV;
    // Home can defer navigation while its weather worker closes. Acknowledge
    // the gesture now; settings must first apply mute and the feedback toggle.
    if (click && source != APP_ID_SETTINGS) feedback(kind);
    if (source == APP_ID_HOME && event == BTN_EVT_LONG && btn != BSP_BTN_OK)
        feedback(UI_SFX_CONFIRM);

    // 3. 事件分发
    if (s_active_id == APP_ID_LAUNCHER) {
        launcher_ui_on_key(btn, event);
    } else {
        const passport_app_t *app = app_registry_get_by_id(s_active_id);
        if (app && app->on_key) {
            app->on_key(btn, event, mv);
        }
    }
    if (click && source == APP_ID_SETTINGS) feedback(kind);
}

void app_registry_tick(void) {
    if (bsp_lvgl_lock(50)) {
        if (!s_building_target.load() && s_handoff_screen && lv_screen_active() != s_handoff_screen) {
            lv_obj_delete(s_handoff_screen);
            s_handoff_screen = nullptr;
            resume_target_refresh(lv_display_get_default());
        }
        bsp_lvgl_unlock();
    }

    const uint16_t timeout = s_screen_timeout_seconds.load(std::memory_order_acquire);
    if (!timeout || !s_screen_on.load(std::memory_order_acquire)) return;

    const int64_t now = esp_timer_get_time();
    const int64_t last = s_last_activity_us.load(std::memory_order_acquire);
    const int64_t timeout_us = static_cast<int64_t>(timeout) * 1000000LL;
    if (now - last >= timeout_us) {
        // 持 LVGL 锁做息屏:与刷屏事务串行,避免 DISPOFF/SLPIN 插进 DMA 传输中间;
        // 睡眠期间顺手暂停 LVGL 刷新(往睡着的面板刷数据既费电又无意义)。
        if (bsp_lvgl_lock(200)) {
            const int64_t latest = s_last_activity_us.load(std::memory_order_acquire);
            if (esp_timer_get_time() - latest < timeout_us) {
                bsp_lvgl_unlock();
                return;
            }
            bsp_display_backlight(0);
            bsp_display_power(false); // DISPOFF+SLPIN 面板真睡眠,只关背光会有内容残影
            lv_timer_t *refr = lv_display_get_refr_timer(lv_display_get_default());
            if (refr) lv_timer_pause(refr);
            s_screen_on.store(false, std::memory_order_release);
            if (s_display_awake_lock) esp_pm_lock_release(s_display_awake_lock);
            bsp_lvgl_unlock();
            ESP_LOGI(TAG, "Screen blanked after %u seconds", (unsigned)timeout);
        }
    }
}

void app_registry_keep_awake(void) {
    // 只续期不唤醒:屏幕已熄灭时刷新计时也无副作用(仍是熄灭态),
    // 下一次真正的按键依旧走"第一按仅唤醒"护栏。
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
}

bool app_registry_sound_notice(void) {
    if (reminders::alarm_active()) return false;
    SwitchGuard guard;
    // LVGL is already held: a blocking take would invert switch->LVGL order.
    if (!guard.try_take(0) || s_building_target.load() ||
        s_pending_target.load() != APP_ID_COUNT || boot_chime_busy() ||
        !feedback_allowed(s_active_id.load())) return false;
    if (esp_get_free_heap_size() < 12288 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 6144) return false;
    return ui_sfx_alarm_tick();
}

bool app_registry_wake_for_notice(void) {
    // Caller owns the LVGL lock. Never expose a transition holder or start an
    // app from the system monitor's small stack just to display a reminder.
    if (s_building_target.load() || s_pending_target.load() != APP_ID_COUNT ||
        s_handoff_screen || !lv_screen_active()) return false;
    if (!s_screen_on.load(std::memory_order_acquire)) {
        if (s_display_awake_lock) esp_pm_lock_acquire(s_display_awake_lock);
        bsp_display_power(true);
        bsp_display_backlight(100);
        s_screen_on.store(true, std::memory_order_release);
        resume_target_refresh(lv_display_get_default());
        lv_obj_invalidate(lv_screen_active());
        ESP_LOGI(TAG, "Screen woken for visual notice; foreground unchanged");
    }
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
    return true;
}

void app_registry_set_screen_timeout(uint16_t seconds) {
    s_screen_timeout_seconds.store(seconds, std::memory_order_release);
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
}

uint16_t app_registry_get_screen_timeout(void) {
    return s_screen_timeout_seconds.load(std::memory_order_acquire);
}

bool app_registry_screen_is_on(void) { return s_screen_on.load(std::memory_order_acquire); }

bool app_registry_background_idle(void) {
    return !s_screen_on.load(std::memory_order_acquire) &&
           power_save_level_for(s_active_id.load()) == WifiPowerSaveLevel::LOW_POWER;
}

void app_registry_log_display_state(void) {
    if (!bsp_lvgl_lock(200)) return;
    ESP_LOGI(TAG, "Display: handoffs=%u placeholder_flushes=%u panel_flushes=%u waiting=%d",
        (unsigned)s_handoff_count, (unsigned)s_handoff_flushes, (unsigned)s_panel_flushes,
        s_handoff_screen && lv_screen_active() == s_handoff_screen);
    bsp_lvgl_unlock();
}
