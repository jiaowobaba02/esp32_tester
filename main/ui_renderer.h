#ifndef UI_RENDERER_H
#define UI_RENDERER_H
#include <stdint.h>
#include <stddef.h>
#include "gfx_driver.h"

/* 颜色 (RGB565) */
#define WHITE 0xFFFF
#define BLACK 0x0000
#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F
#define YELLOW 0xFFE0
#define CYAN  0x07FF
#define MAGENTA 0xF81F
#define GRAY  0x8430

/* 灰度字库字号 (Arial 预渲染) */
typedef enum { FONT_16 = 0, FONT_24 = 1, FONT_32 = 2 } font_sz_t;

/* ---------- 基础图形 (局部开窗, 只发受影响区域) ---------- */
void r_fill_rect(int x0, int y0, int x1, int y1, uint16_t c);
void r_clear(uint16_t c);
void r_draw_hline(int x0, int x1, int y, uint16_t c);
void r_draw_vline(int x, int y0, int y1, uint16_t c);
void r_draw_rect(int x0, int y0, int x1, int y1, uint16_t c);

/* ---------- 文本 ---------- */
/* ASCII 点阵 14x16 (ascii16, 过渡期键盘用) */
void r_draw_char_bw16(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
/* 16x16 中文点阵 (font_cn) */
void r_draw_cn_char(uint16_t x, uint16_t y, uint16_t code, uint16_t fg, uint16_t bg);

/* UTF-8 混排: 中文 16x16 点阵 + ASCII Arial 灰度16 (Alpha 混合), \n 换行 */
void r_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* 按码点绘制 (符号表优先, 回退中文): 供逐字翻页绘制使用 */
void r_draw_char_code(int x, int y, uint16_t code, uint16_t fg, uint16_t bg);
/* 单个 ASCII 字符, 灰度16 (text_wrap_skip 等逐字绘制用) */
void r_draw_char_gray16(int x, int y, char c, uint16_t fg, uint16_t bg);
int  r_char_adv16(char c);              /* 灰度16 前进量 (非 ASCII 返回 1) */

/* 指定灰度字号 (24/32px 大字); 支持 UTF-8 特殊符号 (×÷±√π 等, sym_gray 字库) */
void r_draw_text_sz(int x, int y, font_sz_t sz, const char *s,
                    uint16_t fg, uint16_t bg);

/* 文本宽度 (混排 / 指定字号, 比例字形按 adv 累计; 符号按符号字库 adv) */
int r_text_width(const char *s);
int r_text_width_sz(font_sz_t sz, const char *s);

/* ---------- 脏区记录 (区域化重绘决策) ---------- */
void ui_invalidate(int x, int y, int w, int h);   /* 追加记录 (自动裁剪到屏) */
int  ui_dirty_count(void);
void ui_dirty_take(int idx, int *x, int *y, int *w, int *h);
void ui_dirty_reset(void);

#endif