#include "ui_capture.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
// 只挑需要的私有头，不要图省事 include "lvgl_private.h" —— 它会把
// rlottie/ffmpeg/lottie 的私有头一起拉进来，那些依赖外部库头，编不过。
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr_private.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_obj_private.h"
#include "src/draw/lv_draw_private.h"

static const char *TAG = "uicap";

#define SHOT_W 240
#define SHOT_H 320
#define HEX_PER_LINE 120 /* 每行 120 字节 -> 240 个 hex 字符 */

static const char kHexDigits[] = "0123456789abcdef";

/**
 * 把 [y0, y0+rows) 这条横带渲染进 draw_buf。
 * 与 lv_snapshot_take_to_draw_buf() 的核心区别：buf_area / _clip_area 不是整屏，
 * 而是这条带子 —— 于是 15KB 的缓冲就能"分批"拿到整屏内容。
 */
static void render_band(lv_display_t *disp, lv_obj_t *scr, lv_draw_buf_t *db,
                        int32_t y0, int32_t rows) {
    lv_area_t band;
    band.x1 = 0;
    band.y1 = y0;
    band.x2 = SHOT_W - 1;
    band.y2 = y0 + rows - 1;

    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = db;
    layer.buf_area = band;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = band;
    layer.phy_clip_area = band;

    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);

    lv_display_t *disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t *head_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);

    lv_obj_redraw(&layer, scr);
    // Notices live above the active app, and are part of the real panel image.
    lv_obj_t *top = lv_display_get_layer_top(disp);
    if (top && lv_obj_get_child_count(top)) lv_obj_redraw(&layer, top);

    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    disp->layer_head = head_old;
    lv_refr_set_disp_refreshing(disp_old);

    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
}

static void dump_hex(const uint8_t *data, uint32_t len, uint32_t *line_idx) {
    char out[HEX_PER_LINE * 2 + 24];
    for (uint32_t off = 0; off < len; off += HEX_PER_LINE) {
        uint32_t n = len - off;
        if (n > HEX_PER_LINE) n = HEX_PER_LINE;

        char *p = out;
        p += snprintf(p, 16, "H:%04u:", (unsigned)(*line_idx & 0xFFFFu));
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t b = data[off + i];
            *p++ = kHexDigits[b >> 4];
            *p++ = kHexDigits[b & 0x0Fu];
        }
        *p++ = '\n';
        fwrite(out, 1, (size_t)(p - out), stdout);
        (*line_idx)++;
    }
}

void ui_capture_dump_hex(void) {
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        printf("--SHOT-ERR no-display--\n");
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        printf("--SHOT-ERR no-screen--\n");
        return;
    }

    const size_t stride = (size_t)SHOT_W * 2u;
    uint8_t *buf = NULL;
    int32_t rows = 0;
    const int32_t candidates[] = {32, 16, 8, 4};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        buf = (uint8_t *)heap_caps_malloc(stride * (size_t)candidates[i],
                                          MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (buf != NULL) {
            rows = candidates[i];
            break;
        }
    }
    if (buf == NULL) {
        printf("--SHOT-ERR no-buffer--\n");
        return;
    }

    ESP_LOGI(TAG, "capture start: %dx%d, band=%d rows, free=%u largest=%u", SHOT_W, SHOT_H,
             (int)rows, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    // 先排一次布局，避免截到上一次 layout 未更新的中间态。
    lv_obj_update_layout(scr);

    printf("--SHOT-BEGIN %d %d--\n", SHOT_W, SHOT_H);
    uint32_t line_idx = 0;
    for (int32_t y = 0; y < SHOT_H; y += rows) {
        int32_t r = rows;
        if (y + r > SHOT_H) r = SHOT_H - y;

        memset(buf, 0, stride * (size_t)r);
        lv_draw_buf_t db;
        if (lv_draw_buf_init(&db, (uint32_t)SHOT_W, (uint32_t)r, LV_COLOR_FORMAT_RGB565,
                             (uint32_t)stride, buf, (uint32_t)(stride * (size_t)r)) != LV_RESULT_OK) {
            printf("--SHOT-ERR buf-init--\n");
            break;
        }
        render_band(disp, scr, &db, y, r);
        dump_hex(buf, stride * (size_t)r, &line_idx);
        vTaskDelay(1);  // 别把 USB CDC 与其它任务饿死
    }
    printf("--SHOT-END lines=%u--\n", (unsigned)line_idx);
    fflush(stdout);

    heap_caps_free(buf);
    ESP_LOGI(TAG, "capture done: %u lines", (unsigned)line_idx);
}

static int64_t s_probe_mark, s_probe_flush_us, s_probe_wait_us;
static unsigned s_probe_flushes;
static void probe_event(lv_event_t *e) {
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
        case LV_EVENT_FLUSH_START: ++s_probe_flushes; s_probe_mark = now; break;
        case LV_EVENT_FLUSH_FINISH: s_probe_flush_us += now - s_probe_mark; break;
        case LV_EVENT_FLUSH_WAIT_START: s_probe_mark = now; break;
        case LV_EVENT_FLUSH_WAIT_FINISH: s_probe_wait_us += now - s_probe_mark; break;
        default: break;
    }
}

void ui_capture_perf_probe(void) {
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *scr = lv_screen_active();
    if (!disp || !scr) return;
    const size_t stride = (size_t)SHOT_W * 2u;
    const int32_t rows = 12;
    uint8_t *buf = heap_caps_malloc(stride * rows, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    lv_obj_update_layout(scr);
    for (int run = 0; buf && run < 3; ++run) {
        const int64_t t0 = esp_timer_get_time();
        for (int32_t y = 0; y < SHOT_H; y += rows) {
            lv_draw_buf_t db;
            if (lv_draw_buf_init(&db, SHOT_W, rows, LV_COLOR_FORMAT_RGB565, stride, buf,
                                 stride * rows) != LV_RESULT_OK) break;
            render_band(disp, scr, &db, y, rows);
        }
        ESP_LOGI(TAG, "probe render-only: %lld us", (long long)(esp_timer_get_time() - t0));
    }
    heap_caps_free(buf);
    static const lv_event_code_t codes[] = {LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH,
                                            LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH};
    for (unsigned i = 0; i < 4; ++i) lv_display_add_event_cb(disp, probe_event, codes[i], NULL);
    for (int run = 0; run < 3; ++run) {
        s_probe_flush_us = s_probe_wait_us = 0;
        s_probe_flushes = 0;
        lv_obj_invalidate(scr);
        const int64_t t0 = esp_timer_get_time();
        lv_refr_now(disp);
        ESP_LOGI(TAG, "probe refresh: total=%lld us flush_cb=%lld us dma_wait=%lld us chunks=%u",
                 (long long)(esp_timer_get_time() - t0), (long long)s_probe_flush_us,
                 (long long)s_probe_wait_us, s_probe_flushes);
    }
    for (unsigned i = 0; i < 4; ++i) lv_display_remove_event_cb_with_user_data(disp, probe_event, NULL);
}

