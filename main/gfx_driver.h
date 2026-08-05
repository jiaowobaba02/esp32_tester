#ifndef GFX_DRIVER_H
#define GFX_DRIVER_H
#include <stdint.h>
#include <stddef.h>

/* ST6201 4.3寸 480x272 IPS (SPI2_HOST + DMA) */
#define LCD_WIDTH   480
#define LCD_HEIGHT  272

/* SPI 总线 + LCD 初始化 (app_main 调用一次) */
void gfx_init(void);

/* 底层原语: 命令/数据/局部开窗/像素流 (CS 软件控制) */
void gfx_cmd(uint8_t cmd);
void gfx_data(const uint8_t *buf, size_t len);
void gfx_set_window(int x0, int y0, int x1, int y1);
/* 连续事务模式: begin 后多次 cmd/data/push 共享一次 CS 拉低, end 恢复.
 * 文本绘制批量场景使用 (减少 CS 翻转与事务开销); 必须成对调用, 不可嵌套 */
void gfx_hold_begin(void);
void gfx_hold_end(void);
/* 像素流: 输入 RGB565 数组, 内部转大端 (高字节先) 并分块 DMA 发送,
 * CS 全程拉低 (数据流不中断, 消除撕裂感) */
void gfx_push_pixels(const uint16_t *buf, uint32_t n);
/* 纯色填充 (内部开窗 + 字节流 DMA, 不做裁剪; 坐标由调用方保证合法) */
void gfx_fill_rect(int x0, int y0, int x1, int y1, uint16_t color);

#endif
