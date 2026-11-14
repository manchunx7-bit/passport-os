// components/bsp/include/bsp_audio.h
// ES8311 音频 codec:I2C 走控制口(复用 bsp_i2c 的共享总线),I2S 走全双工数据口。
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 codec 与 I2S。内部会调 bsp_i2c_init()(幂等),无需外部先调。
esp_err_t bsp_audio_init(void);

// RX-only, compact DMA ring for 16 kHz microphone streaming with TLS.
// Deinit before changing profiles; bsp_audio_init retains duplex compatibility.
esp_err_t bsp_audio_init_capture(void);
// TX-only, normal playback DMA ring. No microphone channel is allocated.
esp_err_t bsp_audio_init_playback(void);

// Short system feedback may initialize only an idle codec. It must not reuse
// another owner's device. The registry must await its shutdown before an app starts.
esp_err_t bsp_audio_init_feedback(void);

// 关闭并卸载 codec/I2S,归还 DMA 与 codec 占用的堆(开机音效等一次性用途)。
// 之后的 bsp_audio_init() 可完整重建。运行中的 App 不得调用(会拆掉它的音频)。
void bsp_audio_deinit(void);
// Final power-off step only: close codec and retain its control lock until sleep.
void bsp_audio_prepare_sleep(void);

// 设置采样格式。同格式重复调用是廉价的(直接复用已打开的 codec)。
//
// ⚠ 这里有个必须绕开的坑:esp_codec_dev_open() 在 codec【已打开】时会直接返回 OK 且
//   【不重新配置采样率】。若不先 close,16kHz 播完再播 8kHz 会以 16k 时钟送出 ——
//   音调和速度都快一倍。故本函数在格式变化时先 close 再 open。
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

// 播放 / 录音。bytes 为字节数(16bit 单声道时 = 采样数 x 2)。
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

// 输出音量 0..100(%)。
void bsp_audio_set_volume(uint8_t percent);
// 仅 bsp_audio_init_feedback() 期间有效：番茄钟/提醒用固定输出音量，不随
// 主音量缩小（主音量 0 也照常响）；deinit 或 set_volume 后恢复正常缩放。
void bsp_audio_set_alert_volume(uint8_t percent);

// 整机主音量 0..100(%)。所有 App 请求的音量都会按此比例缩放，避免 App
// 在启动时用自己的默认值覆盖用户的整机设置。
void bsp_audio_set_master_volume(uint8_t percent);
uint8_t bsp_audio_get_master_volume(void);

#ifdef __cplusplus
}
#endif
