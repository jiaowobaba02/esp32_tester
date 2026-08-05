#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H
#include <stdint.h>

/* 主题色 (由 main.c theme_apply 注入) */
void kb_set_theme(uint16_t bg, uint16_t fg, uint16_t bar, uint16_t bar_fg,
                  uint16_t sel, uint16_t border);

/* 打开键盘: field=输入用途 (main.c 定义), init_buf=初始内容 */
void kb_open(int field, const char *init_buf);

/* 全量绘制 (顶栏 + 输入框 + 键盘) */
void kb_draw(void);

/* 触摸事件: 按下即高亮 (局部重绘), 按住连删, 松开触发输入 */
void kb_press(int sx, int sy);
void kb_hold(uint32_t hold_ms);          /* 按住时长 (退格长按连删) */
int  kb_release(int sx, int sy);         /* 返回 1=Enter 完成 */

int  kb_field(void);
const char *kb_buffer(void);

#endif
