#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 真机 UI 截图（调试用途）。
 *
 * C3 内部堆放不下 240x320 的 RGB565 整帧（153600 字节），所以这里**按横带渲染**：
 * 直接操纵 LVGL 的 layer（与 lv_snapshot 同一套机制），把 layer 的 buf_area /
 * _clip_area 限制在一条 240 x N 的横带上，让软件渲染器只画这一带，画完立刻把
 * 像素以十六进制文本从 console 吐出去，再渲染下一条。
 * 峰值内存只有 240 * N * 2 字节（N=32 时 15KB），而不是 150KB。
 *
 * 用十六进制（而不是裸二进制）是因为截图过程中 Wi-Fi / 电池等其他任务仍会打日志，
 * 裸二进制流会被冲断无法恢复；每行带 `H:<行号>:` 前缀，主机端只认这个前缀，
 * 混入的日志行自然被丢掉，缺行还能检测出来。
 *
 * 调用方必须**已经持有** bsp_lvgl_lock（lv_obj_redraw 需要在 LVGL 上下文里跑）。
 */
void ui_capture_dump_hex(void);

/**
 * 渲染性能探针(串口 'F',调试用途)。调用方必须已持有 bsp_lvgl_lock。
 * ① 离屏按 12 行横带纯渲染当前屏(不刷屏) ② 整屏重绘并真实刷屏,
 * 分别记录 flush_cb 与等待 DMA 的耗时。各跑 3 次打印到日志。
 */
void ui_capture_perf_probe(void);

#ifdef __cplusplus
}
#endif
