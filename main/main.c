/**
 * ST6201 4.3寸 480x272 IPS —— ETSP32 SPI 驱动 + 基础图形层
 *
 * 官方配置: SPI2_HOST + DMA, CLK=23 MOSI=19 CS=22 DC=14 RST=12 (软件 CS)
 * mode 0, 40MHz, 像素低字节先 (官方 LVGL 小端)
 * 背光: GPIO2 + GPIO32(飞线) 高电平开
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#define PIN_SCK   23
#define PIN_MOSI  19
#define PIN_CS    22
#define PIN_DC    14
#define PIN_RST   12
#define PIN_BL    2
#define PIN_BL2   32

/* GT911 触摸 (官方配置) */
#define TOUCH_SDA  18
#define TOUCH_SCL  16
#define TOUCH_RST  4
#define TOUCH_INT  17

#define LCD_WIDTH   480
#define LCD_HEIGHT  272

#define WHITE 0xFFFF
#define BLACK 0x0000
#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F
#define YELLOW 0xFFE0
#define CYAN  0x07FF
#define MAGENTA 0xF81F
#define GRAY  0x8430

static const char *TAG = "st6201";
static spi_device_handle_t s_spi;

/* ---------- 官方初始化序列 ---------- */
static const uint8_t seq[][2] = {
    {0xFF,0xA5},{0xE7,0x10},{0x35,0x00},{0x36,0xC0},{0x3A,0x01},{0x40,0x01},
    {0x41,0x03},{0x44,0x15},{0x45,0x15},{0x7D,0x03},{0xC1,0xBB},{0xC2,0x05},
    {0xC3,0x10},{0xC6,0x3E},{0xC7,0x25},{0xC8,0x21},{0x7A,0x51},{0x6F,0x49},
    {0x78,0x57},{0xC9,0x00},{0x67,0x11},{0x51,0x0A},{0x52,0x7D},{0x53,0x0A},
    {0x54,0x7D},{0x46,0x0A},{0x47,0x2A},{0x48,0x0A},{0x49,0x1A},{0x44,0x15},
    {0x45,0x15},{0x73,0x08},{0x74,0x10},{0x56,0x43},{0x57,0x42},{0x58,0x3C},
    {0x59,0x64},{0x5A,0x41},{0x5B,0x3C},{0x5C,0x02},{0x5D,0x3C},{0x5E,0x1F},
    {0x60,0x80},{0x61,0x3F},{0x62,0x21},{0x63,0x07},{0x64,0xE0},{0x65,0x02},
    {0xCA,0x20},{0xCB,0x52},{0xCC,0x10},{0xCD,0x42},{0xD0,0x20},{0xD1,0x52},
    {0xD2,0x10},{0xD3,0x42},{0xD4,0x0A},{0xD5,0x32},
    {0x80,0x00},{0xA0,0x00},{0x81,0x06},{0xA1,0x08},{0x82,0x03},{0xA2,0x03},
    {0x86,0x14},{0xA6,0x14},{0x87,0x2C},{0xA7,0x26},{0x83,0x37},{0xA3,0x37},
    {0x84,0x35},{0xA4,0x35},{0x85,0x3F},{0xA5,0x3F},{0x88,0x0A},{0xA8,0x0A},
    {0x89,0x13},{0xA9,0x12},{0x8A,0x18},{0xAA,0x19},{0x8B,0x0A},{0xAB,0x0A},
    {0x8C,0x17},{0xAC,0x0B},{0x8D,0x1A},{0xAD,0x09},{0x8E,0x1A},{0xAE,0x08},
    {0x8F,0x1F},{0xAF,0x00},{0x90,0x08},{0xB0,0x00},{0x91,0x10},{0xB1,0x06},
    {0x92,0x19},{0xB2,0x15},{0xFF,0x00},
};

/* ---------- SPI 底层 (CS 软件控制) ---------- */
static void lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_DC, 0);
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_transmit(s_spi, &t);
    gpio_set_level(PIN_CS, 1);
}

static void lcd_data(const uint8_t *buf, size_t len)
{
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_DC, 1);
    while (len) {
        size_t chunk = len > 4096 ? 4096 : len;
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = chunk * 8;
        t.tx_buffer = buf;
        spi_device_transmit(s_spi, &t);
        buf += chunk;
        len -= chunk;
    }
    gpio_set_level(PIN_CS, 1);
}

/* ---------- 初始化 ---------- */
static void lcd_init(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        lcd_cmd(seq[i][0]);
        uint8_t d = seq[i][1];
        lcd_data(&d, 1);
    }
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---------- 图形原语 ---------- */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];
    lcd_cmd(0x2A);
    b[0]=x0>>8; b[1]=x0&0xFF; b[2]=x1>>8; b[3]=x1&0xFF; lcd_data(b, 4);
    lcd_cmd(0x2B);
    b[0]=y0>>8; b[1]=y0&0xFF; b[2]=y1>>8; b[3]=y1&0xFF; lcd_data(b, 4);
    lcd_cmd(0x2C);
}

/* 推送像素: 必须已设窗口; CS 全程低 (数据流不中断) */
/* ⚠️ 实测: 屏按大端解析 (高字节先) — 小端会 R→B/G→R/B→G 通道错乱 */
static void lcd_push_pixels(const uint16_t *buf, uint32_t n)
{
    static uint8_t big[8192];
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_DC, 1);
    uint32_t i = 0;
    while (i < n) {
        uint32_t chunk_n = (n - i > 4096 / 2) ? 4096 / 2 : (n - i);
        for (uint32_t j = 0; j < chunk_n; j++) {
            big[j * 2]     = buf[i + j] >> 8;   /* 高字节先 */
            big[j * 2 + 1] = buf[i + j] & 0xFF;
        }
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = chunk_n * 16;
        t.tx_buffer = big;
        spi_device_transmit(s_spi, &t);
        i += chunk_n;
    }
    gpio_set_level(PIN_CS, 1);
}

void lcd_fill_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color)
{
    /* 边界裁剪 (int32 正确处理负坐标) */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    if (x0 > x1 || y0 > y1 || x0 >= LCD_WIDTH || y0 >= LCD_HEIGHT)
        return;

    uint32_t n = (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1);
    static uint8_t buf[2048 * 2];              /* 大端: 高字节先, 4KB 大块减少事务数 */
    lcd_set_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_DC, 1);
    uint32_t i = 0;
    while (i < n) {
        uint32_t chunk = (n - i > 2048) ? 2048 : (n - i);
        for (uint32_t j = 0; j < chunk; j++) {
            buf[j * 2]     = color >> 8;       /* 高字节先 */
            buf[j * 2 + 1] = color & 0xFF;
        }
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = chunk * 16;
        t.tx_buffer = buf;
        spi_device_transmit(s_spi, &t);
        i += chunk;
    }
    gpio_set_level(PIN_CS, 1);
}

void lcd_clear(uint16_t color)
{
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, color);
}

void lcd_draw_hline(int32_t x0, int32_t x1, int32_t y, uint16_t color)
{
    lcd_fill_rect(x0, y, x1, y, color);
}

void lcd_draw_vline(int32_t x, int32_t y0, int32_t y1, uint16_t color)
{
    lcd_fill_rect(x, y0, x, y1, color);
}

void lcd_draw_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color)
{
    lcd_draw_hline(x0, x1, y0, color);
    lcd_draw_hline(x0, x1, y1, color);
    lcd_draw_vline(x0, y0, y1, color);
    lcd_draw_vline(x1, y0, y1, color);
}


/* 画字符 (x,y 左上角): 14x16 平滑雅黑 ASCII (ascii16.c) */
#include "ascii16.h"

static void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = ascii16_data[c - 0x20];
    static uint8_t buf[14 * 16 * 2];           /* 大端直存, 与 fill_rect 一致 */
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
    lcd_set_window(x, y, x + 13, y + 15);
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 14 * 16 * 16;
    t.tx_buffer = buf;
    spi_device_transmit(s_spi, &t);
    gpio_set_level(PIN_CS, 1);
}

void lcd_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) {
        lcd_draw_char(x, y, *s++, fg, bg);
        x += 17;
    }
}

/* ---------- 16x16 中文 (font_cn.bin 生成, unicode 有序, 二分查找) ---------- */
#include "font_cn.h"

static void lcd_draw_cn_char(uint16_t x, uint16_t y, uint16_t code, uint16_t fg, uint16_t bg)
{
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
            lcd_set_window(x, y, x + 15, y + 15);
            lcd_push_pixels(buf, 256);
            return;
        } else if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    /* 缺字: 画方框 */
    lcd_draw_rect(x, y, x + 15, y + 15, fg);
}

/* UTF-8 混合文本: ASCII 14x16 (15px), 中文 16x16 (17px), \n 换行 */
void lcd_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t cx = x;
    while (*s) {
        uint8_t c = *s;
        if (c < 0x80) {
            if (c == '\n') { cx = x; y += 18; s++; continue; }
            lcd_draw_char(cx, y, c, fg, bg);
            cx += 17;
            s++;
        } else if ((c & 0xE0) == 0xC0) {          /* 2 字节 */
            uint16_t code = ((c & 0x1F) << 6) | (s[1] & 0x3F);
            lcd_draw_cn_char(cx, y, code, fg, bg);
            cx += 17;
            s += 2;
        } else if ((c & 0xF0) == 0xE0) {          /* 3 字节 CJK */
            uint16_t code = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            lcd_draw_cn_char(cx, y, code, fg, bg);
            cx += 17;
            s += 3;
        } else {
            s++;
        }
    }
}

/* ================================================================
 * GT911 触摸 — 软件 I2C (bit-bang) 实现
 * 原因: ESP32 I2C 外设轮询 GT911 会挂死主循环(旧/新驱动都卡),
 *       软件 I2C 完全可控, 不会死锁 (MicroPython SoftI2C 同思路)
 * ================================================================ */
static uint8_t s_touch_addr;
static int touch_init_done = 0;
static uint16_t s_xmax = LCD_WIDTH, s_ymax = LCD_HEIGHT;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

/* ---------- 软件 I2C (开漏输出 + 内部上拉) ---------- */
static void i2c_delay(void)
{
    esp_rom_delay_us(10);                   /* ~100kHz 标准模式 */
}

static void i2c_start(void)
{
    gpio_set_level(TOUCH_SDA, 1);
    gpio_set_level(TOUCH_SCL, 1);
    i2c_delay();
    gpio_set_level(TOUCH_SDA, 0);
    i2c_delay();
    gpio_set_level(TOUCH_SCL, 0);
}

static void i2c_stop(void)
{
    gpio_set_level(TOUCH_SDA, 0);
    gpio_set_level(TOUCH_SCL, 1);
    i2c_delay();
    gpio_set_level(TOUCH_SDA, 1);
    i2c_delay();
}

static int i2c_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(TOUCH_SDA, (b >> i) & 1);
        i2c_delay();
        gpio_set_level(TOUCH_SCL, 1);
        i2c_delay();
        gpio_set_level(TOUCH_SCL, 0);
    }
    /* ACK: SDA 释放, 读从机应答 */
    gpio_set_level(TOUCH_SDA, 1);
    i2c_delay();
    gpio_set_level(TOUCH_SCL, 1);
    i2c_delay();
    int ack = gpio_get_level(TOUCH_SDA);
    gpio_set_level(TOUCH_SCL, 0);
    return ack;                              /* 0=ACK */
}

static uint8_t i2c_read_byte(int send_ack)
{
    uint8_t v = 0;
    gpio_set_level(TOUCH_SDA, 1);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(TOUCH_SCL, 1);
        i2c_delay();
        v = (v << 1) | gpio_get_level(TOUCH_SDA);
        gpio_set_level(TOUCH_SCL, 0);
        i2c_delay();
    }
    gpio_set_level(TOUCH_SDA, send_ack ? 0 : 1);   /* ACK=0 / NACK=1 */
    i2c_delay();
    gpio_set_level(TOUCH_SCL, 1);
    i2c_delay();
    gpio_set_level(TOUCH_SCL, 0);
    gpio_set_level(TOUCH_SDA, 1);
    return v;
}

/* GT911 读 (硬件 I2C) */
static int gt911_r(uint16_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, r, 2, buf, len, 100) == ESP_OK ? 0 : -1;
}

static int gt911_w8(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_master_transmit(s_dev, b, 3, 100) == ESP_OK ? 0 : -1;
}

static void gt911_reset(void)
{
    gpio_set_level(TOUCH_RST, 0);
    gpio_set_level(TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_INT, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));         /* 复位后等待更长 */
    gpio_set_direction(TOUCH_INT, GPIO_MODE_INPUT);
}

static int touch_init(void)
{
    /* 复位 GT911 (地址由 INT 电平决定) */
    gpio_config_t rst = {
        .pin_bit_mask = (1ULL << TOUCH_RST) | (1ULL << TOUCH_INT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gt911_reset();

    /* 硬件 I2C (已验证 GT911 在线) */
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init fail");
        return -1;
    }

    static const uint8_t addrs[] = { 0x5D, 0x14 };
    for (int i = 0; i < 2; i++) {
        s_touch_addr = addrs[i];
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,        /* 400kHz 快 4 倍 */
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK)
            continue;
        uint8_t id[4] = { 0 };
        if (gt911_r(0x8140, id, 4) == 0 && id[0] == '9' && id[1] == '1') {
            ESP_LOGI(TAG, "GT911 found @0x%02X id=%c%c%c (hw i2c)",
                     addrs[i], id[0], id[1], id[2]);
            uint8_t b[4];
            if (gt911_r(0x8048, b, 4) == 0) {
                uint16_t xm = b[0] | (b[1] << 8), ym = b[2] | (b[3] << 8);
                if (xm > 16 && xm < 4096) s_xmax = xm;
                if (ym > 16 && ym < 4096) s_ymax = ym;
            }
            ESP_LOGI(TAG, "chip max=(%d,%d)", s_xmax, s_ymax);
            touch_init_done = 1;
            return 0;
        }
    }
    ESP_LOGE(TAG, "GT911 not found (hw i2c)");
    return -1;
}

/* 硬件 I2C 对照探测 (验证 GT911 是否在线 / 软件 I2C 是否 bug) */
static void hw_i2c_probe(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "hw i2c bus init fail");
        return;
    }
    static const uint8_t addrs[] = { 0x5D, 0x14 };
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,        /* 400kHz 快 4 倍 */
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) {
            ESP_LOGE(TAG, "hw add dev 0x%02X fail", addrs[i]);
            continue;
        }
        uint8_t reg[2] = { 0x81, 0x40 }, id[4] = { 0 };
        esp_err_t e = i2c_master_transmit_receive(dev, reg, 2, id, 4, 200);
        ESP_LOGI(TAG, "HW i2c 0x%02X: ret=%s id=%c%c%c",
                 addrs[i], esp_err_to_name(e), id[0], id[1], id[2]);
    }
}

/* 返回 1=有触摸 (官方 EYA gt911.c 逻辑: 每点 4 字节, 小端) */
static int touch_read(int *x, int *y)
{
    uint8_t st;
    if (gt911_r(0x814E, &st, 1) != 0)
        return 0;
    if (st & 0x80) {
        gt911_w8(0x814E, 0x00);              /* 先清 buffer ready (官方逻辑, 打破死锁) */
        ESP_LOGD(TAG, "cleared status 0x%02X", st);
    }
    int npoints = st & 0x0F;                 /* 点数 (低 4 位) */
    if (npoints == 0 || npoints >= 6)
        return 0;
    uint8_t buf[4];
    if (gt911_r(0x8150, buf, 4) != 0)        /* 官方: 只读第 1 点 4 字节 */
        return 0;
    int px = buf[0] | (buf[1] << 8);         /* 小端 */
    int py = buf[2] | (buf[3] << 8);
    if (px == 0 && py == 0)                  /* 官方: 全 0 忽略 */
        return 0;
    if (px >= 4096 || py >= 4096)
        return 0;
    *x = px;
    *y = py;
    return 1;
}

/* ================================================================
 * 刷题机 UI (BOOT 键导航: 短按=选择/下一步, 长按=进入/提交/返回)
 * ================================================================ */
#include "questions.h"
#include "ai_quiz.h"
#include "fav.h"
#include "weak.h"

#define BTN_GPIO  0            /* BOOT 键, 低电平按下 */
#define LBLUE     0x5D7C       /* 选中高亮 */
#define PANEL_BG  0x0841
#define BTN_BD    0x7BEF

static const char *s_subjects[9] = {
    "数学", "物理", "化学", "生物", "英语", "语文", "历史", "政治", "地理"
};

static int s_state = 0;        /* 0=菜单 1=答题 */
static int s_menu_sel = 0;
static int s_qidx = 0;         /* 科目内题号 */
static int s_qlist[16];        /* 科目内题目索引 */
static int s_qcount = 0;
static int s_opt_sel = 0;      /* 高亮选项 */
static int s_answered = 0;
static int s_show_ans = 0;     /* 非选择题: 已显示参考答案 */
static int s_correct = 0, s_total = 0;
static int s_exp_page = 0, s_exp_total = 0;   /* 解析页: 页码/总页 */
static int s_qpage = 0, s_qtotal = 0;         /* 题目全文页: 页码/总行 */
static int s_wpage = 0, s_wtotal = 0;         /* 薄弱点总结页: 页码/总行 */
static int s_fav_page = 0;                    /* 收藏列表: 页码 */
static int s_weak_list_page = 0;              /* 薄弱点错题列表: 页码 */

static void text_center(int y, const char *s, uint16_t fg, uint16_t bg);
static void text_center2(int cx, int y, const char *s, uint16_t fg, uint16_t bg);
static void http_server_start(void);

/* 设置数据 (WiFi/API 等, 定义在此供 WiFi 代码使用) */
static char s_wifi_ssid[33] = "";
static char s_wifi_pass[65] = "";
char s_api_key[65] = "";         /* 全局: ai_quiz.c 使用 */
static int s_wifi_state = 0;     /* 0=未连接 1=连接中 2=已连接 */
static char s_wifi_ip[17] = "";
int s_grade = 2;                 /* 年级: 0=高一 1=高二 2=高三 (ai_quiz.c 使用) */
static const char *s_grade_names[3] = { "高一", "高二", "高三" };

/* ---------- 主题 (0=明亮 1=护眼 2=夜间) ---------- */
static int s_theme = 0;
static uint16_t s_th_bg = 0xFFFF;      /* 背景 */
static uint16_t s_th_fg = 0x0000;      /* 前景文字 */
static uint16_t s_th_bar = 0x001F;     /* 顶栏 */
static uint16_t s_th_bar_fg = 0xFFFF;  /* 顶栏文字 */
static uint16_t s_th_sel = 0x5D7C;     /* 选中高亮 */
static uint16_t s_th_border = 0x8430;  /* 边框/次要 */

static void theme_apply(int t)
{
    s_theme = t;
    switch (t) {
    case 1:  /* 护眼: 米黄底 */
        s_th_bg = 0xFDEF; s_th_fg = 0x0000; s_th_bar = 0x2B4B;
        s_th_bar_fg = 0xFFFF; s_th_sel = 0x9E93; s_th_border = 0x7BEF;
        break;
    case 2:  /* 夜间: 黑底白字 */
        s_th_bg = 0x0000; s_th_fg = 0xFFFF; s_th_bar = 0x2104;
        s_th_bar_fg = 0xFFFF; s_th_sel = 0x28A8; s_th_border = 0x6B6D;
        break;
    default: /* 明亮 */
        s_th_bg = 0xFFFF; s_th_fg = 0x0000; s_th_bar = 0x001F;
        s_th_bar_fg = 0xFFFF; s_th_sel = 0x5D7C; s_th_border = 0x8430;
    }
}

/* ---------- 背光亮度 (LEDC PWM, 0-100) ---------- */
#include "driver/ledc.h"

static int s_brightness = 100;

static void backlight_set(int pct)
{
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    s_brightness = pct;
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void backlight_init(void)
{
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t c1 = {
        .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = 255, .hpoint = 0,
    };
    ledc_channel_config(&c1);
    ledc_channel_config_t c2 = {
        .gpio_num = PIN_BL2, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_0,
        .duty = 255, .hpoint = 0,
    };
    ledc_channel_config(&c2);
    backlight_set(s_brightness);
}

/* ---------- 5x7 小字 (列优先, bit0=顶行) ---------- */
static const uint8_t s_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

static void draw_small_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = s_font5x7[c - 0x20];
    static uint8_t buf[5 * 7 * 2];          /* 大端直存 */
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++) {
            uint16_t px = ((g[col] >> row) & 1) ? fg : bg;
            buf[(row * 5 + col) * 2]     = px >> 8;
            buf[(row * 5 + col) * 2 + 1] = px & 0xFF;
        }
    lcd_set_window(x, y, x + 4, y + 6);
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 5 * 7 * 16;
    t.tx_buffer = buf;
    spi_device_transmit(s_spi, &t);
    gpio_set_level(PIN_CS, 1);
}

static void draw_small_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) {
        draw_small_char(x, y, *s++, fg, bg);
        x += 6;
    }
}

/* 顶部栏: 长按返回提示 + 右侧网络状态 (小字) */
static void draw_ip_bar(int show_back)
{
    if (show_back)
        lcd_draw_text(250, 5, "长按返回", s_th_bar_fg, s_th_bar);
    if (s_wifi_state == 2) {
        draw_small_str(330, 4, s_wifi_ip, s_th_bar_fg, s_th_bar);
    } else if (s_wifi_state == 1) {
        draw_small_str(370, 4, "WIFI...", s_th_bar_fg, s_th_bar);
    } else {
        lcd_draw_text(420, 6, "离线", s_th_bar_fg, s_th_bar);
    }
}

/* ---------- WiFi ---------- */
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#define MAX_AP 16
static wifi_ap_record_t s_ap_list[MAX_AP];
static int s_ap_count = 0;

static uint32_t s_last_reconnect = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA_START, ssid='%s'", s_wifi_ssid);
        if (s_wifi_ssid[0]) {
            esp_err_t e = esp_wifi_connect();
            ESP_LOGI(TAG, "connect ret=%s", esp_err_to_name(e));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_state = 0;
        ESP_LOGI(TAG, "wifi disconnected");
        uint32_t now = esp_timer_get_time() / 1000;
        if (now - s_last_reconnect > 15000) {  /* 节流: 15 秒重连一次 */
            s_last_reconnect = now;
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_state = 2;
        ESP_LOGI(TAG, "got ip %s", s_wifi_ip);
        http_server_start();               /* 连接成功自动开 HTTP 服务器 */
    }
}

static void wifi_connect_now(void)
{
    if (!s_wifi_ssid[0])
        return;
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_wifi_pass, sizeof(wc.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_connect();
    s_wifi_state = 1;
    ESP_LOGI(TAG, "connecting to %s", s_wifi_ssid);
}

static void wifi_scan(void)
{
    s_ap_count = 0;
    /* 扫描前断开连接 (连接中不允许扫描) */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = MAX_AP;
        esp_wifi_scan_get_ap_records(&n, s_ap_list);
        s_ap_count = n;
        for (int i = 0; i < s_ap_count - 1; i++)      /* 按信号排序 */
            for (int j = i + 1; j < s_ap_count; j++)
                if (s_ap_list[j].rssi > s_ap_list[i].rssi) {
                    wifi_ap_record_t t = s_ap_list[i];
                    s_ap_list[i] = s_ap_list[j];
                    s_ap_list[j] = t;
                }
    }
    ESP_LOGI(TAG, "scan done: %d APs", s_ap_count);
    /* 扫描完恢复连接 */
    if (s_wifi_ssid[0]) {
        s_wifi_state = 1;
        esp_wifi_connect();
    }
}

static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    if (s_wifi_ssid[0]) {
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, s_wifi_ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, s_wifi_pass, sizeof(wc.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        s_wifi_state = 1;              /* STA_START 事件会自动 connect */
    }
}

static void settings_save(void);

/* ---------- API Key 网页输入 (HTTP 服务器) ---------- */
#include "esp_http_server.h"

static httpd_handle_t s_httpd = NULL;
static volatile int s_api_key_updated = 0;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(const char *src, char *dst, int dstsz)
{
    int i = 0, j = 0;
    while (src[i] && j < dstsz - 1) {
        if (src[i] == '+') { dst[j++] = ' '; i++; }
        else if (src[i] == '%' && src[i+1] && src[i+2]) {
            dst[j++] = (char)(hexval(src[i+1]) * 16 + hexval(src[i+2]));
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = 0;
}

static esp_err_t http_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "</head><body style='font-family:sans-serif;text-align:center;margin-top:50px'>"
        "<h2>输入 API Key</h2>"
        "<form method='POST' action='/api'>"
        "<input name='key' size='40' style='font-size:20px;padding:8px' "
        "placeholder='sk-...'>"
        "<br><br><button type='submit' style='font-size:20px;padding:10px 30px'>保存</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t http_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = 0;
        char *v = strstr(buf, "key=");
        if (v) {
            v += 4;
            /* 去掉尾部换行/空白 */
            char *nl = strchr(v, '\r');
            if (nl) *nl = 0;
            nl = strchr(v, '\n');
            if (nl) *nl = 0;
            url_decode(v, s_api_key, sizeof(s_api_key));
            settings_save();
            s_api_key_updated = 1;
            ESP_LOGI(TAG, "api key saved (%d chars)", (int)strlen(s_api_key));
        }
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='text-align:center;margin-top:60px;font-size:24px'>"
        "<b>已保存</b>，可以关闭此页面了。</body></html>");
    return ESP_OK;
}

static void http_server_start(void)
{
    if (s_httpd)
        return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        httpd_uri_t g = { .uri = "/", .method = HTTP_GET, .handler = http_get_handler,
                          .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &g);
        httpd_uri_t p = { .uri = "/api", .method = HTTP_POST, .handler = http_post_handler,
                          .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &p);
        ESP_LOGI(TAG, "http server on :8080");
    }
}

/* ---------- 设置 (NVS 存储) ---------- */
#include "nvs_flash.h"
#include "nvs.h"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READONLY, &h) == ESP_OK) {
        size_t len;
        len = sizeof(s_wifi_ssid);
        if (nvs_get_str(h, "ssid", s_wifi_ssid, &len) != ESP_OK)
            s_wifi_ssid[0] = 0;
        len = sizeof(s_wifi_pass);
        if (nvs_get_str(h, "pass", s_wifi_pass, &len) != ESP_OK)
            s_wifi_pass[0] = 0;
        len = sizeof(s_api_key);
        if (nvs_get_str(h, "apikey", s_api_key, &len) != ESP_OK)
            s_api_key[0] = 0;
        int32_t v = 0;
        if (nvs_get_i32(h, "theme", &v) == ESP_OK && v >= 0 && v <= 2)
            s_theme = v;
        if (nvs_get_i32(h, "bright", &v) == ESP_OK && v >= 1 && v <= 100)
            s_brightness = v;
        if (nvs_get_i32(h, "grade", &v) == ESP_OK && v >= 0 && v <= 2)
            s_grade = v;
        nvs_close(h);
    }
    theme_apply(s_theme);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", s_wifi_ssid);
        nvs_set_str(h, "pass", s_wifi_pass);
        nvs_set_str(h, "apikey", s_api_key);
        nvs_set_i32(h, "theme", s_theme);
        nvs_set_i32(h, "bright", s_brightness);
        nvs_set_i32(h, "grade", s_grade);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- 软键盘 ---------- */
#define KB_Y0    46            /* 键盘区起始 y */
#define KB_H     40            /* 键高 */
#define KB_W     48            /* 键宽 (10 列) */

static int s_kb_field = 0;     /* 0=SSID 1=密码 2=API Key */
static int s_kb_shift = 0;
static int s_kb_sym = 0;       /* 符号页 */
static char s_kb_buf[80];

static const char *s_field_names[3] = { "WiFi 名称", "WiFi 密码", "API 密钥" };

/* 键盘主布局 (行 x 10 列) */
static const char kb_main[5][10] = {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l','\x03'},   /* 退格 */
    {'\x01','z','x','c','v','b','n','m','\x02','\x02'}, /* shift, zxcvbnm, 完成(2列) */
    {'\x04','\x04','\x04','\x04','\x04','\x04','\x04','\x04','\x05','\x05'}, /* 空格(8), 符号(2) */
};
/* 符号布局 */
static const char kb_sym[5][10] = {
    {'~','!','@','#','$','%','^','&','*','('},
    {')','-','_','=','+','[',']','{','}','|'},
    {'\\',';',':','\'','"',',','.','<','>','\x03'},
    {'/','?','`','\x06','\x02','\x02','\x02','\x02','\x02','\x02'},  /* 返回, 完成 */
    {'\x04','\x04','\x04','\x04','\x04','\x04','\x04','\x04','\x05','\x05'},
};
/* 功能码: 0x01=shift 0x02=完成 0x03=退格 0x04=空格 0x05=符号页 0x06=返回 */

static void draw_keyboard(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char top[72];
    snprintf(top, sizeof(top), "%s  (%d)", s_field_names[s_kb_field], s_kb_field + 1);
    lcd_draw_text(8, 5, top, s_th_bar_fg, s_th_bar);
    lcd_draw_text(340, 5, "完成保存", s_th_bar_fg, s_th_bar);

    /* 输入显示 (超长显示末尾) */
    lcd_draw_rect(6, 30, 474, 44, s_th_border);
    int blen = strlen(s_kb_buf);
    const char *disp = s_kb_buf;
    if (blen > 28)
        disp = s_kb_buf + blen - 28;
    lcd_draw_text(10, 31, disp, s_th_fg, s_th_bg);

    const char (*kb)[10] = s_kb_sym ? kb_sym : kb_main;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 10; c++) {
            char k = kb[r][c];
            int dup = (c > 0 && kb[r][c - 1] == k);   /* 宽键重复列 */
            int x = c * KB_W, y = KB_Y0 + r * KB_H;
            uint16_t bg = s_th_bg, fg = BLACK;
            if (k < 0x20) { bg = s_th_sel; fg = 0x0000; }
            lcd_fill_rect(x + 2, y + 2, x + KB_W - 2, y + KB_H - 2, bg);
            lcd_draw_rect(x + 2, y + 2, x + KB_W - 2, y + KB_H - 2, s_th_border);
            if (dup)
                continue;                              /* 宽键 label 只画一次 */
            if (k >= 0x20) {
                char ch = (s_kb_shift && k >= 'a' && k <= 'z') ? k - 'a' + 'A' : k;
                lcd_draw_char(x + (KB_W - 14) / 2, y + 10, ch, fg, bg);
            } else if (k == 0x01) {                    /* shift: 激活高亮 */
                if (s_kb_shift) {
                    lcd_fill_rect(x + 2, y + 2, x + KB_W - 2, y + KB_H - 2, s_th_sel);
                    lcd_draw_rect(x + 2, y + 2, x + KB_W - 2, y + KB_H - 2, s_th_bar);
                }
                lcd_draw_char(x + (KB_W - 14) / 2, y + 10, '^', BLACK, s_kb_shift ? s_th_sel : s_th_sel);
            } else if (k == 0x02) {                    /* 完成 (2 列宽, 居中) */
                text_center2(x + KB_W, y + 12, "OK", fg, bg);
            } else if (k == 0x03) {                    /* 退格 */
                lcd_draw_char(x + (KB_W - 14) / 2, y + 10, '<', fg, bg);
            } else if (k == 0x04) {                    /* 空格 (8 列宽, 居中) */
                text_center2(x + KB_W * 8 / 2, y + 12, "SPC", fg, bg);
            } else if (k == 0x05) {                    /* 符号页 */
                lcd_draw_char(x + (KB_W - 14) / 2, y + 10, '#', fg, bg);
            } else if (k == 0x06) {                    /* 返回 */
                lcd_draw_char(x + (KB_W - 14) / 2, y + 10, '<', fg, bg);
            }
        }
    }
}

/* 触摸键盘: 返回 1=输入框变(局部刷新) 2=完成 3=shift变(刷输入框+shift键) 4=键盘页切换(全重绘) */
static int kb_touch(int sx, int sy)
{
    if (sy < KB_Y0) return 0;
    int r = (sy - KB_Y0) / KB_H;
    int c = sx / KB_W;
    if (r < 0 || r > 4 || c < 0 || c > 9) return 0;
    const char (*kb)[10] = s_kb_sym ? kb_sym : kb_main;
    char k = kb[r][c];
    int len = strlen(s_kb_buf);
    if (k >= 0x20) {
        char ch = (s_kb_shift && k >= 'a' && k <= 'z') ? k - 'a' + 'A' : k;
        if (len < (int)sizeof(s_kb_buf) - 1) {
            s_kb_buf[len] = ch;
            s_kb_buf[len + 1] = 0;
        }
        s_kb_shift = 0;
        return 3;
    }
    switch (k) {
    case 0x01: s_kb_shift = !s_kb_shift; return 3;
    case 0x03: if (len) s_kb_buf[len - 1] = 0; return 1;
    case 0x04: if (len < (int)sizeof(s_kb_buf) - 1) { s_kb_buf[len] = ' '; s_kb_buf[len+1] = 0; } return 1;
    case 0x05: s_kb_sym = !s_kb_sym; return 4;
    case 0x06: s_kb_sym = 0; s_kb_shift = 0; return 4;
    case 0x02:  /* 完成: 保存到设置项 */
        if (s_kb_field == 0) {
            strncpy(s_wifi_ssid, s_kb_buf, sizeof(s_wifi_ssid) - 1);
            s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = 0;
        } else if (s_kb_field == 1) {
            strncpy(s_wifi_pass, s_kb_buf, sizeof(s_wifi_pass) - 1);
            s_wifi_pass[sizeof(s_wifi_pass) - 1] = 0;
        } else {
            strncpy(s_api_key, s_kb_buf, sizeof(s_api_key) - 1);
            s_api_key[sizeof(s_api_key) - 1] = 0;
        }
        settings_save();
        s_kb_sym = 0;
        s_kb_shift = 0;
        if (s_kb_field == 1)             /* 密码完成: 触发 WiFi 连接 */
            wifi_connect_now();
        return 2;
    }
    return 0;
}

static void draw_settings(void)
{
    static const char *tnames[3] = { "明亮", "护眼", "夜间" };

    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "设置", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    /* 3 项: WiFi 名 / 密码 / API (紧凑布局) */
    const char *vals[3] = { s_wifi_ssid, s_wifi_pass, s_api_key };
    for (int i = 0; i < 3; i++) {
        int y = 34 + i * 40;
        lcd_draw_rect(10, y, 470, y + 36, s_th_border);
        lcd_draw_text(20, y + 2, s_field_names[i], s_th_fg, s_th_bg);
        char vbuf[80];
        if (i == 1 && s_wifi_pass[0])
            snprintf(vbuf, sizeof(vbuf), "%s", "******");
        else
            snprintf(vbuf, sizeof(vbuf), "%s", vals[i]);
        lcd_draw_text(20, y + 19, vbuf[0] ? vbuf : "(未设置)", s_th_border, s_th_bg);
        lcd_draw_text(400, y + 19, i == 2 ? "网页输入" : "编辑", s_th_fg, s_th_bg);
    }

    /* 主题 (点击切换) */
    int ty = 158;
    lcd_draw_rect(10, ty, 470, ty + 26, s_th_border);
    lcd_draw_text(20, ty + 5, "主题", s_th_fg, s_th_bg);
    char tbuf[40];
    snprintf(tbuf, sizeof(tbuf), "%s (点击切换)", tnames[s_theme]);
    lcd_draw_text(130, ty + 5, tbuf, s_th_bar, s_th_bg);

    /* 亮度 (左减右加) */
    int by = 188;
    lcd_draw_rect(10, by, 470, by + 26, s_th_border);
    lcd_draw_text(20, by + 5, "亮度", s_th_fg, s_th_bg);
    char bbuf[32];
    snprintf(bbuf, sizeof(bbuf), "%d%%", s_brightness);
    lcd_draw_text(120, by + 5, bbuf, s_th_fg, s_th_bg);
    lcd_draw_text(280, by + 5, "[左半减]", s_th_border, s_th_bg);
    lcd_draw_text(400, by + 5, "[右半加]", s_th_bar, s_th_bg);

    /* 年级 (点击切换) */
    int gy = 218;
    lcd_draw_rect(10, gy, 470, gy + 24, s_th_border);
    lcd_draw_text(20, gy + 4, "年级", s_th_fg, s_th_bg);
    char gbuf[40];
    snprintf(gbuf, sizeof(gbuf), "%s (点击切换)", s_grade_names[s_grade]);
    lcd_draw_text(130, gy + 4, gbuf, s_th_bar, s_th_bg);

    /* 底部按钮: 扫描 WiFi / 手动输入 */
    lcd_draw_rect(10, 244, 235, 264, s_th_bar);
    text_center2(122, 248, "扫描 WiFi", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(245, 244, 470, 264, s_th_border);
    text_center2(357, 248, "手动输入", s_th_fg, s_th_bg);
}

/* API Key 网页输入页 */
static void draw_api_page(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "API 密钥", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    if (s_wifi_state == 2) {
        char buf[64];
        snprintf(buf, sizeof(buf), "浏览器打开 http://%s:8080", s_wifi_ip);
        text_center2(240, 60, buf, s_th_fg, s_th_bg);
        text_center(90, "粘贴 API Key 后点保存", s_th_border, s_th_bg);
        if (s_api_key_updated)
            text_center(120, "已收到并保存!", GREEN, s_th_bg);
        else if (s_api_key[0])
            text_center(120, "当前已保存 (可覆盖)", s_th_border, s_th_bg);
    } else {
        text_center(90, "请先连接 WiFi", RED, s_th_bg);
        text_center(110, "返回设置页连接网络", s_th_border, s_th_bg);
    }
}

/* WiFi 扫描列表页 */
static void draw_ap_list(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "WiFi 列表", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    int shown = s_ap_count > 7 ? 7 : s_ap_count;
    for (int i = 0; i < shown; i++) {
        int y = 30 + i * 32;
        lcd_draw_rect(6, y, 474, y + 28, s_th_border);
        char ssid[33];
        memcpy(ssid, s_ap_list[i].ssid, 32);
        ssid[32] = 0;
        for (int j = 0; ssid[j]; j++)
            if ((uint8_t)ssid[j] < 0x20) ssid[j] = '?';
        lcd_draw_text(12, y + 5, ssid[0] ? ssid : "(隐藏网络)", s_th_fg, s_th_bg);
        char rssi[20];
        snprintf(rssi, sizeof(rssi), "%d dBm", s_ap_list[i].rssi);
        lcd_draw_text(400, y + 5, rssi, s_th_border, s_th_bg);
    }
    if (s_ap_count == 0)
        text_center(150, "未发现 WiFi 网络", RED, s_th_bg);
    else if (s_ap_count > 7)
        text_center(262, "仅显示前 7 个, 点选输入密码", s_th_border, s_th_bg);
    else
        text_center(262, "点选 WiFi 输入密码", s_th_border, s_th_bg);
}

/* ---------- 文本工具 ---------- */
static int text_width(const char *s)
{
    int w = 0;
    while (*s) {
        uint8_t c = *s;
        if (c < 0x80) { w += 17; s++; }
        else if ((c & 0xE0) == 0xC0) { w += 17; s += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 17; s += 3; }
        else s++;
    }
    return w;
}

static void text_center(int y, const char *s, uint16_t fg, uint16_t bg)
{
    lcd_draw_text((LCD_WIDTH - text_width(s)) / 2, y, s, fg, bg);
}

static int text_wrap_skip(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                          int max_w, int line_h, int max_lines, int skip_lines,
                          int *p_total);

/* 自动换行文本, 返回行数; max_lines 限行(0=不限), 超出截断 */
static int text_wrap(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                     int max_w, int line_h, int max_lines)
{
    return text_wrap_skip(x, y, s, fg, bg, max_w, line_h, max_lines, 0, NULL);
}

/* 带跳行的自动换行 (翻页用): skip_lines 跳过前 N 行, 最多画 max_lines 行,
 * 返回总行数 (p_total 同值); y<0 时只统计不绘制 */
static int text_wrap_skip(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                          int max_w, int line_h, int max_lines, int skip_lines,
                          int *p_total)
{
    int cx = x, cy = y, cur_line = 1, lines = 1;
    int count_only = (y < 0);              /* y<0: 只统计行数不绘制 */
    while (*s) {
        uint8_t c = *s;
        int w = 17;                        /* ASCII 与中文同宽步进 */

        /* 逐字换行 (中英文统一) */
        if (cx + w > x + max_w && cx > x) {
            cx = x;
            cy += line_h;
            cur_line++;
            if (cur_line > lines) lines = cur_line;
        }
        if (c == '\n') {
            cx = x;
            cy += line_h;
            cur_line++;
            if (cur_line > lines) lines = cur_line;
            s++;
            continue;
        }

        /* 当前行在显示窗口 [skip+1, skip+max_lines] 内才画 */
        int in_view = (cur_line > skip_lines && cur_line <= skip_lines + max_lines);
        if (!count_only && in_view) {
            if (c < 0x80) {
                lcd_draw_char(cx, cy, c, fg, bg);
                cx += 17;   /* 与 ASCII 步进一致 (曾漏改, 导致字符重叠右半被覆盖) */
                s++;
            } else if ((c & 0xE0) == 0xC0) {
                uint16_t code = ((c & 0x1F) << 6) | (s[1] & 0x3F);
                lcd_draw_cn_char(cx, cy, code, fg, bg);
                cx += 17;
                s += 2;
            } else if ((c & 0xF0) == 0xE0) {
                uint16_t code = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
                lcd_draw_cn_char(cx, cy, code, fg, bg);
                cx += 17;
                s += 3;
            } else {
                s++;
            }
        } else {
            /* 不画, 只推进宽度以保持换行计算 */
            if (c < 0x80) {
                cx += 17;
                s++;
            } else if ((c & 0xE0) == 0xC0) {
                cx += 17;
                s += 2;
            } else if ((c & 0xF0) == 0xE0) {
                cx += 17;
                s += 3;
            } else {
                s++;
            }
        }
    }
    if (p_total) *p_total = lines;
    return lines;
}

static void text_center2(int cx, int y, const char *s, uint16_t fg, uint16_t bg);

/* ---------- 菜单页 ---------- */
static void draw_menu(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 30, s_th_bar);
    text_center(6, "高中刷题机", s_th_bar_fg, s_th_bar);
    draw_ip_bar(0);

    /* 统计 (顶部) */
    char buf[48];
    snprintf(buf, sizeof(buf), "已答 %d  正确 %d (%.0f%%)",
             s_total, s_correct, s_total ? 100.0 * s_correct / s_total : 0);
    text_center(34, buf, s_th_border, s_th_bg);

    /* 科目网格 3x3 */
    for (int i = 0; i < 9; i++) {
        int x = 10 + (i % 3) * 158, y = 46 + (i / 3) * 64;
        int sel = (i == s_menu_sel);
        uint16_t bg = sel ? s_th_sel : s_th_bg;
        lcd_fill_rect(x, y, x + 148, y + 56, bg);
        lcd_draw_rect(x, y, x + 148, y + 56, sel ? s_th_bar : s_th_border);
        text_center2(x + 74, y + 20, s_subjects[i], sel ? s_th_bar_fg : s_th_fg, bg);
    }

    /* 底部按钮: AI 出题 / 收藏 / 薄弱点 / 设置 */
    lcd_draw_rect(10, 240, 130, 262, s_th_bar);
    text_center2(70, 244, "AI 出题", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(135, 240, 255, 262, s_th_border);
    text_center2(195, 244, "收藏", s_th_fg, s_th_bg);
    lcd_draw_rect(260, 240, 380, 262, s_th_border);
    text_center2(320, 244, "薄弱点", s_th_fg, s_th_bg);
    lcd_draw_rect(385, 240, 470, 262, s_th_border);
    text_center2(427, 244, "设置", s_th_fg, s_th_bg);
}

/* ---------- 薄弱点 UI ---------- */
static int s_weak_subj = 0;

static void draw_weak_subject(void)      /* 选科 */
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "薄弱点: 选择科目", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    for (int i = 0; i < 9; i++) {
        int x = 10 + (i % 3) * 158, y = 40 + (i / 3) * 70;
        lcd_fill_rect(x, y, x + 148, y + 60, s_th_bg);
        lcd_draw_rect(x, y, x + 148, y + 60, s_th_border);
        text_center2(x + 74, y + 22, s_subjects[i], s_th_fg, s_th_bg);
    }
}

static void draw_weak_page(void)         /* 该科薄弱点页 */
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char t[48];
    snprintf(t, sizeof(t), "薄弱点: %s", s_subjects[s_weak_subj]);
    lcd_draw_text(8, 5, t, s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    int n = weak_count(s_weak_subj);
    int per = 4;                              /* 每页 4 条错题 */
    int total_pages = (n + per - 1) / per;
    if (total_pages < 1) total_pages = 1;
    if (s_weak_list_page >= total_pages) s_weak_list_page = total_pages - 1;
    char cnt[40];
    if (total_pages > 1)
        snprintf(cnt, sizeof(cnt), "错题 %d 题  %d/%d", n, s_weak_list_page + 1, total_pages);
    else
        snprintf(cnt, sizeof(cnt), "错题 %d 题", n);
    lcd_draw_text(8, 30, cnt, s_th_fg, s_th_bg);

    /* 错题列表 (每页 4 条; 多页时左右半屏翻页) */
    int ly = 46;
    for (int i = s_weak_list_page * per; i < n && i < s_weak_list_page * per + per; i++) {
        char tt[130];
        if (weak_get_wrong(s_weak_subj, i, tt, sizeof(tt)) == 0) {
            char line[160];
            snprintf(line, sizeof(line), "%d. %s", i + 1, tt);
            int bl = strlen(line), px = 0, cut = 0;
            for (int j = 0; j < bl && px < 430; ) {
                uint8_t ch = (uint8_t)line[j];
                if (ch < 0x80) { px += 17; j++; }
                else if ((ch & 0xE0) == 0xC0) { px += 17; j += 2; }
                else if ((ch & 0xF0) == 0xE0) { px += 17; j += 3; }
                else j++;
                cut = j;
            }
            if (cut < bl) {
                line[cut] = 0;
                strcat(line, "...");
            }
            lcd_draw_text(8, ly, line, s_th_fg, s_th_bg);
            ly += 30;
        }
    }
    if (total_pages > 1) {
        lcd_draw_text(8, ly, "左翻上页 右翻下页", s_th_border, s_th_bg);
        ly += 18;
    } else if (n == 0) {
        lcd_draw_text(8, ly, "(暂无错题, 答错会自动记录)", s_th_border, s_th_bg);
        ly += 18;
    }

    /* AI 总结预览 (2 行) + 全屏入口 */
    const char *ai = weak_get_ai(s_weak_subj);
    if (ai[0]) {
        text_wrap(8, ly + 8, ai, s_th_fg, s_th_bg, 460, 15, 2);
        lcd_draw_text(8, ly + 40, "全屏查看总结 >", s_th_border, s_th_bg);
    } else {
        lcd_draw_text(8, ly + 8, "点下方按钮生成 AI 薄弱点总结", s_th_border, s_th_bg);
    }

    /* 底部三按钮: AI 总结 / AI 分析 / 返回 */
    int has_ai = ai[0];
    lcd_draw_rect(10, 240, 156, 262, has_ai ? s_th_bar : s_th_border);
    text_center2(83, 244, "AI 总结", has_ai ? s_th_bar_fg : s_th_border, s_th_bg);
    lcd_draw_rect(162, 240, 308, 262, s_th_bar);
    text_center2(235, 244, "AI 分析", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(314, 240, 470, 262, s_th_border);
    text_center2(392, 244, "返回", s_th_fg, s_th_bg);
}

static void draw_weak_full(void);        /* 前置声明 (定义在下方) */

static void weak_ai_run(void)            /* 触发 AI 分析 (阻塞) */
{
    ESP_LOGI(TAG, "weak ai run: wifi=%d subj=%d", s_wifi_state, s_weak_subj);
    if (s_wifi_state != 2) {
        lcd_clear(s_th_bg);
        text_center(100, "请先连接 WiFi", RED, s_th_bg);
        text_center(130, "点击返回", s_th_border, s_th_bg);
        s_state = 14;
        return;
    }
    lcd_clear(s_th_bg);
    text_center(110, "AI 分析中... (最长60秒)", s_th_fg, s_th_bg);
    /* 汇总错题 (最多 5 题) */
    char topics[1000] = "";
    int n = weak_count(s_weak_subj);
    for (int i = 0; i < n && i < 5; i++) {
        char t[130];
        if (weak_get_wrong(s_weak_subj, i, t, sizeof(t)) == 0) {
            if (strlen(topics) + strlen(t) + 2 < sizeof(topics)) {
                strcat(topics, t);
                strcat(topics, "；");
            }
        }
    }
    const char *ai = ai_analyze_weakness(s_subjects[s_weak_subj], topics[0] ? topics : "暂无具体错题内容，请给出该科学习建议");
    if (ai[0]) {
        weak_set_ai(s_weak_subj, ai);
        s_wpage = 0;                       /* 生成成功: 直接全屏展示总结 */
        s_state = 16;
        draw_weak_full();
    } else {
        lcd_clear(s_th_bg);
        text_center(100, "AI 分析失败", RED, s_th_bg);
        text_center(130, "检查网络 / API Key", s_th_border, s_th_bg);
        text_center(150, "点击返回", s_th_border, s_th_bg);
        s_state = 14;
    }
}

/* 收藏夹列表页 */
static void draw_fav_list(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "收藏夹", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d 题", fav_count());
    lcd_draw_text(400, 5, cnt, s_th_bar_fg, s_th_bar);

    int n = fav_count();
    int per = 5;                              /* 每页 5 条, 底部留翻页按钮行 */
    int total_pages = (n + per - 1) / per;
    if (total_pages < 1) total_pages = 1;
    if (s_fav_page >= total_pages) s_fav_page = total_pages - 1;
    int shown = n - s_fav_page * per;
    if (shown > per) shown = per;
    if (shown < 0) shown = 0;
    for (int i = 0; i < shown; i++) {
        if (fav_get(s_fav_page * per + i) != 0)
            break;
        int y = 30 + i * 38;
        lcd_draw_rect(6, y, 474, y + 34, s_th_border);
        /* 单行: [科目] 内容 (截断) */
        char line[96];
        snprintf(line, sizeof(line), "[%s] %s", g_fav_q.subject, g_fav_q.content);
        int bl = strlen(line), px = 0, cut = 0;
        for (int j = 0; j < bl && px < 440; ) {
            uint8_t ch = (uint8_t)line[j];
            if (ch < 0x80) { px += 17; j++; }
            else if ((ch & 0xE0) == 0xC0) { px += 17; j += 2; }
            else if ((ch & 0xF0) == 0xE0) { px += 17; j += 3; }
            else j++;
            cut = j;
        }
        if (cut < bl) {
            line[cut] = 0;
            strcat(line, "...");
        }
        lcd_draw_text(12, y + 10, line, s_th_fg, s_th_bg);
    }
    if (n == 0)
        text_center(130, "还没有收藏题目", s_th_border, s_th_bg);

    if (total_pages > 1) {                    /* 多页: 底部翻页按钮 */
        lcd_draw_rect(10, 230, 156, 264, s_th_border);
        text_center2(83, 234, "< 上页", s_fav_page > 0 ? s_th_fg : s_th_border, s_th_bg);
        lcd_draw_rect(162, 230, 308, 264, s_th_border);
        char pg[24];
        snprintf(pg, sizeof(pg), "%d/%d", s_fav_page + 1, total_pages);
        text_center2(235, 234, pg, s_th_fg, s_th_bg);
        lcd_draw_rect(314, 230, 470, 264, s_th_border);
        text_center2(392, 234, "下页 >", s_fav_page + 1 < total_pages ? s_th_fg : s_th_border, s_th_bg);
    } else {
        text_center(262, "点选查看 / 长按返回", s_th_border, s_th_bg);
    }
}

static void text_center2(int cx, int y, const char *s, uint16_t fg, uint16_t bg)
{
    lcd_draw_text(cx - text_width(s) / 2, y, s, fg, bg);
}

/* ---------- 答题页 ---------- */
#define OPT_Y0   118           /* 选项区起始 */
#define OPT_H    40
#define OPT_GAP  8

static void draw_option(int i)
{
    int x = (i % 2) ? 244 : 10, y = OPT_Y0 + (i / 2) * (OPT_H + OPT_GAP);
    int sel = (i == s_opt_sel && !s_answered);
    uint16_t bg = sel ? s_th_sel : s_th_bg;
    lcd_fill_rect(x, y, x + 226, y + OPT_H, bg);
    lcd_draw_rect(x, y, x + 226, y + OPT_H, sel ? s_th_bar : s_th_border);
    char label[4] = { 'A' + i, '.', ' ', 0 };
    lcd_draw_text(x + 6, y + 12, label, sel ? s_th_bar_fg : s_th_fg, bg);
}

/* 完整重画一个选项按钮 (背景+label+文字), 切换高亮时用 */
static void redraw_option_full(int i)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    int sel = (i == s_opt_sel && !s_answered);
    draw_option(i);
    int x = (i % 2) ? 284 : 50, y = OPT_Y0 + 10 + (i / 2) * (OPT_H + OPT_GAP);
    text_wrap(x, y, q->options[i], sel ? s_th_bar_fg : s_th_fg,
              sel ? s_th_sel : s_th_bg, 186, 16, 2);
}

static void draw_quiz(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    lcd_clear(s_th_bg);

    /* 顶栏 */
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char top[64];
    snprintf(top, sizeof(top), "%s  %d/%d", q->subject, s_qidx + 1, s_qcount);
    lcd_draw_text(8, 5, top, s_th_bar_fg, s_th_bar);
    if (s_qlist[0] != question_count + 1) {   /* 收藏按钮 (收藏题本身不显示) */
        lcd_draw_text(190, 5, fav_contains(q) ? "★已藏" : "☆收藏",
                      fav_contains(q) ? RED : s_th_bar_fg, s_th_bar);
    }
    draw_ip_bar(1);

    /* 题目 (最多 4 行, 避免压到选项区; 过长提示点开全文) */
    int ql = 0;
    text_wrap_skip(10, 30, q->content, s_th_fg, s_th_bg, 460, 18, 4, 0, &ql);
    if (ql > 4)
        lcd_draw_text(10, 102, "点击题目翻页", s_th_border, s_th_bg);

    if (q->is_choice) {
        for (int i = 0; i < 4; i++)
            draw_option(i);
        /* 选项文字 (限 2 行, 按钮内换行, 内容在 label 后) */
        for (int i = 0; i < 4; i++) {
            int x = (i % 2) ? 284 : 50, y = OPT_Y0 + 10 + (i / 2) * (OPT_H + OPT_GAP);
            int sel = (i == s_opt_sel && !s_answered);
            uint16_t fg = sel ? s_th_bar_fg : s_th_fg;
            text_wrap(x, y, q->options[i], fg, sel ? s_th_sel : s_th_bg, 186, 16, 2);
        }
        if (s_answered) {
            const char *res = (s_opt_sel == q->answer_idx) ? "回答正确" : "回答错误";
            lcd_draw_text(10, 214, res, (s_opt_sel == q->answer_idx) ? GREEN : RED, s_th_bg);
            if (s_qlist[0] != question_count + 1) {   /* 收藏按钮 */
                lcd_draw_rect(340, 212, 470, 230, s_th_border);
                lcd_draw_text(346, 215, fav_contains(q) ? "★已藏" : "☆收藏本题",
                              fav_contains(q) ? RED : s_th_fg, s_th_bg);
            }
            if (q->explanation && q->explanation[0]) {
                int el = 0;
                text_wrap_skip(10, 232, q->explanation, s_th_fg, s_th_bg,
                               460, 16, 2, 0, &el);
                if (el > 2)
                    text_center(262, "点击解析翻页", s_th_border, s_th_bg);
            }
        } else {
            text_center(230, "短按选答案  长按提交", s_th_border, s_th_bg);
        }
    } else {
        if (s_show_ans) {
            lcd_draw_text(10, 160, "参考答案:", s_th_fg, s_th_bg);
            text_wrap(10, 178, q->answer_text, s_th_bar, s_th_bg, 460, 16, 2);
            if (q->explanation && q->explanation[0]) {
                int el = 0;
                text_wrap_skip(10, 214, q->explanation, s_th_fg, s_th_bg,
                               460, 16, 2, 0, &el);
                if (el > 2)
                    lcd_draw_text(10, 246, "点击解析翻页", s_th_border, s_th_bg);
            }
            text_center(262, "短按下一题", s_th_border, s_th_bg);
        } else {
            lcd_draw_rect(10, 168, 470, 250, s_th_border);
            text_center(205, "解答题: 自己思考后查看答案", s_th_border, s_th_bg);
            text_center(222, "短按显示参考答案", s_th_border, s_th_bg);
        }
    }
}

static void ui_handle(int ev);
static void ui_submit(void);

/* ---------- 解析页 (全屏, 可翻页) ---------- */
#define EXP_LINES 12           /* 每页行数 */
#define FULL_LINES 12          /* 全文翻页: 每页行数 */

/* ---------- 翻页页公共框架: 顶栏标题 + 页码 + 动态底部提示 ----------
 * 交互约定: 左半屏=上一页 (首页=返回), 右半屏=下一页 (末页=返回) */
static void draw_page_chrome(const char *title, int page, int total_pages)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, title, s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    char top[48];
    snprintf(top, sizeof(top), "%d/%d", page + 1, total_pages);
    lcd_draw_text(420, 5, top, s_th_bar_fg, s_th_bar);
    if (total_pages <= 1)
        text_center(262, "点击返回", s_th_border, s_th_bg);
    else if (page == 0)
        text_center(262, "左返回 右翻页", s_th_border, s_th_bg);
    else if (page + 1 >= total_pages)
        text_center(262, "左翻上页 右返回", s_th_border, s_th_bg);
    else
        text_center(262, "左翻上页 右翻下页", s_th_border, s_th_bg);
}

static void draw_explain(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    /* 解答题: 参考答案+解析 合并分页; 选择题: 仅解析 */
    static char expbuf[2048];
    const char *src = q->explanation ? q->explanation : "";
    if (!q->is_choice) {
        snprintf(expbuf, sizeof(expbuf), "参考答案: %s\n解析: %s",
                 q->answer_text ? q->answer_text : "",
                 q->explanation ? q->explanation : "");
        src = expbuf;
    }
    /* 先统计总行数, 再画框架 (清屏), 最后画正文 */
    text_wrap_skip(10, -1, src, 0, 0, 460, 18, 0, 0, &s_exp_total);
    int total_pages = (s_exp_total + EXP_LINES - 1) / EXP_LINES;
    if (total_pages < 1) total_pages = 1;
    draw_page_chrome("解析", s_exp_page, total_pages);
    text_wrap_skip(10, 36, src, s_th_fg, s_th_bg, 460, 18,
                   EXP_LINES, s_exp_page * EXP_LINES, NULL);
}

/* ---------- 题目全文页 (长题翻页) ---------- */
static void draw_question_full(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    text_wrap_skip(10, -1, q->content, 0, 0, 460, 18, 0, 0, &s_qtotal);
    int total_pages = (s_qtotal + FULL_LINES - 1) / FULL_LINES;
    if (total_pages < 1) total_pages = 1;
    draw_page_chrome("题目全文", s_qpage, total_pages);
    text_wrap_skip(10, 36, q->content, s_th_fg, s_th_bg, 460, 18,
                   FULL_LINES, s_qpage * FULL_LINES, NULL);
}

/* ---------- 薄弱点总结全文页 (可翻页, 底部可切错题列表) ---------- */
static void draw_weak_full(void)
{
    const char *ai = weak_get_ai(s_weak_subj);
    text_wrap_skip(10, -1, ai, 0, 0, 460, 18, 0, 0, &s_wtotal);
    int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
    if (total_pages < 1) total_pages = 1;
    char t[48];
    snprintf(t, sizeof(t), "薄弱点: %s", s_subjects[s_weak_subj]);
    draw_page_chrome(t, s_wpage, total_pages);
    text_wrap_skip(10, 36, ai, s_th_fg, s_th_bg, 460, 18,
                   FULL_LINES, s_wpage * FULL_LINES, NULL);
    /* 底部左侧: [错题列表] 切换 */
    lcd_draw_rect(10, 250, 130, 268, s_th_border);
    text_center2(70, 254, "错题列表", s_th_fg, s_th_bg);
}

static const char *s_ai_subject = NULL;

/* AI 再出一题 (同科目); 失败进入 s_state 10 可重试 */
static void ai_next_question(void)
{
    if (!s_ai_subject) {
        s_state = 0;
        draw_menu();
        return;
    }
    lcd_clear(s_th_bg);
    char sbuf[64];
    snprintf(sbuf, sizeof(sbuf), "AI 出题中... (%s)", s_ai_subject);
    text_center(110, sbuf, s_th_fg, s_th_bg);
    text_center(140, "最长 60 秒, 请稍候", s_th_border, s_th_bg);
    if (ai_generate_question(s_ai_subject) == 0) {
        s_qcount = 1;
        s_qlist[0] = question_count;
        s_qidx = 0; s_opt_sel = 0; s_answered = 0; s_show_ans = 0;
        s_state = 1;
        draw_quiz();
    } else {
        lcd_clear(s_th_bg);
        text_center(100, "出题失败", RED, s_th_bg);
        text_center(130, "点击重试 / 长按返回", s_th_border, s_th_bg);
        s_state = 10;
    }
}

/* 答题结束 (最后一题): AI 再出 / 收藏回夹 / 本地回菜单 */
static void quiz_finished(void)
{
    if (s_qcount == 1 && s_qlist[0] == question_count) {
        ai_next_question();
    } else if (s_qcount == 1 && s_qlist[0] == question_count + 1) {
        s_state = 11;
        draw_fav_list();
    } else {
        s_state = 0;
        draw_menu();
    }
}

/* AI 出题: 科目选择页 */
static void draw_ai_subject(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "AI 出题: 选择科目", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    for (int i = 0; i < 9; i++) {
        int x = 10 + (i % 3) * 158, y = 40 + (i / 3) * 70;
        lcd_fill_rect(x, y, x + 148, y + 60, s_th_bg);
        lcd_draw_rect(x, y, x + 148, y + 60, s_th_border);
        text_center2(x + 74, y + 22, s_subjects[i], BLACK, s_th_bg);
    }
}

/* ---------- 触摸输入 (上升沿触发, 坐标已按 chip max 缩放) ---------- */
static void ui_touch(int sx, int sy)
{
    if (s_state == 0) {                     /* 菜单 */
        if (sx >= 10 && sx <= 160 && sy >= 238 && sy <= 264) {  /* AI 出题 */
            s_state = 8;
            draw_ai_subject();
            return;
        }
        if (sx >= 135 && sx <= 255 && sy >= 238 && sy <= 264) { /* 收藏 */
            s_state = 11;
            draw_fav_list();
            return;
        }
        if (sx >= 260 && sx <= 380 && sy >= 238 && sy <= 264) { /* 薄弱点 */
            s_state = 12;
            draw_weak_subject();
            return;
        }
        if (sx >= 385 && sx <= 470 && sy >= 238 && sy <= 264) {  /* 设置 */
            s_state = 4;
            draw_settings();
            return;
        }
        int col = (sx - 10) / 158, row = (sy - 46) / 64;
        if (col >= 0 && col < 3 && row >= 0 && row < 3) {
            s_menu_sel = row * 3 + col;
            ui_handle(2);                   /* 本地题目 */
        }
    } else if (s_state == 8) {              /* AI 科目选择 */
        int col = (sx - 10) / 158, row = (sy - 40) / 70;
        if (col >= 0 && col < 3 && row >= 0 && row < 3) {
            if (s_wifi_state != 2) {        /* WiFi 未连接 */
                lcd_clear(s_th_bg);
                text_center(100, "请先连接 WiFi", RED, s_th_bg);
                text_center(130, "在设置页连接网络", s_th_border, s_th_bg);
                text_center(150, "点击返回", s_th_border, s_th_bg);
                s_state = 10;
                return;
            }
            s_ai_subject = s_subjects[row * 3 + col];
            ai_next_question();
        }
    } else if (s_state == 10) {             /* AI 失败提示: 点击重试 */
        if (s_ai_subject && s_wifi_state == 2)
            ai_next_question();
        else {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 11) {             /* 收藏夹: 点选查看 / 底部翻页 */
        int n = fav_count();
        int total_pages = (n + 5 - 1) / 5;
        if (total_pages < 1) total_pages = 1;
        if (total_pages > 1 && sy >= 230) {  /* 底部翻页按钮 */
            if (sx < 156 && s_fav_page > 0) {
                s_fav_page--;
                draw_fav_list();
            } else if (sx >= 314 && s_fav_page + 1 < total_pages) {
                s_fav_page++;
                draw_fav_list();
            }
            return;
        }
        if (sy >= 30 && sy <= 218) {
            int i = s_fav_page * 5 + (sy - 30) / 38;
            if (i >= 0 && i < n) {
                if (fav_get(i) == 0) {
                    s_qcount = 1;
                    s_qlist[0] = question_count + 1;   /* 收藏题 */
                    s_qidx = 0; s_opt_sel = 0; s_answered = 0; s_show_ans = 0;
                    s_state = 1;
                    draw_quiz();
                }
            }
        }
    } else if (s_state == 12) {             /* 薄弱点选科 */
        int col = (sx - 10) / 158, row = (sy - 40) / 70;
        if (col >= 0 && col < 3 && row >= 0 && row < 3) {
            s_weak_subj = row * 3 + col;
            if (weak_get_ai(s_weak_subj)[0]) {   /* 已有总结: 直接全屏 */
                s_wpage = 0;
                s_state = 16;
                draw_weak_full();
            } else {
                s_state = 13;
                draw_weak_page();
            }
        }
    } else if (s_state == 13) {             /* 薄弱点错题页: 左右翻页 */
        if (sy >= 240) {
            if (sx < 156) {                 /* AI 总结 (无总结时无效) */
                if (weak_get_ai(s_weak_subj)[0]) {
                    s_wpage = 0;
                    s_state = 16;
                    draw_weak_full();
                }
            } else if (sx < 308) {          /* AI 分析 */
                weak_ai_run();
            } else {                        /* 返回 */
                s_state = 0;
                draw_menu();
            }
            return;
        }
        int n = weak_count(s_weak_subj);
        int total_pages = (n + 4 - 1) / 4;
        if (total_pages < 1) total_pages = 1;
        /* 总结预览区点击 → 全屏 */
        const char *ai = weak_get_ai(s_weak_subj);
        if (ai[0]) {
            int shown = n - s_weak_list_page * 4;
            if (shown > 4) shown = 4;
            if (shown < 0) shown = 0;
            int ly = 46 + shown * 30;
            if (n > 4 || n == 0) ly += 18;
            if (sy >= ly + 8 && sy <= ly + 55) {
                s_wpage = 0;
                s_state = 16;
                draw_weak_full();
                return;
            }
        }
        /* 列表区左右半屏翻页 */
        if (sx < 240) {
            if (s_weak_list_page > 0) {
                s_weak_list_page--;
                draw_weak_page();
            }
        } else {
            if (s_weak_list_page + 1 < total_pages) {
                s_weak_list_page++;
                draw_weak_page();
            }
        }
    } else if (s_state == 14) {             /* 薄弱点失败: 点击返回 */
        s_state = 13;
        draw_weak_page();
    } else if (s_state == 1) {              /* 答题 */
        const quiz_q_t *q = get_question(s_qlist[s_qidx]);
        if (sy < 26) {
            if (sx > 330) {                 /* 顶栏右侧: 返回 */
                s_state = 0;
                draw_menu();
                return;
            }
            if (sx >= 170 && sx <= 260 && s_qlist[0] != question_count + 1) {
                /* ★ 收藏/取消收藏 */
                if (fav_contains(q)) {
                    for (int i = 0; i < fav_count(); i++) {
                        if (fav_get(i) == 0 &&
                            strcmp(g_fav_q.content, q->content) == 0) {
                            fav_remove(i);
                            break;
                        }
                    }
                } else {
                    fav_add(q);
                }
                draw_quiz();
                return;
            }
        }
        /* 题目区: 长题点开全文翻页阅读 */
        if (sy >= 30 && sy <= 117) {
            int ql = 0;
            text_wrap_skip(10, -1, q->content, 0, 0, 460, 18, 4, 0, &ql);
            if (ql > 4) {
                s_qpage = 0;
                s_state = 15;
                draw_question_full();
                return;
            }
        }
        if (q->is_choice) {
            if (sy >= 118 && sy <= 206) {   /* 选项区: 点哪个答哪个 */
                int i = (sy >= 166) ? 2 : 0;
                i += (sx >= 244) ? 1 : 0;
                if (!s_answered) {
                    s_opt_sel = i;
                    ui_submit();
                }
            } else if (s_answered && sy >= 210 && sy <= 232 && sx > 330
                       && s_qlist[0] != question_count + 1) {
                /* 收藏本题 (答完按钮) */
                if (fav_contains(q)) {
                    for (int i = 0; i < fav_count(); i++) {
                        if (fav_get(i) == 0 &&
                            strcmp(g_fav_q.content, q->content) == 0) {
                            fav_remove(i);
                            break;
                        }
                    }
                } else {
                    fav_add(q);
                }
                draw_quiz();
            } else if (s_answered && sy > 214) {  /* 解析区: 进入解析页 */
                s_exp_page = 0;
                s_state = 3;
                draw_explain();
            }
        } else {                            /* 解答题 */
            if (sy > 160) {
                if (!s_show_ans) { s_show_ans = 1; draw_quiz(); }
                else if (sy >= 214 && q->explanation && q->explanation[0]) {
                    /* 解析过长: 进入解析页 (参考答案+解析 合并翻页) */
                    int el = 0;
                    text_wrap_skip(10, -1, q->explanation, 0, 0, 460, 16, 2, 0, &el);
                    if (el > 2) {
                        s_exp_page = 0;
                        s_state = 3;
                        draw_explain();
                        return;
                    }
                    if (s_qidx == s_qcount - 1) { quiz_finished(); }
                    else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; draw_quiz(); }
                } else if (s_qidx == s_qcount - 1) { quiz_finished(); }
                else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; draw_quiz(); }
            }
        }
    } else if (s_state == 3) {              /* 解析页: 左翻上/右翻下, 边界返回 */
        int total_pages = (s_exp_total + EXP_LINES - 1) / EXP_LINES;
        if (sx < 240) {                     /* 左半: 上一页 (首页=返回答题) */
            if (s_exp_page > 0) {
                s_exp_page--;
                draw_explain();
            } else {
                s_state = 1;
                draw_quiz();
            }
        } else {                            /* 右半: 下一页 (末页=下一题) */
            if (s_exp_page + 1 < total_pages) {
                s_exp_page++;
                draw_explain();
            } else {
                if (s_qidx == s_qcount - 1) { quiz_finished(); }
                else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_state = 1; draw_quiz(); }
            }
        }
    } else if (s_state == 15) {             /* 题目全文: 左翻上/右翻下, 边界返回 */
        int total_pages = (s_qtotal + FULL_LINES - 1) / FULL_LINES;
        if (sx < 240) {
            if (s_qpage > 0) {
                s_qpage--;
                draw_question_full();
            } else {
                s_state = 1;
                draw_quiz();
            }
        } else {
            if (s_qpage + 1 < total_pages) {
                s_qpage++;
                draw_question_full();
            } else {
                s_state = 1;
                draw_quiz();
            }
        }
    } else if (s_state == 16) {             /* 总结全文: 左翻上/右翻下, [错题列表] */
        if (sy >= 250 && sx <= 130) {       /* 底部 [错题列表] 按钮 */
            s_state = 13;
            draw_weak_page();
            return;
        }
        int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
        if (sx < 240) {
            if (s_wpage > 0) {
                s_wpage--;
                draw_weak_full();
            } else {
                s_state = 13;
                draw_weak_page();
            }
        } else {
            if (s_wpage + 1 < total_pages) {
                s_wpage++;
                draw_weak_full();
            } else {
                s_state = 13;
                draw_weak_page();
            }
        }
    } else if (s_state == 4) {              /* 设置列表 */
        if (sy >= 244) {                    /* 底部按钮 */
            if (sx < 240) {                 /* 扫描 WiFi */
                lcd_clear(s_th_bg);
                text_center(130, "扫描中...", s_th_border, s_th_bg);
                wifi_scan();
                s_state = 6;
                draw_ap_list();
            } else {                        /* 手动输入 SSID */
                s_kb_field = 0;
                snprintf(s_kb_buf, sizeof(s_kb_buf), "%s", s_wifi_ssid);
                s_kb_shift = 0;
                s_kb_sym = 0;
                s_state = 5;
                draw_keyboard();
            }
        } else if (sy >= 188 && sy <= 214) { /* 亮度: 左减右加 */
            if (sx < 240) {
                if (s_brightness > 5) {
                    backlight_set(s_brightness - 10);
                    settings_save();
                }
            } else {
                if (s_brightness < 100) {
                    backlight_set(s_brightness + 10);
                    settings_save();
                }
            }
            draw_settings();
        } else if (sy >= 158 && sy <= 184) { /* 主题: 点击切换 */
            theme_apply((s_theme + 1) % 3);
            settings_save();
            draw_settings();
        } else if (sy >= 218 && sy <= 242) { /* 年级: 点击切换 */
            s_grade = (s_grade + 1) % 3;
            settings_save();
            draw_settings();
        } else if (sy >= 34 && sy <= 154) {
            int i = (sy - 34) / 40;
            if (i == 2) {                   /* API Key: 网页输入 */
                if (s_wifi_state == 2)
                    http_server_start();
                s_state = 7;
                draw_api_page();
            } else if (i >= 0 && i < 2) {
                s_kb_field = i;
                snprintf(s_kb_buf, sizeof(s_kb_buf), "%s",
                         i == 0 ? s_wifi_ssid : s_wifi_pass);
                s_kb_shift = 0;
                s_kb_sym = 0;
                s_state = 5;
                draw_keyboard();
            }
        }
    } else if (s_state == 6) {              /* WiFi 列表: 点选 AP */
        if (sy >= 30 && s_ap_count > 0) {
            int i = (sy - 30) / 32;
            if (i >= 0 && i < s_ap_count && i < 7) {
                memcpy(s_wifi_ssid, s_ap_list[i].ssid, 32);
                s_wifi_ssid[32] = 0;
                s_kb_field = 1;             /* 输入密码 */
                snprintf(s_kb_buf, sizeof(s_kb_buf), "%s", s_wifi_pass);
                s_kb_shift = 0;
                s_kb_sym = 0;
                s_state = 5;
                draw_keyboard();
            }
        }
    } else if (s_state == 5) {              /* 键盘 */
        int r = kb_touch(sx, sy);
        if (r == 1 || r == 3) {
            /* 局部刷新: 输入框 (+shift 键) — 打字跟手 */
            lcd_fill_rect(6, 30, 474, 44, s_th_bg);
            lcd_draw_rect(6, 30, 474, 44, s_th_border);
            int blen = strlen(s_kb_buf);
            const char *disp = s_kb_buf;
            if (blen > 28)
                disp = s_kb_buf + blen - 28;
            lcd_draw_text(10, 31, disp, s_th_fg, s_th_bg);
            if (r == 3 && !s_kb_sym) {
                int sx0 = 2, sy0 = KB_Y0 + 4 * KB_H + 2;
                lcd_fill_rect(sx0, sy0, sx0 + KB_W - 4, sy0 + KB_H - 4,
                              s_kb_shift ? s_th_sel : s_th_sel);
                lcd_draw_rect(sx0, sy0, sx0 + KB_W - 4, sy0 + KB_H - 4, s_th_border);
                lcd_draw_char(sx0 + (KB_W - 14) / 2, sy0 + 8, '^', BLACK,
                              s_kb_shift ? s_th_sel : s_th_sel);
            }
        } else if (r == 2) {
            s_state = 4;
            draw_settings();
        } else if (r == 4) {
            draw_keyboard();
        }
    } else if (s_state == 2) {
        s_state = 0;
        draw_menu();
    }
}

/* ---------- 按键 (BOOT) ---------- */
static int btn_scan(uint32_t *press_ms)
{
    /* 返回 1=短按, 2=长按, 0=无; press_ms 累计按下时间 */
    static int was_down = 0;
    static uint32_t down_at = 0;
    int down = (gpio_get_level(BTN_GPIO) == 0);
    if (down && !was_down) {
        down_at = esp_timer_get_time() / 1000;
        was_down = 1;
    } else if (!down && was_down) {
        was_down = 0;
        uint32_t dur = esp_timer_get_time() / 1000 - down_at;
        return dur >= 900 ? 2 : 1;
    }
    if (was_down)
        *press_ms = esp_timer_get_time() / 1000 - down_at;
    return 0;
}

static void ui_submit(void);

/* ---------- UI 状态机 ---------- */
static void ui_handle(int ev)
{
    if (s_state == 0) {                 /* 菜单 */
        if (ev == 1) {                  /* 短按: 下一个科目 */
            s_menu_sel = (s_menu_sel + 1) % 9;
            draw_menu();
        } else if (ev == 2) {           /* 长按: 进入科目 */
            s_qcount = 0;
            for (int i = 0; i < question_count; i++) {
                const quiz_q_t *q = get_question(i);
                if (strcmp(q->subject, s_subjects[s_menu_sel]) == 0 && s_qcount < 16)
                    s_qlist[s_qcount++] = i;
            }
            if (s_qcount == 0) {
                lcd_clear(s_th_bg);
                text_center(120, "该科目暂无题目", RED, s_th_bg);
                text_center(150, "长按返回", s_th_border, s_th_bg);
                s_state = 2;            /* 空科目提示态 */
            } else {
                s_qidx = 0; s_opt_sel = 0; s_answered = 0; s_show_ans = 0;
                s_state = 1;
                draw_quiz();
            }
        }
    } else if (s_state == 1) {          /* 答题 */
        const quiz_q_t *q = get_question(s_qlist[s_qidx]);
        if (ev == 2) {                  /* 长按 */
            if (q->is_choice && !s_answered) {
                ui_submit();            /* 提交答案 */
            } else if (s_qidx == s_qcount - 1) {
                quiz_finished();} else {
                s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0;
                draw_quiz();
            }
        } else if (ev == 1) {           /* 短按 */
            if (q->is_choice) {
                if (!s_answered) {
                    s_opt_sel = (s_opt_sel + 1) % 4;
                    redraw_option_full(s_opt_sel);           /* 新选中 */
                    redraw_option_full((s_opt_sel + 3) % 4); /* 旧选中 */
                } else {
                    if (s_qidx == s_qcount - 1) {
                        quiz_finished();} else {
                        s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0;
                        draw_quiz();
                    }
                }
            } else {
                if (!s_show_ans) {
                    s_show_ans = 1;
                    draw_quiz();
                } else {
                    if (s_qidx == s_qcount - 1) {
                        quiz_finished();} else {
                        s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0;
                        draw_quiz();
                    }
                }
            }
        }
    } else if (s_state == 2) {          /* 空科目提示 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 3) {          /* 解析页: 短按翻页, 长按返回 */
        int total_pages = (s_exp_total + EXP_LINES - 1) / EXP_LINES;
        if (ev == 2) {
            if (s_qidx == s_qcount - 1) { quiz_finished(); }
            else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_state = 1; draw_quiz(); }
        } else if (ev == 1) {
            if (s_exp_page + 1 < total_pages) {
                s_exp_page++;
                draw_explain();
            } else {
                if (s_qidx == s_qcount - 1) { quiz_finished(); }
                else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_state = 1; draw_quiz(); }
            }
        }
    } else if (s_state == 15) {         /* 题目全文: 短按翻页, 长按返回 */
        int total_pages = (s_qtotal + FULL_LINES - 1) / FULL_LINES;
        if (ev == 2) {
            s_state = 1;
            draw_quiz();
        } else if (ev == 1) {
            if (s_qpage + 1 < total_pages) {
                s_qpage++;
                draw_question_full();
            } else {
                s_state = 1;
                draw_quiz();
            }
        }
    } else if (s_state == 16) {         /* 总结全文: 短按翻页, 长按返回 */
        int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
        if (ev == 2) {
            s_state = 13;
            draw_weak_page();
        } else if (ev == 1) {
            if (s_wpage + 1 < total_pages) {
                s_wpage++;
                draw_weak_full();
            } else {
                s_state = 13;
                draw_weak_page();
            }
        }
    } else if (s_state == 4) {          /* 设置列表: 长按返回 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 5) {          /* 键盘: 长按返回设置 */
        if (ev == 2) {
            s_state = 4;
            draw_settings();
        }
    } else if (s_state == 6) {          /* WiFi 列表: 长按返回设置 */
        if (ev == 2) {
            s_state = 4;
            draw_settings();
        }
    } else if (s_state == 7) {          /* API 页: 长按返回设置 */
        if (ev == 2) {
            s_state = 4;
            draw_settings();
        }
    } else if (s_state == 8) {          /* AI 科目选择: 长按返回 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 10) {         /* AI 失败提示: 长按返回 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 11) {         /* 收藏夹: 长按返回 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 12 || s_state == 13 || s_state == 14) { /* 薄弱点: 长按返回 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    }
}

/* 提交答案 (长按选项时调用), 局部刷新提速 */
static void ui_submit(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    if (!q->is_choice || s_answered)
        return;
    s_answered = 1;
    s_total++;
    if (s_opt_sel == q->answer_idx) {
        s_correct++;
    } else {
        /* 答错: 记录到该科薄弱点 */
        for (int i = 0; i < 9; i++) {
            if (strcmp(q->subject, s_subjects[i]) == 0) {
                weak_add_wrong(i, q->content);
                break;
            }
        }
    }

    /* 只刷新选项+答案区 (题目/顶栏不动; 长题补画翻页提示) */
    lcd_fill_rect(0, 104, 479, 271, s_th_bg);
    int ql = 0;
    text_wrap_skip(10, -1, q->content, 0, 0, 460, 18, 4, 0, &ql);
    if (ql > 4)
        lcd_draw_text(10, 102, "点击题目翻页", s_th_border, s_th_bg);
    for (int i = 0; i < 4; i++)
        draw_option(i);
    for (int i = 0; i < 4; i++) {
        int x = (i % 2) ? 284 : 50, y = OPT_Y0 + 10 + (i / 2) * (OPT_H + OPT_GAP);
        text_wrap(x, y, q->options[i], BLACK, s_th_bg, 186, 16, 2);
    }
    const char *res = (s_opt_sel == q->answer_idx) ? "回答正确" : "回答错误";
    lcd_draw_text(10, 214, res, (s_opt_sel == q->answer_idx) ? GREEN : RED, s_th_bg);
    if (q->explanation && q->explanation[0]) {
        int el = 0;
        text_wrap_skip(10, 232, q->explanation, s_th_fg, s_th_bg, 460, 16, 2, 0, &el);
        if (el > 2)
            text_center(262, "点击解析翻页", s_th_border, s_th_bg);
    }
}

/* ---------- 测试画面 ---------- */
static void gfx_test(void)
{
    lcd_clear(s_th_bg);

    /* 色块 */
    lcd_fill_rect(10, 10, 100, 60, RED);
    lcd_fill_rect(110, 10, 200, 60, GREEN);
    lcd_fill_rect(210, 10, 300, 60, s_th_bar);
    lcd_fill_rect(310, 10, 400, 60, YELLOW);
    lcd_fill_rect(410, 10, 470, 60, CYAN);

    /* 网格线 */
    for (int x = 10; x < 480; x += 40)
        lcd_draw_vline(x, 80, 180, s_th_border);
    for (int y = 80; y <= 180; y += 20)
        lcd_draw_hline(10, 470, y, s_th_border);

    /* 矩形边框 */
    lcd_draw_rect(10, 200, 150, 260, RED);
    lcd_draw_rect(160, 200, 300, 260, GREEN);
    lcd_draw_rect(310, 200, 470, 260, s_th_bar);

    /* ASCII 文字 */
    lcd_draw_str(10, 205, "Hello ESP32!", s_th_fg, s_th_bg);
    lcd_draw_str(160, 205, "ST6201 4.3in", s_th_fg, s_th_bg);
    lcd_draw_str(310, 205, "480x272 SPI", s_th_fg, s_th_bg);

    /* 中文渲染测试 */
    lcd_draw_text(10, 90, "中文渲染测试", RED, s_th_bg);
    lcd_draw_text(10, 108, "物理 化学 生物 数学", s_th_fg, s_th_bg);
    lcd_draw_text(10, 126, "题目: 自由落体运动时间", s_th_fg, s_th_bg);
    lcd_draw_text(10, 144, "A. 2秒  B. 3秒  C. 4秒", s_th_fg, s_th_bg);
    lcd_draw_text(10, 162, "解析: 根据 h=1/2gt^2 计算", GREEN, s_th_bg);

    ESP_LOGI(TAG, "gfx test done");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ST6201 GFX test start");

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST) | (1ULL << PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 26000000,        /* GPIO matrix 上限 26.6MHz (实测 40MHz 不支持) */
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    lcd_init();
    backlight_init();      /* 背光 PWM (主题/亮度) */

    /* NVS (设置存储) */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();       /* 主题/亮度从 NVS 恢复 */
    theme_apply(s_theme);
    backlight_set(s_brightness);
    touch_init();          /* 初始化触摸 (坐标由校准映射直接使用) */
    fav_init();            /* 收藏分区初始化 */
    fav_debug();           /* 调试 */
    weak_init();           /* 薄弱点分区初始化 */
    wifi_init();           /* WiFi: 有保存的配置则自动连接 */

    /* ========== 刷题机 (触摸 + BOOT 键双输入) ========== */
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    lcd_clear(s_th_bg);
    draw_menu();
    ESP_LOGI(TAG, "quiz start: %d questions, touch %s",
             question_count, touch_init_done ? "ready" : "N/A");

    /* 触摸: 松开触发点击, 按住 900ms 触发长按 (返回) */
    int prev_touch = 0;
    uint32_t t_press_at = 0;
    int t_px = 0, t_py = 0, t_long_sent = 0;
    while (1) {
        int tx, ty;
        int tnow = touch_read(&tx, &ty);
        uint32_t now = esp_timer_get_time() / 1000;
        if (tnow && !prev_touch) {
            t_px = tx;
            t_py = ty;
            t_press_at = now;
            t_long_sent = 0;
        } else if (!tnow && prev_touch && !t_long_sent) {
            int sx = t_px * LCD_WIDTH / s_xmax;
            int sy = t_py * LCD_HEIGHT / s_ymax;
            ESP_LOGI(TAG, "tap %d,%d", sx, sy);
            ui_touch(sx, sy);
        } else if (tnow && prev_touch && !t_long_sent
                   && now - t_press_at >= 900
                   && (s_state == 4 || s_state == 6 || s_state == 7
                       || s_state == 8 || s_state == 10 || s_state == 11
                       || s_state == 12 || s_state == 13 || s_state == 14
                       || s_state == 15 || s_state == 16)) {
            t_long_sent = 1;
            ESP_LOGI(TAG, "touch long press");
            ui_handle(2);           /* 触摸长按 = 返回 */
        }
        prev_touch = tnow;

        /* BOOT 键兜底 */
        uint32_t press_ms = 0;
        int ev = btn_scan(&press_ms);
        if (ev)
            ui_handle(ev);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
