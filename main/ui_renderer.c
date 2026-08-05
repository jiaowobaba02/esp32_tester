/**
 * ui_renderer.c — 绘图渲染层: 基础图形 + Alpha 混合文本 + 脏区记录
 *
 * 图形原语全部走 gfx_set_window + gfx_push_pixels 局部开窗,
 * 只向 SPI 发送受影响区域的像素, 不做全屏重绘。
 * ASCII 使用 PC 端预渲染的 Arial 灰度字库 (16/24/32px, 含抗锯齿 Alpha),
 * 中文沿用 16x16 点阵 (font_cn.c, 小尺寸下点阵更清晰)。
 * Alpha 混合: 分量 (fg*a + bg*(255-a)) >> 8, 纯整数运算。
 */
#include "ui_renderer.h"
#include "ascii16.h"
#include "font_cn.h"
#include "ascii_gray16.h"
#include "ascii_gray24.h"
#include "ascii_gray32.h"
#include "cn_gray.h"
#include "sym_gray16.h"
#include "sym_gray24.h"
#include "sym_gray32.h"

/* ================= 基础图形 ================= */

void r_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    /* 边界裁剪 (int32 正确处理负坐标) */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    if (x0 > x1 || y0 > y1 || x0 >= LCD_WIDTH || y0 >= LCD_HEIGHT)
        return;
    gfx_fill_rect(x0, y0, x1, y1, color);
}

void r_clear(uint16_t color)
{
    r_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, color);
}

void r_draw_hline(int x0, int x1, int y, uint16_t color)
{
    r_fill_rect(x0, y, x1, y, color);
}

void r_draw_vline(int x, int y0, int y1, uint16_t color)
{
    r_fill_rect(x, y0, x, y1, color);
}

void r_draw_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    r_draw_hline(x0, x1, y0, color);
    r_draw_hline(x0, x1, y1, color);
    r_draw_vline(x0, y0, y1, color);
    r_draw_vline(x1, y0, y1, color);
}

/* ================= 文本 ================= */

/* 中英混排垂直对齐修正: cn_gray 的 yoff 相对 16x16 格顶, 而 Arial 灰度
 * 字形的 yoff 相对基线 (基线 = y + ascent, 16px ascent=15).
 * 实测混排时中文视觉顶约 y+4..y+5, 英文大写约 y+3 -> 中文整体下沉约 1px.
 * 将中文格顶改为 y-1 后: 中文基线 ≈ (y-1)+16 = y+15 = 英文基线, 顶部也对齐
 * (y+3..y+4). 点阵回退字形 (满 16 格) 底边 = y+15 与英文基线一致. */
#define CN_ALIGN_DY 1

/* ASCII 点阵 14x16 (ascii16.c, 过渡期保留给键盘) */
void r_draw_char_bw16(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = ascii16_data[c - 0x20];
    static uint8_t buf[14 * 16 * 2];           /* 大端直存 */
    for (int row = 0; row < 16; row++) {
        uint8_t b0 = g[row * 2], b1 = g[row * 2 + 1];
        for (int col = 0; col < 14; col++) {
            int bit = (col < 8) ? (b0 >> (7 - col))
                                : (b1 >> (13 - col));
            uint16_t px = (bit & 1) ? fg : bg;
            buf[(row * 14 + col) * 2]     = px >> 8;
            buf[(row * 14 + col) * 2 + 1] = px & 0xFF;
        }
    }
    gfx_set_window(x, y, x + 13, y + 15);
    gfx_push_pixels((const uint16_t *)buf, 14 * 16);
}

/* 灰度中文绘制 (定义在下方): 命中返回 1 */
static int cn_gray_draw(int x, int y, uint16_t code, uint16_t fg, uint16_t bg);

/* 中文绘制: 优先微软雅黑灰度 (抗锯齿), 未收录回退 16x16 点阵 */
void r_draw_cn_char(uint16_t x, uint16_t y, uint16_t code,
                     uint16_t fg, uint16_t bg)
{
    if (cn_gray_draw(x, y, code, fg, bg))
        return;
    int lo = 0, hi = font_cn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *p = &font_cn_data[2 + mid * 34];
        uint16_t c = p[0] | (p[1] << 8);
        if (c == code) {
            const uint8_t *g = p + 2;
            static uint16_t buf[256];
            for (int row = 0; row < 16; row++) {
                for (int col = 0; col < 16; col++) {
                    int bit = (col < 8) ? (g[row*2]   >> (7 - col))
                                        : (g[row*2+1] >> (15 - col));
                    buf[row * 16 + col] = (bit & 1) ? fg : bg;
                }
            }
            gfx_set_window(x, y, x + 15, y + 15);
            gfx_push_pixels(buf, 256);
            return;
        } else if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    /* 缺字: 画方框 */
    r_draw_rect(x, y, x + 15, y + 15, fg);
}

/* ---------- Arial 灰度字库 (Alpha 混合) ---------- */
static const gray_glyph_t *const s_gray_fonts[3] = {
    ascii_gray16, ascii_gray24, ascii_gray32
};
static const uint8_t s_gray_ascent[3] = {
    ascii_gray16_ascent, ascii_gray24_ascent, ascii_gray32_ascent
};

/* ---------- 特殊符号字库 (Arial 灰度, 数学/物理/化学) ---------- */
static const gray_glyph_t *const s_sym_fonts[3] = {
    sym_gray16, sym_gray24, sym_gray32
};
static const uint16_t *const s_sym_codes[3] = {
    sym_gray16_codes, sym_gray24_codes, sym_gray32_codes
};
static const int s_sym_counts[3] = {
    sym_gray16_count, sym_gray24_count, sym_gray32_count
};

/* 符号字形二分查找 (code 表升序): 命中返回字形, 未收录返回 NULL */
static const gray_glyph_t *sym_lookup(font_sz_t sz, uint16_t code)
{
    const uint16_t *codes = s_sym_codes[sz];
    int lo = 0, hi = s_sym_counts[sz] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (codes[mid] == code)
            return &s_sym_fonts[sz][mid];
        else if (codes[mid] < code)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* UTF-8 解码 s[0] 处字符: 返回码点, *plen=字节数 (1/2/3) */
static uint32_t utf8_decode(const char *s, int *plen)
{
    uint8_t c = (uint8_t)*s;
    if (c < 0x80) { *plen = 1; return c; }
    if ((c & 0xE0) == 0xC0 && ((uint8_t)s[1] & 0xC0) == 0x80) {
        *plen = 2;
        return ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && ((uint8_t)s[1] & 0xC0) == 0x80 &&
        ((uint8_t)s[2] & 0xC0) == 0x80) {
        *plen = 3;
        return ((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) |
               ((uint8_t)s[2] & 0x3F);
    }
    *plen = 1;
    return c;
}

/* RGB565 分量 Alpha 混合: (fg*a + bg*(255-a)) >> 8 */
static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a)
{
    int fr = (fg >> 11) & 31, fgc = (fg >> 5) & 63, fb = fg & 31;
    int br = (bg >> 11) & 31, bgc = (bg >> 5) & 63, bb = bg & 31;
    fr = (fr * a + br * (255 - a)) >> 8;
    fgc = (fgc * a + bgc * (255 - a)) >> 8;
    fb = (fb * a + bb * (255 - a)) >> 8;
    return (uint16_t)((fr << 11) | (fgc << 5) | fb);
}

/* ---------- 灰度中文字库 (2bit/像素, 4 级抗锯齿) ---------- */
static const uint16_t cn_a4[4] = { 0, 85, 170, 255 };

/* unicode 二分查找 */
static int cn_gray_find(uint16_t code, const cn_gray_glyph_t **out)
{
    int lo = 0, hi = (int)cn_gray_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = cn_gray[mid].code;
        if (c == code) { *out = &cn_gray[mid]; return 1; }
        else if (c < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* 灰度中文绘制: 整字形局部开窗, 未收录返回 0 (调用方回退点阵) */
static int cn_gray_draw(int x, int y, uint16_t code, uint16_t fg, uint16_t bg)
{
    const cn_gray_glyph_t *g;
    if (!cn_gray_find(code, &g) || !g->w || !g->h)
        return 0;
    int gx = x + g->xoff, gy = y + g->yoff;
    int sx = 0, sy = 0, ex = g->w, ey = g->h;   /* 裁剪子区域 */
    if (gx < 0) { sx = -gx; gx = 0; }
    if (gy < 0) { sy = -gy; gy = 0; }
    if (gx + ex > LCD_WIDTH)  ex = LCD_WIDTH - gx;
    if (gy + ey > LCD_HEIGHT) ey = LCD_HEIGHT - gy;
    if (sx >= ex || sy >= ey)
        return 1;
    int W = ex - sx, H = ey - sy;
    static uint16_t buf[17 * 18];              /* 16x16 字形最大 */
    if (W * H > 17 * 18)
        return 0;
    for (int row = 0; row < H; row++)
        for (int col = 0; col < W; col++) {
            uint32_t pix = g->pixoff +
                           (uint32_t)(row + sy) * g->w + (col + sx);
            uint8_t v = (cn_gray_data[pix >> 2] >> ((pix & 3) << 1)) & 3;
            buf[row * W + col] = blend565(fg, bg, cn_a4[v]);
        }
    gfx_set_window(gx, gy, gx + W - 1, gy + H - 1);
    gfx_push_pixels(buf, (uint32_t)(W * H));
    return 1;
}

/* 绘制单个灰度字形: 整字形局部开窗一次发送, 越界时子区域裁剪 */
static void draw_gray_glyph(int x, int y, font_sz_t sz,
                            const gray_glyph_t *g, uint16_t fg, uint16_t bg)
{
    if (!g->w || !g->h)
        return;
    int baseline = y + s_gray_ascent[sz];
    int gx = x + g->xoff, gy = baseline + g->yoff;
    int sx = 0, sy = 0, ex = g->w, ey = g->h;   /* 裁剪后的子区域 */
    if (gx < 0) { sx = -gx; gx = 0; }
    if (gy < 0) { sy = -gy; gy = 0; }
    if (gx + ex > LCD_WIDTH)  ex = LCD_WIDTH - gx;
    if (gy + ey > LCD_HEIGHT) ey = LCD_HEIGHT - gy;
    if (sx >= ex || sy >= ey)
        return;
    int W = ex - sx, H = ey - sy;
    static uint16_t buf[64 * 48];              /* 32px 最大字形足够 */
    if (W * H > 64 * 48)
        return;
    for (int row = 0; row < H; row++)
        for (int col = 0; col < W; col++) {
            /* 2bit 打包 (4 像素/字节, 低位优先; 与 cn_gray 同格式) */
            uint32_t pix = (uint32_t)((row + sy) * g->w + (col + sx));
            uint8_t v = (g->px[pix >> 2] >> ((pix & 3) << 1)) & 3;
            buf[row * W + col] = blend565(fg, bg, cn_a4[v]);
        }
    gfx_set_window(gx, gy, gx + W - 1, gy + H - 1);
    gfx_push_pixels(buf, (uint32_t)(W * H));
}

static void draw_gray_char(int x, int y, char c, font_sz_t sz,
                           uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E)
        return;
    draw_gray_glyph(x, y, sz, &s_gray_fonts[sz][c - 0x20], fg, bg);
}

/* UTF-8 混排: ASCII 灰度16 (adv 前进) + 中文 16x16 (17px), \n 换行 */
void r_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    gfx_hold_begin();          /* 整段文本共享一次 CS 拉低 */
    int cx = x;
    while (*s) {
        uint8_t c = *s;
        if (c < 0x80) {
            if (c == '\n') { cx = x; y += 18; s++; continue; }
            draw_gray_char(cx, y, (char)c, FONT_16, fg, bg);
            cx += ascii_gray16[c - 0x20].adv;
            s++;
        } else if ((c & 0xE0) == 0xC0) {          /* 2 字节 */
            uint16_t code = ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
            const gray_glyph_t *g = sym_lookup(FONT_16, code);
            if (g && g->w) {                      /* 特殊符号 (×÷°² 等) */
                draw_gray_glyph(cx, y, FONT_16, g, fg, bg);
                cx += g->adv;
            } else {
                r_draw_cn_char(cx, y - CN_ALIGN_DY, code, fg, bg);
                cx += 17;
            }
            s += 2;
        } else if ((c & 0xF0) == 0xE0) {          /* 3 字节 */
            uint16_t code = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            const gray_glyph_t *g = sym_lookup(FONT_16, code);
            if (g && g->w) {
                draw_gray_glyph(cx, y, FONT_16, g, fg, bg);
                cx += g->adv;
            } else {
                r_draw_cn_char(cx, y - CN_ALIGN_DY, code, fg, bg);
                cx += 17;
            }
            s += 3;
        } else {
            s++;
        }
    }
    gfx_hold_end();
}

void r_draw_char_gray16(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    draw_gray_char(x, y, c, FONT_16, fg, bg);
}

/* 码点绘制: 符号字库优先 (₂⇌⁺⁻ 等), 未命中回退中文 (cn_gray → 点阵) */
void r_draw_char_code(int x, int y, uint16_t code, uint16_t fg, uint16_t bg)
{
    const gray_glyph_t *g = sym_lookup(FONT_16, code);
    if (g && g->w) {
        draw_gray_glyph(x, y, FONT_16, g, fg, bg);
    } else {
        r_draw_cn_char(x, y - CN_ALIGN_DY, code, fg, bg);
    }
}

int r_char_adv16(char c)
{
    if (c >= 0x20 && c <= 0x7E)
        return ascii_gray16[c - 0x20].adv;
    return 1;
}

void r_draw_text_sz(int x, int y, font_sz_t sz, const char *s,
                    uint16_t fg, uint16_t bg)
{
    gfx_hold_begin();          /* 整段文本共享一次 CS 拉低 */
    int cx = x;
    while (*s) {
        uint8_t c = *s;
        if (c >= 0x20 && c <= 0x7E) {
            draw_gray_char(cx, y, (char)c, sz, fg, bg);
            cx += s_gray_fonts[sz][c - 0x20].adv;
            s++;
        } else if (c >= 0x80) {               /* UTF-8: 特殊符号 */
            int len;
            uint16_t code = (uint16_t)utf8_decode(s, &len);
            const gray_glyph_t *g = sym_lookup(sz, code);
            if (g && g->w) {
                draw_gray_glyph(cx, y, sz, g, fg, bg);
                cx += g->adv;
            }
            s += len;
        } else {
            s++;
        }
    }
    gfx_hold_end();
}

int r_text_width(const char *s)
{
    int w = 0;
    while (*s) {
        uint8_t c = *s;
        if (c < 0x80) { w += ascii_gray16[c - 0x20].adv; s++; }
        else if ((c & 0xE0) == 0xC0) {
            uint16_t code = ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
            const gray_glyph_t *g = sym_lookup(FONT_16, code);
            w += (g && g->w) ? g->adv : 17;
            s += 2;
        }
        else if ((c & 0xF0) == 0xE0) {
            uint16_t code = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            const gray_glyph_t *g = sym_lookup(FONT_16, code);
            w += (g && g->w) ? g->adv : 17;
            s += 3;
        }
        else s++;
    }
    return w;
}

int r_text_width_sz(font_sz_t sz, const char *s)
{
    int w = 0;
    while (*s) {
        uint8_t c = *s;
        if (c >= 0x20 && c <= 0x7E) {
            w += s_gray_fonts[sz][c - 0x20].adv;
            s++;
        } else if (c >= 0x80) {
            int len;
            uint16_t code = (uint16_t)utf8_decode(s, &len);
            const gray_glyph_t *g = sym_lookup(sz, code);
            if (g && g->w)
                w += g->adv;
            s += len;
        } else {
            s++;
        }
    }
    return w;
}

/* ================= 脏区记录 ================= */
/* 记录被修改的屏幕区域 (上限 16 块, 用于上层做区域化重绘决策) */
#define UI_DIRTY_MAX 16
static int s_dirty_n = 0;
static int s_dirty[UI_DIRTY_MAX][4];

void ui_invalidate(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return;
    if (s_dirty_n < UI_DIRTY_MAX) {
        s_dirty[s_dirty_n][0] = x;
        s_dirty[s_dirty_n][1] = y;
        s_dirty[s_dirty_n][2] = w;
        s_dirty[s_dirty_n][3] = h;
        s_dirty_n++;
    }
}

int ui_dirty_count(void)
{
    return s_dirty_n;
}

void ui_dirty_take(int idx, int *x, int *y, int *w, int *h)
{
    if (idx < 0 || idx >= s_dirty_n)
        return;
    *x = s_dirty[idx][0];
    *y = s_dirty[idx][1];
    *w = s_dirty[idx][2];
    *h = s_dirty[idx][3];
}

void ui_dirty_reset(void)
{
    s_dirty_n = 0;
}
