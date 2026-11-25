// components/bsp/src/bsp_display_lvgl.c
// LVGL 接入单独成文件:不用 LVGL 的开发者删掉本文件 + idf_component.yml 里的两条依赖即可。
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_timer.h"

static const char *TAG = "bsp_lvgl";

static lv_display_t *s_disp;

static uint32_t monotonic_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

lv_display_t *bsp_lvgl_init(void) {
    if (s_disp) return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "请先成功调用 bsp_display_init()");
        return NULL;
    }

    lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
    // 默认 7168 太小:天气图标等 JPEG 走 LVGL TJPEG decoder_info/open(文件源),
    // 栈上 TJPGD_WORKBUFF_SIZE 就有 4KB(lv_tjpgd.c:110),叠加渲染调用链实测
    // 爆栈(Stack protection fault,2026-09-13,崩溃任务即 taskLVGL)。
    pc.task_stack = 12288;
    if (lvgl_port_init(&pc) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init 失败");
        return NULL;
    }

    // The port's default 5ms tick wakes ESP32-C3 200 times/second even asleep.
    // Read the monotonic clock on demand, but leave the handler enabled:
    // a disabled lv_timer_handler returns a 1ms polling delay.
    lvgl_port_lock(0);
    if (lvgl_port_stop() == ESP_OK) lv_tick_set_cb(monotonic_tick_ms);
    lv_timer_enable(true);
    lvgl_port_unlock();

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle    = bsp_display_io(),
        // 12-row single buffer (~5.6 KiB) leaves enough internal RAM for the
        // full-screen I4 Codex canvas and NimBLE on ESP32-C3 without PSRAM.
        .buffer_size   = (uint32_t)BSP_LCD_W * 12,
        .double_buffer = false,
        .hres = BSP_LCD_W, .vres = BSP_LCD_H,
        // 旋转/镜像必须在这里配:esp_lvgl_port 注册显示时会重新下发 MADCTL,
        // 覆盖 bsp_display.c 里 esp_lcd_panel_mirror() 的设置。
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        // swap_bytes:LVGL 输出小端 RGB565,ST7789 走 SPI 要大端 → 需交换高低字节。
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&dc);
    if (!s_disp) { ESP_LOGE(TAG, "lvgl_port_add_disp 失败"); return NULL; }

    // 把 LVGL 的【默认屏】刷成黑。
    // LVGL 默认主题的 screen 背景是近白色;开机阶段(开机动画之前/切换过程中)
    // 只要默认屏短暂成为活动屏并被画进 GRAM,背光一亮就是一片白闪。
    // 统一钉成黑,消除这一整类白闪 —— 与 app_registry 的过渡屏同色。
    lv_obj_t *scr = lv_screen_active();
    if (scr) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    }

    ESP_LOGI(TAG, "LVGL 就绪");
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void bsp_lvgl_unlock(void) {
    lvgl_port_unlock();
    // With no due timer the port task sleeps up to task_max_sleep_ms (500 ms),
    // and only invalidations wake it. A timer created or re-armed from another
    // task (e.g. Tetris builds its UI in the first tick) then waited up to
    // ~0.5 s: game -> Tetris measured 757 ms vs 312 ms. Wake it explicitly.
    lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
}
