#pragma once
#include "platform.h"
int bsp_audio_init_feedback(void);
void bsp_audio_deinit(void);
void bsp_audio_set_volume(uint8_t);
void bsp_audio_set_alert_volume(uint8_t);
uint8_t bsp_audio_get_master_volume(void);
