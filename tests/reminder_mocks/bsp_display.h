#pragma once
#include "platform.h"
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock();
uint8_t bsp_display_get_master_brightness();
