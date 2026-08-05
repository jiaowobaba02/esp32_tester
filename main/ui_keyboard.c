/**
 * ui_keyboard.c — 虚拟键盘组件 (QWERTY 错位布局 + 按下反馈 + 长按连删)
 *
 * 布局: 数字行 + 4 行错位 (R1 缩进半键, R2 缩进一键), 含 Shift/退格/空格/回车/符号页。
 * 按下反馈: kb_press 立即将该键背景变深 (局部重绘该键 bbox),
 *           kb_release 恢复原色并触发输入 — 不整屏刷新, 无闪烁。
 * 字母键用 24px Arial 灰度字库 (抗锯齿), 功能键用 16px。
 * 输入框固定在键盘上方 (y=30..44), 键盘从 y=46 起, 无遮挡。
 */
#include "ui_keyboard.h"
#include "ui_renderer.h"
#include "gfx_driver.h"
#include <string.h>
#include <stdio.h>

#define KB_Y0     46             /* 键盘区起始 y */
#define KB_H      42             /* 键高 */
#define KB_GAP    2              /* 键间距 */
#define KB_ROW    (KB_H + KB_GAP) /* 行距 (主键盘 5 行共 218px, 272 屏内) */

/* 功能码 */
#define KB_SHIFT 0x01
#define KB_ENTER 0x02
#define KB_BACK  0x03
#define KB_SPACE 0x04
#define KB_SYM   0x05
#define KB_RET   0x06
#define KB_SYM2  0x07      /* 符号页1 → 符号页2 */
#define KB_SYM1  0x08      /* 符号页2/3 → 符号页1 */
#define KB_SYM3  0x09      /* 符号页2 → 符号页3 (下标) */

/* code: ASCII 字符 (>=0x20) / 功能码 (<0x20) / 0=UTF-8 符号 (utf8 非空) */
typedef struct { char code; const char *utf8; uint16_t w; } kb_key_t;
typedef struct { uint8_t y, x0, n; const kb_key_t *keys; } kb_row_t;

#define K(c, w)  { (c), NULL, (w) }
#define KU(u, w) { 0, (u), (w) }   /* UTF-8 特殊符号键 */

/* ---------- 主布局 (数字行 + QWERTY 错位) ---------- */
static const kb_key_t kb_num_r0[] = {
    K('1',46),K('2',46),K('3',46),K('4',46),K('5',46),
    K('6',46),K('7',46),K('8',46),K('9',46),K('0',46),
};
static const kb_key_t kb_main_r0[] = {
    K('q',46),K('w',46),K('e',46),K('r',46),K('t',46),
    K('y',46),K('u',46),K('i',46),K('o',46),K('p',46),
};
static const kb_key_t kb_main_r1[] = {
    K('a',44),K('s',44),K('d',44),K('f',44),K('g',44),
    K('h',44),K('j',44),K('k',44),K('l',44),K(KB_BACK,44),
};
static const kb_key_t kb_main_r2[] = {
    K(KB_SHIFT,44),K('z',44),K('x',44),K('c',44),K('v',44),
    K('b',44),K('n',44),K('m',44),K(KB_SPACE,80),
};
static const kb_key_t kb_main_r3[] = {
    K(KB_SYM,46),K(KB_SPACE,336),K(KB_ENTER,86),
};
static const kb_row_t kb_main_rows[5] = {
    { KB_Y0 + 0,         8, 10, kb_num_r0 },
    { KB_Y0 + KB_ROW,    8, 10, kb_main_r0 },
    { KB_Y0 + KB_ROW*2, 30, 10, kb_main_r1 },
    { KB_Y0 + KB_ROW*3, 42,  9, kb_main_r2 },
    { KB_Y0 + KB_ROW*4,  8,  3, kb_main_r3 },
};

/* ---------- 符号布局 ---------- */
static const kb_key_t kb_sym_r0[] = {
    K('~',46),K('!',46),K('@',46),K('#',46),K('$',46),
    K('%',46),K('^',46),K('&',46),K('*',46),K('(',46),
};
static const kb_key_t kb_sym_r1[] = {
    K(')',46),K('-',46),K('_',46),K('=',46),K('+',46),
    K('[',46),K(']',46),K('{',46),K('}',46),K('|',46),
};
static const kb_key_t kb_sym_r2[] = {
    K('\\',46),K(';',46),K(':',46),K('\'',46),K('"',46),
    K(',',46),K('.',46),K('<',46),K('>',46),K(KB_BACK,46),
};
static const kb_key_t kb_sym_r3[] = {
    K(KB_RET,44),K(KB_SYM2,44),K(KB_SPACE,292),K(KB_ENTER,86),
};
static const kb_row_t kb_sym_rows[4] = {
    { KB_Y0 + 0,        8, 10, kb_sym_r0 },
    { KB_Y0 + KB_ROW,   8, 10, kb_sym_r1 },
    { KB_Y0 + KB_ROW*2, 8, 10, kb_sym_r2 },
    { KB_Y0 + KB_ROW*3, 8,  3, kb_sym_r3 },
};

/* ---------- 符号页2 (数学/物理/化学) ---------- */
static const kb_key_t kb_sym2_r0[] = {
    KU("×",46),KU("÷",46),KU("±",46),KU("√",46),KU("π",46),
    KU("∞",46),KU("°",46),KU("²",46),KU("³",46),KU("¹",46),
};
static const kb_key_t kb_sym2_r1[] = {
    KU("₂",46),KU("₃",46),KU("↑",46),KU("↓",46),KU("α",46),
    KU("β",46),KU("γ",46),KU("Δ",46),KU("Ω",46),KU("µ",46),
};
static const kb_key_t kb_sym2_r2[] = {
    KU("∑",46),KU("∫",46),KU("≤",46),KU("≥",46),KU("≠",46),
    KU("≈",46),KU("→",46),KU("←",46),KU("½",46),KU("¼",46),
};
static const kb_key_t kb_sym2_r3[] = {
    K(KB_SYM1,44),K(KB_SYM3,44),K(KB_SPACE,292),K(KB_ENTER,86),
};
static const kb_row_t kb_sym2_rows[4] = {
    { KB_Y0 + 0,        8, 10, kb_sym2_r0 },
    { KB_Y0 + KB_ROW,   8, 10, kb_sym2_r1 },
    { KB_Y0 + KB_ROW*2, 8, 10, kb_sym2_r2 },
    { KB_Y0 + KB_ROW*3, 8,  4, kb_sym2_r3 },
};

/* ---------- 符号页3 (下标: 化学式/数学) ---------- */
static const kb_key_t kb_sym3_r0[] = {
    KU("₀",46),KU("₁",46),KU("₂",46),KU("₃",46),KU("₄",46),
    KU("₅",46),KU("₆",46),KU("₇",46),KU("₈",46),KU("₉",46),
};
static const kb_key_t kb_sym3_r1[] = {
    KU("₊",46),KU("₋",46),KU("₌",46),KU("₍",46),KU("₎",46),
    KU("ₐ",46),KU("ₑ",46),KU("ₒ",46),KU("ₓ",46),KU("ₔ",46),
};
static const kb_key_t kb_sym3_r2[] = {
    KU("ₕ",46),KU("ₖ",46),KU("ₗ",46),KU("ₘ",46),KU("ₙ",46),
    KU("ₚ",46),KU("ₛ",46),KU("ₜ",46),KU("½",46),KU("¼",46),
};
static const kb_key_t kb_sym3_r3[] = {
    K(KB_SYM1,44),K(KB_RET,44),K(KB_SPACE,292),K(KB_ENTER,86),
};
static const kb_row_t kb_sym3_rows[4] = {
    { KB_Y0 + 0,        8, 10, kb_sym3_r0 },
    { KB_Y0 + KB_ROW,   8, 10, kb_sym3_r1 },
    { KB_Y0 + KB_ROW*2, 8, 10, kb_sym3_r2 },
    { KB_Y0 + KB_ROW*3, 8,  4, kb_sym3_r3 },
};

/* ---------- 状态 ---------- */
static char s_buf[80];
static int s_field = 0;
static int s_shift = 0;
static int s_sym = 0;   /* 0=主键盘 1=符号页1 2=符号页2 3=下标页 */
static int s_pr_row = -1, s_pr_col = -1;   /* 当前按下的键 (无=-1) */
static uint32_t s_hold_del_at = 0;         /* 连删节流时间戳 */

/* 主题色 */
static uint16_t t_bg = 0xFFFF, t_fg = 0x0000, t_bar = 0x001F;
static uint16_t t_bar_fg = 0xFFFF, t_sel = 0x5D7C, t_border = 0x8430;

void kb_set_theme(uint16_t bg, uint16_t fg, uint16_t bar, uint16_t bar_fg,
                  uint16_t sel, uint16_t border)
{
    t_bg = bg; t_fg = fg; t_bar = bar; t_bar_fg = bar_fg;
    t_sel = sel; t_border = border;
}

void kb_open(int field, const char *init_buf)
{
    s_field = field;
    snprintf(s_buf, sizeof(s_buf), "%s", init_buf ? init_buf : "");
    s_shift = 0;
    s_sym = 0;
    s_pr_row = s_pr_col = -1;
}

int kb_field(void) { return s_field; }
const char *kb_buffer(void) { return s_buf; }

static const kb_row_t *rows(void)
{
    if (s_sym == 3)
        return kb_sym3_rows;
    if (s_sym == 2)
        return kb_sym2_rows;
    return s_sym ? kb_sym_rows : kb_main_rows;
}

/* 键几何: 返回该行该键的 x,y,w (命中返回 1) */
static int key_rect(const kb_row_t *r, int col, int *x, int *y, int *w)
{
    if (col < 0 || col >= r->n)
        return 0;
    int cx = r->x0;
    for (int i = 0; i < col; i++)
        cx += r->keys[i].w + KB_GAP;
    *x = cx;
    *y = r->y;
    *w = r->keys[col].w;
    return 1;
}

/* 命中测试: 返回 (行, 列); 未命中 (-1,-1) */
static void hit_test(int sx, int sy, int *row, int *col)
{
    *row = *col = -1;
    const kb_row_t *rs = rows();
    int rn = s_sym ? 4 : 5;              /* 主键盘 5 行 (含数字行), 符号页 4 行 */
    for (int r = 0; r < rn; r++) {
        if (sy < rs[r].y || sy >= rs[r].y + KB_H)
            continue;
        int cx = rs[r].x0;
        for (int c = 0; c < rs[r].n; c++) {
            if (sx >= cx && sx < cx + rs[r].keys[c].w) {
                *row = r; *col = c;
                return;
            }
            cx += rs[r].keys[c].w + KB_GAP;
        }
    }
}

/* ---------- 绘制 ---------- */
static const char *key_label(char code)
{
    switch (code) {
    case KB_SHIFT: return "^";
    case KB_ENTER: return "OK";
    case KB_BACK:  return "<";
    case KB_SPACE: return "SPC";
    case KB_SYM:   return "#";
    case KB_RET:   return "<";
    case KB_SYM2:  return "2/2";
    case KB_SYM1:  return "1/2";
    case KB_SYM3:  return "3/3";
    default:       return NULL;
    }
}

static void draw_key(int row, int col, int pressed)
{
    const kb_row_t *r = &rows()[row];
    char code = r->keys[col].code;
    const char *utf8 = r->keys[col].utf8;
    int x, y, w;
    if (!key_rect(r, col, &x, &y, &w))
        return;
    /* 背景: 按下=选中色; shift 激活=选中色; 功能键=浅灰; 普通=主题 bg */
    uint16_t bg = t_bg;
    int is_fn = (code > 0 && code < 0x20);
    if (pressed)
        bg = t_sel;
    else if (code == KB_SHIFT && s_shift)
        bg = t_sel;
    else if (is_fn)
        bg = t_border;
    uint16_t fg = (bg == t_sel) ? t_bar_fg : t_fg;
    uint16_t bd = (code == KB_SHIFT && s_shift) ? t_bar : t_border;
    r_fill_rect(x, y, x + w - 1, y + KB_H - 1, bg);
    r_draw_rect(x, y, x + w - 1, y + KB_H - 1, bd);

    /* label: 字母/符号=24px 灰度, 功能=16px */
    if (code >= 0x20) {
        char ch = (s_shift && code >= 'a' && code <= 'z') ? code - 'a' + 'A' : code;
        char one[2] = { ch, 0 };
        int tw = r_text_width_sz(FONT_24, one);
        int th = 24;
        r_draw_text_sz(x + (w - tw) / 2, y + (KB_H - th) / 2, FONT_24, one, fg, bg);
    } else if (code == 0 && utf8) {
        int tw = r_text_width_sz(FONT_24, utf8);
        r_draw_text_sz(x + (w - tw) / 2, y + (KB_H - 24) / 2, FONT_24, utf8, fg, bg);
    } else {
        const char *lb = key_label(code);
        if (lb) {
            int tw = r_text_width_sz(FONT_16, lb);
            r_draw_text_sz(x + (w - tw) / 2, y + (KB_H - 16) / 2, FONT_16, lb, fg, bg);
        }
    }
}

/* 输入框 (y=30..44): 超长显示末尾 (按像素从尾部逐字符累计, 到右缘才滚动) */
#define INPUT_MAX_W (474 - 10)          /* 文本起点 x=10, 框右缘 x=474 */
static void draw_input(void)
{
    r_fill_rect(6, 30, 474, 44, t_bg);
    r_draw_rect(6, 30, 474, 44, t_border);
    int blen = strlen(s_buf);
    const char *disp = s_buf;
    if (blen > 0) {
        int w = 0;
        int start = blen;
        while (start > 0) {
            /* 回退到字符起始 (跳过 UTF-8 连续字节) */
            int i = start - 1;
            while (i > 0 && ((uint8_t)s_buf[i] & 0xC0) == 0x80)
                i--;
            char one[4];
            int clen = start - i;
            memcpy(one, &s_buf[i], clen);
            one[clen] = 0;
            int cw = r_text_width_sz(FONT_16, one);   /* 单个字符宽 */
            if (w + cw > INPUT_MAX_W)
                break;                                /* 放不下: 从这里开始滚 */
            w += cw;
            start = i;
        }
        disp = s_buf + start;
    }
    r_draw_text_sz(10, 31, FONT_16, disp, t_fg, t_bg);
}

void kb_draw(void)
{
    r_clear(t_bg);
    r_fill_rect(0, 0, 479, 26, t_bar);
    /* 标题按用途: 填空输入显示"填空答案" */
    r_draw_text(8, 5, s_field == 4 ? "填空答案" : "输入", t_bar_fg, t_bar);
    /* 返回按钮 (点击放弃输入返回; 填空时提示"返回题目") */
    r_draw_rect(240, 2, 330, 24, t_bar_fg);
    r_draw_text(248, 5, s_field == 4 ? "返回题目" : "返回", t_bar_fg, t_bar);
    r_draw_text(340, 5, "完成保存", t_bar_fg, t_bar);
    draw_input();
    const kb_row_t *rs = rows();
    int rn = s_sym ? 4 : 5;
    for (int r = 0; r < rn; r++)
        for (int c = 0; c < rs[r].n; c++)
            draw_key(r, c, 0);
}

/* ---------- 事件 ---------- */
void kb_press(int sx, int sy)
{
    int r, c;
    hit_test(sx, sy, &r, &c);
    if (r < 0) {
        s_pr_row = s_pr_col = -1;
        return;
    }
    if (s_pr_row == r && s_pr_col == c)
        return;                       /* 同一键重复按下 */
    if (s_pr_row >= 0)                 /* 换键: 恢复旧的 */
        draw_key(s_pr_row, s_pr_col, 0);
    s_pr_row = r;
    s_pr_col = c;
    draw_key(r, c, 1);                 /* 按下高亮 (仅该键区域) */
    s_hold_del_at = 0;
}

/* 退格: 删除末尾一个完整 UTF-8 字符 */
static void kb_backspace(void)
{
    int len = strlen(s_buf);
    if (len <= 0)
        return;
    int i = len - 1;
    while (i > 0 && ((uint8_t)s_buf[i] & 0xC0) == 0x80)
        i--;
    s_buf[i] = 0;
}

/* 按住: 退格长按连续删除 (>500ms 后每 120ms 删一个) */
void kb_hold(uint32_t hold_ms)
{
    if (s_pr_row < 0)
        return;
    char code = rows()[s_pr_row].keys[s_pr_col].code;
    if (code != KB_BACK)
        return;
    if (hold_ms < 500)
        return;
    uint32_t now = hold_ms;
    if (s_hold_del_at == 0)
        s_hold_del_at = now;
    if (now - s_hold_del_at < 120)
        return;
    s_hold_del_at = now;
    kb_backspace();
    draw_input();                  /* 局部刷新输入框 */
}

/* 松开: 恢复键色 + 触发输入; 返回 1=Enter 完成 */
int kb_release(int sx, int sy)
{
    int r = s_pr_row, c = s_pr_col;
    s_pr_row = s_pr_col = -1;
    if (r < 0)
        return 0;
    char code = rows()[r].keys[c].code;
    const char *utf8 = rows()[r].keys[c].utf8;
    int changed = 0;
    int len = strlen(s_buf);

    if (code >= 0x20) {
        char ch = (s_shift && code >= 'a' && code <= 'z') ? code - 'a' + 'A' : code;
        if (len < (int)sizeof(s_buf) - 1) {
            s_buf[len] = ch;
            s_buf[len + 1] = 0;
            changed = 1;
        }
        if (s_shift) {                 /* 输入字母后 shift 自动复位 */
            s_shift = 0;
            /* 复位 shift 键高亮 (遍历主键盘各行查找) */
            if (!s_sym) {
                const kb_row_t *rs = rows();
                for (int rr = 0; rr < 5; rr++)
                    for (int i = 0; i < rs[rr].n; i++)
                        if (rs[rr].keys[i].code == KB_SHIFT)
                            draw_key(rr, i, 0);
            }
        }
    } else if (code == KB_BACK) {
        if (len > 0) {
            kb_backspace();
            changed = 1;
        }
    } else if (code == 0 && utf8) {    /* UTF-8 特殊符号 */
        int ul = (int)strlen(utf8);
        if (len + ul < (int)sizeof(s_buf)) {
            memcpy(s_buf + len, utf8, ul + 1);
            changed = 1;
        }
    } else if (code == KB_SPACE) {
        if (len < (int)sizeof(s_buf) - 1) {
            s_buf[len] = ' ';
            s_buf[len + 1] = 0;
            changed = 1;
        }
    } else if (code == KB_SHIFT) {
        s_shift = !s_shift;
        draw_key(r, c, 0);             /* 恢复原色并更新高亮态 */
        return 0;
    } else if (code == KB_SYM) {       /* 主 ↔ 符号页1 */
        s_sym = s_sym ? 0 : 1;
        s_pr_row = s_pr_col = -1;
        kb_draw();                     /* 整页切换 */
        return 0;
    } else if (code == KB_SYM2) {      /* 符号页1 → 符号页2 */
        s_sym = 2;
        s_pr_row = s_pr_col = -1;
        kb_draw();
        return 0;
    } else if (code == KB_SYM3) {      /* 符号页2 → 符号页3 (下标) */
        s_sym = 3;
        s_pr_row = s_pr_col = -1;
        kb_draw();
        return 0;
    } else if (code == KB_SYM1) {      /* 符号页2/3 → 符号页1 */
        s_sym = 1;
        s_pr_row = s_pr_col = -1;
        kb_draw();
        return 0;
    } else if (code == KB_RET) {       /* 符号页返回主布局 */
        s_sym = 0;
        s_pr_row = s_pr_col = -1;
        kb_draw();
        return 0;
    } else if (code == KB_ENTER) {
        draw_key(r, c, 0);
        return 1;                      /* 完成 */
    }
    draw_key(r, c, 0);                 /* 恢复原色 */
    if (changed)
        draw_input();                  /* 局部刷新输入框 */
    return 0;
}
