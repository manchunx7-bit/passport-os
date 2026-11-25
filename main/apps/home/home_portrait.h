#pragma once

#include "ui/ui_palette.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOME_PORTRAIT_W 96
#define HOME_PORTRAIT_H 138
#define HOME_PORTRAIT_PIXELS (HOME_PORTRAIT_W * HOME_PORTRAIT_H)
#define HOME_AVATAR_V2_BYTES (HOME_PORTRAIT_PIXELS / 4)
#define HOME_PORTRAIT_MASK_BYTES (HOME_PORTRAIT_PIXELS / 8)
#define HOME_PORTRAIT_HEADER_BYTES 8
#define HOME_AVATAR_BYTES (HOME_PORTRAIT_HEADER_BYTES + HOME_AVATAR_V2_BYTES + HOME_PORTRAIT_MASK_BYTES)
#define HOME_AVATAR_LEGACY_BYTES (96 * 150 / 8)

/* PT36: 15 RGB565 colors plus transparent index 0, stored as 4bpp indices. */
#define HOME_AVATAR_PALETTE_BYTES 32
#define HOME_AVATAR_INDEX_OFFSET (HOME_PORTRAIT_HEADER_BYTES + HOME_AVATAR_PALETTE_BYTES)
#define HOME_AVATAR_COLOR_BYTES (HOME_AVATAR_INDEX_OFFSET + HOME_PORTRAIT_PIXELS / 2)
#define HOME_AVATAR_MAX_BYTES (HOME_AVATAR_COLOR_BYTES > HOME_AVATAR_BYTES ? \
    HOME_AVATAR_COLOR_BYTES : HOME_AVATAR_BYTES)
#define HOME_PORTRAIT_I4_BYTES (64 + HOME_PORTRAIT_PIXELS / 2)

/* PT37: true-color 16bpp little-endian RGB565 portrait + 1bpp MSB-first mask. */
#define HOME_PORTRAIT_RGB565_BYTES (HOME_PORTRAIT_PIXELS * 2)
#define HOME_AVATAR_RGB565_MASK_OFFSET (HOME_PORTRAIT_HEADER_BYTES + HOME_PORTRAIT_RGB565_BYTES)
#define HOME_AVATAR_RGB565_BYTES (HOME_AVATAR_RGB565_MASK_OFFSET + HOME_PORTRAIT_MASK_BYTES)

/* PT35, preset (0=legacy, 1=dots, 2=flat, 3=hybrid), three reserved bytes.
 * Then MSB-first 2bpp tones and an independent MSB-first visibility mask. */
static inline bool home_portrait_is_color(const uint8_t *data) {
    return data[0] == 'P' && data[1] == 'T' && data[2] == '3' && data[3] == '6';
}
static inline bool home_portrait_is_rgb565(const uint8_t *data) {
    return data[0] == 'P' && data[1] == 'T' && data[2] == '3' && data[3] == '7';
}
static inline int home_portrait_valid_header(const uint8_t *data) {
    if (home_portrait_is_rgb565(data))
        return data[4] == 0 && data[5] <= 100 && !data[6] && !data[7];
    if (home_portrait_is_color(data))
        return data[4] <= UI_THEME_COUNT && data[5] <= 100 && !data[6] && !data[7];
    return data[0] == 'P' && data[1] == 'T' && data[2] == '3' && data[3] == '5' &&
        data[4] <= 3 && !data[5] && !data[6] && !data[7];
}
static inline int home_portrait_valid_for_size(const uint8_t *data, size_t bytes) {
    if (bytes == HOME_AVATAR_RGB565_BYTES)
        return home_portrait_is_rgb565(data) && home_portrait_valid_header(data);
    if (bytes == HOME_AVATAR_COLOR_BYTES)
        return home_portrait_is_color(data) && home_portrait_valid_header(data);
    if (bytes == HOME_AVATAR_BYTES)
        return !home_portrait_is_color(data) && !home_portrait_is_rgb565(data) &&
            home_portrait_valid_header(data);
    return 0;
}
static inline void home_portrait_init_header(uint8_t *data, uint8_t preset) {
    data[0] = 'P'; data[1] = 'T'; data[2] = '3'; data[3] = '5';
    data[4] = preset; data[5] = data[6] = data[7] = 0;
}
static inline void home_portrait_init_color_header(uint8_t *data, uint8_t style,
                                                   uint8_t retention) {
    data[0] = 'P'; data[1] = 'T'; data[2] = '3'; data[3] = '6';
    data[4] = style; data[5] = retention; data[6] = data[7] = 0;
}
static inline void home_portrait_init_rgb565_header(uint8_t *data, uint8_t retention) {
    data[0] = 'P'; data[1] = 'T'; data[2] = '3'; data[3] = '7';
    data[4] = 0; data[5] = retention; data[6] = data[7] = 0;
}
static inline int home_portrait_upload_size(size_t bytes) {
    return bytes == HOME_AVATAR_RGB565_BYTES || bytes == HOME_AVATAR_COLOR_BYTES ||
        bytes == HOME_AVATAR_BYTES || bytes == HOME_AVATAR_V2_BYTES ||
        bytes == HOME_AVATAR_LEGACY_BYTES;
}
static inline uint8_t home_portrait_level(const uint8_t *data, size_t pixel) {
    return (data[HOME_PORTRAIT_HEADER_BYTES + pixel / 4] >> (6 - 2 * (pixel % 4))) & 3;
}
static inline uint8_t home_portrait_index(const uint8_t *data, size_t pixel) {
    if (!(data[HOME_PORTRAIT_HEADER_BYTES + HOME_AVATAR_V2_BYTES + pixel / 8] & (128 >> (pixel % 8)))) return 0;
    const uint8_t level = home_portrait_level(data, pixel);
    if (!data[4]) return level;
    if (data[4] == 1) return level ? 4 : 0;
    return level + 1;
}
static inline uint8_t home_portrait_color_index(const uint8_t *data, size_t pixel) {
    return (data[HOME_AVATAR_INDEX_OFFSET + pixel / 2] >> (pixel % 2 ? 0 : 4)) & 15;
}
