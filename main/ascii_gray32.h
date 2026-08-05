#ifndef ASCII_GRAY32_H
#define ASCII_GRAY32_H
#include <stdint.h>

/* 32px Arial 灰度字形: w/h 位图尺寸, xoff/yoff 偏移, adv 前进量 */
#ifndef GRAY_GLYPH_T_DEFINED
#define GRAY_GLYPH_T_DEFINED
typedef struct {
    uint8_t w, h;
    int8_t xoff;
    int8_t yoff;
    uint8_t adv;
    const uint8_t *px;
} gray_glyph_t;
#endif
#define ascii_gray32_ascent 29
extern const gray_glyph_t ascii_gray32[95];
#endif
