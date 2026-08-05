#ifndef SYM_GRAY24_H
#define SYM_GRAY24_H
#include <stdint.h>

/* 24px Arial 特殊符号灰度字形 (码点升序, C 端二分查找) */
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
#define sym_gray24_count 110
extern const uint16_t sym_gray24_codes[110];
extern const gray_glyph_t sym_gray24[110];
#endif
