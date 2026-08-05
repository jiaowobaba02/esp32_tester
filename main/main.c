/**
 * ST6201 4.3寸 480x272 IPS —— ETSP32 SPI 驱动 + 基础图形层
 *
 * 官方配置: SPI2_HOST + DMA, CLK=23 MOSI=19 CS=22 DC=14 RST=12 (软件 CS)
 * mode 0, 26MHz (GPIO matrix 上限), 像素高字节先 (大端)
 * 背光: GPIO2 + GPIO32(飞线) 高电平开
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "gfx_driver.h"
#include "ui_renderer.h"
#include "ui_keyboard.h"

/* ---- 兼容别名: 旧 lcd_* 名字 → ui_renderer (迁移过渡期) ---- */
#define lcd_fill_rect  r_fill_rect
#define lcd_clear      r_clear
#define lcd_draw_hline r_draw_hline
#define lcd_draw_vline r_draw_vline
#define lcd_draw_rect  r_draw_rect
#define lcd_draw_text  r_draw_text
#define lcd_draw_char  r_draw_char_bw16
#define lcd_draw_cn_char r_draw_cn_char
#define text_width     r_text_width

#define PIN_BL    2
#define PIN_BL2   32

/* GT911 触摸 (官方配置) */
#define TOUCH_SDA  18
#define TOUCH_SCL  16
#define TOUCH_RST  4
#define TOUCH_INT  17

static const char *TAG = "st6201";
/* ================================================================
 * GT911 触摸 — 硬件 I2C (400kHz, 已验证稳定)
 * ================================================================ */
static int touch_init_done = 0;
static uint16_t s_xmax = LCD_WIDTH, s_ymax = LCD_HEIGHT;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

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

/* 科目名 → 索引 (0-8), 未知返回 -1 */
static int subj_of(const char *name)
{
    for (int i = 0; i < 9; i++)
        if (strcmp(name, s_subjects[i]) == 0)
            return i;
    return -1;
}

static int s_state = 0;        /* 0=菜单 1=答题 */
static int s_menu_sel = 0;
static int s_qidx = 0;         /* 科目内题号 */
static int s_qlist[16];        /* 科目内题目索引 */
static int s_qcount = 0;
static int s_opt_sel = 0;      /* 高亮选项 */
static int s_answered = 0;
static int s_show_ans = 0;     /* 非选择题: 已显示参考答案 */
static int s_correct = 0, s_total = 0;
static char s_fill_buf[64] = "";   /* 填空题: 键盘输入的答案 */
static int s_fill_ok = 0;          /* 填空题: 判分结果 */
static int s_exp_page = 0, s_exp_total = 0;   /* 解析页: 页码/总页 */
static int s_qpage = 0, s_qtotal = 0;         /* 题目全文页: 页码/总行 */
static int s_wpage = 0, s_wtotal = 0;         /* 薄弱点总结页: 页码/总行 */
static int s_fav_page = 0;                    /* 收藏列表: 页码 */
static int s_weak_list_page = 0;              /* 薄弱点错题列表: 页码 */
static int s_opt_page = 0, s_opt_pages = 1;   /* 答题页选项分页: 页码/总页 */
static int s_kb_page = 0;                     /* 知识库全文页: 页码 */

/* 全屏翻页: 每页行数 (解析/全文/薄弱点/知识库/收藏分析共用) */
#define EXP_LINES 12
#define FULL_LINES 12

static void text_center(int y, const char *s, uint16_t fg, uint16_t bg);
static void text_center2(int cx, int y, const char *s, uint16_t fg, uint16_t bg);
static void http_server_start(void);

/* 设置数据 (WiFi/API 等, 定义在此供 WiFi 代码使用) */
static char s_wifi_ssid[33] = "";
static char s_wifi_pass[65] = "";
char s_api_key[65] = "";         /* 全局: ai_quiz.c 使用 (正式版无内置 Key,
                                    * 需联网后访问 http://IP:8080 网页输入) */
int s_wifi_state = 0;             /* 0=未连接 1=连接中 2=已连接 (ai_quiz.c 使用) */
static char s_wifi_ip[17] = "";
static char s_time_str[16] = "";  /* 顶栏时间 "MM-DD HH:MM" (SNTP 同步后非空) */
int s_grade = 2;                 /* 年级: 0=高一 1=高二 2=高三 (ai_quiz.c 使用) */
static const char *s_grade_names[3] = { "高一", "高二", "高三" };
int s_diff = 1;                  /* AI 出题难度: 0=基础 1=中等 2=较难 (ai_quiz.c 使用) */
static const char *s_diff_names[3] = { "基础", "中等", "较难" };
int s_fill_pct = 20;             /* AI 出题填空概率 % (数理化英, ai_quiz.c 使用) */
static char s_kb_custom[40] = "";   /* 知识库自定义主题名 */
static const char *s_field_names[4] = { "WiFi 名称", "WiFi 密码", "API 密钥", "知识库主题" };

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
        s_th_bar_fg = 0xFFFF; s_th_sel = 0x28A8;
        s_th_border = 0x8430;   /* 中灰: 0x6B6D 在黑底上对比度太低 */
        break;
    default: /* 明亮 */
        s_th_bg = 0xFFFF; s_th_fg = 0x0000; s_th_bar = 0x001F;
        s_th_bar_fg = 0xFFFF; s_th_sel = 0x5D7C; s_th_border = 0x8430;
    }
    kb_set_theme(s_th_bg, s_th_fg, s_th_bar, s_th_bar_fg, s_th_sel, s_th_border);
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
    static uint16_t buf[35];               /* RGB565, gfx_push_pixels 转大端 */
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            buf[row * 5 + col] = ((g[col] >> row) & 1) ? fg : bg;
    gfx_set_window(x, y, x + 4, y + 6);
    gfx_push_pixels(buf, 35);
}

static void draw_small_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    gfx_hold_begin();                      /* 连续小字符共享 CS */
    while (*s) {
        draw_small_char(x, y, *s++, fg, bg);
        x += 6;
    }
    gfx_hold_end();
}

/* 返回键图形 (左箭头, 26x16) */
static void draw_back_icon(int x, int y)
{
    uint16_t c = s_th_bar_fg;
    lcd_fill_rect(x + 5, y + 6, x + 26, y + 9, c);      /* 杆 */
    for (int i = 0; i < 6; i++)                          /* 三角尖 */
        lcd_fill_rect(x + i, y + 2 + i, x + i + 1, y + 13 - i, c);
}

/* 顶部栏: 返回键图形 (右上角) + 右侧网络状态 (小字) */
static void draw_ip_bar(int show_back)
{
    if (show_back)
        draw_back_icon(452, 4);      /* 右上角, 避开中间标题/IP */
    if (s_time_str[0])               /* SNTP 已同步: 顶栏时间 (240..330) */
        draw_small_str(240, 4, s_time_str, s_th_bar_fg, s_th_bar);
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
#include "esp_sntp.h"        /* 联网后 NTP 校时 */
#include <time.h>

#define MAX_AP 16
static wifi_ap_record_t s_ap_list[MAX_AP];
static int s_ap_count = 0;

static uint32_t s_last_reconnect = 0;

/* ---------- SNTP 校时 (联网后自动同步, 顶栏显示 MM-DD HH:MM) ---------- */
static void time_sync_start(void)
{
    static int started = 0;
    if (started)
        return;
    started = 1;
    setenv("TZ", "CST-8", 1);          /* 中国时区 UTC+8, 无夏令时 */
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(NULL);
    esp_sntp_init();
    ESP_LOGI(TAG, "sntp started");
}

/* 刷新顶栏时间字符串 (仅在 SNTP 已同步后有效) */
static void time_refresh(void)
{
    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
        return;                        /* 未同步: 保持空白 */
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    if (!tm)
        return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d",
             tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    strncpy(s_time_str, buf, sizeof(s_time_str) - 1);
    s_time_str[sizeof(s_time_str) - 1] = 0;
}

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
        time_sync_start();                 /* 联网后 NTP 校时 */
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

/* 调试接口: POST /aitest  body=主题名 → 触发一次知识库生成并返回结果 (诊断用) */
static esp_err_t http_ai_test_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) buf[ret] = 0;
    else buf[0] = 0;
    char topic[64];
    snprintf(topic, sizeof(topic), "%.40s", buf[0] ? buf : "PCR原理");
    ESP_LOGI(TAG, "aitest: topic='%s' heap=%lu", topic,
             (unsigned long)esp_get_free_heap_size());
    static char out[4300];          /* httpd 任务栈小, 用静态缓冲 */
    if (strncmp(buf, "err", 3) == 0) {   /* 查最近一次 AI 错误 (诊断用, 免串口) */
        snprintf(out, sizeof(out), "%s", ai_last_error());
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, out, strlen(out));
    }
    if (strncmp(buf, "trans:", 6) == 0) {   /* 翻译测试: body=trans:拼音 */
        const char *cn = ai_translate_topic("生物", buf + 6);
        snprintf(out, sizeof(out), "%.4000s", cn[0] ? cn : "(trans failed)");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, out, strlen(out));
    }
    if (strncmp(buf, "aiq:", 4) == 0) {   /* 出题测试: body=aiq:数学 (诊断用) */
        int rc = ai_generate_question(buf[4] ? buf + 4 : "数学");
        if (rc == 0 && g_ai_q.content && g_ai_q.content[0])
            snprintf(out, sizeof(out), "OK [%s] %s",
                     g_ai_q.is_choice == 1 ? "选择" : "填空", g_ai_q.content);
        else
            snprintf(out, sizeof(out), "FAIL: %s", ai_last_error());
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, out, strlen(out));
    }
    const char *kb = ai_get_knowledge("生物", topic);
    snprintf(out, sizeof(out), "%.4000s", kb[0] ? kb : "(FAILED - check serial log)");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, out, strlen(out));
}

static void http_server_start(void)
{
    if (s_httpd)
        return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;
    cfg.stack_size = 8192;             /* /aitest 里跑 AI 请求, 需要更大栈 */
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        httpd_uri_t g = { .uri = "/", .method = HTTP_GET, .handler = http_get_handler,
                          .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &g);
        httpd_uri_t p = { .uri = "/api", .method = HTTP_POST, .handler = http_post_handler,
                          .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &p);
        httpd_uri_t t = { .uri = "/aitest", .method = HTTP_POST, .handler = http_ai_test_handler,
                          .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &t);
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
        else if (strncmp(s_api_key, "sk-", 3) != 0)
            s_api_key[0] = 0;   /* 防坏值: DeepSeek key 必须以 sk- 开头 */
        int32_t v = 0;
        if (nvs_get_i32(h, "theme", &v) == ESP_OK && v >= 0 && v <= 2)
            s_theme = v;
        if (nvs_get_i32(h, "bright", &v) == ESP_OK && v >= 1 && v <= 100)
            s_brightness = v;
        if (nvs_get_i32(h, "grade", &v) == ESP_OK && v >= 0 && v <= 2)
            s_grade = v;
        if (nvs_get_i32(h, "diff", &v) == ESP_OK && v >= 0 && v <= 2)
            s_diff = v;
        if (nvs_get_i32(h, "fill", &v) == ESP_OK && v >= 0 && v <= 100)
            s_fill_pct = v;
        nvs_close(h);
    }
    /* 无内置 Key: NVS 无 Key 时保持空, AI 功能会提示先联网网页输入 */
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
        nvs_set_i32(h, "diff", s_diff);
        nvs_set_i32(h, "fill", s_fill_pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- 软键盘 ---------- */
static void draw_settings(void)
{
    static const char *tnames[3] = { "明亮", "护眼", "夜间" };

    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "设置", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    /* 4 项: WiFi 名 / 密码 / API / 填空比例 (紧凑布局) */
    const char *vals[3] = { s_wifi_ssid, s_wifi_pass, s_api_key };
    for (int i = 0; i < 4; i++) {
        int y = 30 + i * 30;
        lcd_draw_rect(10, y, 470, y + 28, s_th_border);
        if (i == 3) {                   /* 填空比例: 左减右加 */
            lcd_draw_text(20, y + 2, "填空比例", s_th_fg, s_th_bg);
            char fb[32];
            snprintf(fb, sizeof(fb), "%d%%", s_fill_pct);
            lcd_draw_text(20, y + 15, fb, s_th_border, s_th_bg);
            lcd_draw_text(380, y + 15, "[左减右加]", s_th_bar, s_th_bg);
            continue;
        }
        lcd_draw_text(20, y + 2, s_field_names[i], s_th_fg, s_th_bg);
        char vbuf[80];
        if (i == 1 && s_wifi_pass[0])
            snprintf(vbuf, sizeof(vbuf), "%s", "******");
        else
            snprintf(vbuf, sizeof(vbuf), "%s", vals[i]);
        lcd_draw_text(20, y + 15, vbuf[0] ? vbuf : "(未设置)", s_th_border, s_th_bg);
        lcd_draw_text(400, y + 15, i == 2 ? "网页输入" : "编辑", s_th_fg, s_th_bg);
    }

    /* 主题 (点击切换) */
    int ty = 154;
    lcd_draw_rect(10, ty, 470, ty + 22, s_th_border);
    lcd_draw_text(20, ty + 3, "主题", s_th_fg, s_th_bg);
    char tbuf[40];
    snprintf(tbuf, sizeof(tbuf), "%s (点击切换)", tnames[s_theme]);
    lcd_draw_text(130, ty + 3, tbuf, s_th_bar, s_th_bg);

    /* 亮度 (左减右加) */
    int by = 178;
    lcd_draw_rect(10, by, 470, by + 22, s_th_border);
    lcd_draw_text(20, by + 3, "亮度", s_th_fg, s_th_bg);
    char bbuf[32];
    snprintf(bbuf, sizeof(bbuf), "%d%%", s_brightness);
    lcd_draw_text(120, by + 3, bbuf, s_th_fg, s_th_bg);
    lcd_draw_text(280, by + 3, "[左半减]", s_th_border, s_th_bg);
    lcd_draw_text(400, by + 3, "[右半加]", s_th_bar, s_th_bg);

    /* 年级 / 难度 (点击切换: 左半=年级, 右半=难度) */
    int gy = 202;
    lcd_draw_rect(10, gy, 470, gy + 22, s_th_border);
    lcd_draw_text(20, gy + 3, "年级", s_th_fg, s_th_bg);
    char gbuf[40];
    snprintf(gbuf, sizeof(gbuf), "%s", s_grade_names[s_grade]);
    lcd_draw_text(110, gy + 3, gbuf, s_th_bar, s_th_bg);
    lcd_draw_text(250, gy + 3, "难度", s_th_fg, s_th_bg);
    char dbuf[40];
    snprintf(dbuf, sizeof(dbuf), "%s", s_diff_names[s_diff]);
    lcd_draw_text(310, gy + 3, dbuf, s_th_bar, s_th_bg);
    lcd_draw_text(390, gy + 3, "点击切换", s_th_border, s_th_bg);

    /* 存储空间: 知识库剩余 + 收藏占用 (只读显示) */
    int sy = 226;
    lcd_draw_rect(10, sy, 470, sy + 18, s_th_border);
    char sbuf[80];
    int kb_remain = 0;
    for (int i = 0; i < 9; i++)
        kb_remain += weak_kb_remain_bytes(i);
    snprintf(sbuf, sizeof(sbuf), "存储: 知识库余 %dKB · 收藏 %d/256", 
             kb_remain / 1024, fav_count());
    lcd_draw_text(20, sy + 2, sbuf, s_th_border, s_th_bg);

    /* 底部按钮: 扫描 WiFi / 手动输入 */
    lcd_draw_rect(10, 248, 235, 268, s_th_bar);
    text_center2(122, 252, "扫描 WiFi", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(245, 248, 470, 268, s_th_border);
    text_center2(357, 252, "手动输入", s_th_fg, s_th_bg);
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
        text_center(254, "仅显示前 7 个, 点选输入密码", s_th_border, s_th_bg);
    else
        text_center(254, "点选 WiFi 输入密码", s_th_border, s_th_bg);
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
    gfx_hold_begin();                      /* 整页文本共享一次 CS 拉低 */
    while (*s) {
        uint8_t c = *s;
        int w = (c < 0x80) ? r_char_adv16((char)c) : 17;   /* ASCII 用灰度实际宽度 */

        /* 逐字换行 (中英文统一) */
        if (cx + w > x + max_w && cx > x) {
            cx = x;
            if (cur_line > skip_lines) cy += line_h;  /* 跳过区不推进 (分页从顶部重画) */
            cur_line++;
            if (cur_line > lines) lines = cur_line;
        }
        if (c == '\n') {
            cx = x;
            if (cur_line > skip_lines) cy += line_h;
            cur_line++;
            if (cur_line > lines) lines = cur_line;
            s++;
            continue;
        }

        /* 当前行在显示窗口 [skip+1, skip+max_lines] 内才画 */
        int in_view = (cur_line > skip_lines && cur_line <= skip_lines + max_lines);
        if (!count_only && in_view) {
            if (c < 0x80) {
                r_draw_char_gray16(cx, cy, (char)c, fg, bg);   /* Arial 灰度抗锯齿 */
                cx += w;
                s++;
            } else if ((c & 0xE0) == 0xC0) {
                uint16_t code = ((c & 0x1F) << 6) | (s[1] & 0x3F);
                r_draw_char_code(cx, cy, code, fg, bg);   /* 符号优先, 回退中文 */
                cx += 17;
                s += 2;
            } else if ((c & 0xF0) == 0xE0) {
                uint16_t code = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
                r_draw_char_code(cx, cy, code, fg, bg);   /* 符号优先, 回退中文 */
                cx += 17;
                s += 3;
            } else {
                s++;
            }
        } else {
            /* 不画, 只推进宽度以保持换行计算 */
            if (c < 0x80) {
                cx += w;
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
    gfx_hold_end();
    if (p_total) *p_total = lines;
    return lines;
}

static void text_center2(int cx, int y, const char *s, uint16_t fg, uint16_t bg);

/* ---------- 菜单页 ---------- */
static void draw_menu(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 30, s_th_bar);
    lcd_draw_text(8, 6, "高中刷题机", s_th_bar_fg, s_th_bar);  /* 左对齐, 给顶栏时间留位 */
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

    /* 底部按钮: AI 出题 / 知识库 / 收藏 / 薄弱点 / 设置 (5 等分) */
    int bw = 90, bx = 10;
    lcd_draw_rect(bx, 240, bx + bw, 262, s_th_bar);
    text_center2(bx + bw / 2, 244, "AI 出题", s_th_bar_fg, s_th_bar);
    bx += bw + 2;
    lcd_draw_rect(bx, 240, bx + bw, 262, s_th_border);
    text_center2(bx + bw / 2, 244, "知识库", s_th_fg, s_th_bg);
    bx += bw + 2;
    lcd_draw_rect(bx, 240, bx + bw, 262, s_th_border);
    char favb[24];
    snprintf(favb, sizeof(favb), "收藏 %d", fav_count());
    text_center2(bx + bw / 2, 244, favb, s_th_fg, s_th_bg);
    bx += bw + 2;
    lcd_draw_rect(bx, 240, bx + bw, 262, s_th_border);
    text_center2(bx + bw / 2, 244, "薄弱点", s_th_fg, s_th_bg);
    bx += bw + 2;
    lcd_draw_rect(bx, 240, bx + bw, 262, s_th_border);
    text_center2(bx + bw / 2, 244, "设置", s_th_fg, s_th_bg);
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

static void draw_weak_page_body(void);   /* 前置声明 (定义在下方) */

/* 错题显示序 → 存储索引: 列表最新在前 (i=0 显示最新一道) */
static int weak_show_idx(int i)
{
    int n = weak_count(s_weak_subj);
    return n - 1 - i;
}

static void draw_weak_page(void)         /* 该科薄弱点页 (首次进入: 全屏) */
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char t[48];
    snprintf(t, sizeof(t), "薄弱点: %s", s_subjects[s_weak_subj]);
    lcd_draw_text(8, 5, t, s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    draw_weak_page_body();
}

/* 翻页/局部刷新: 只清内容区 (27..271), 顶栏保留 */
static void draw_weak_page_body(void)
{
    lcd_fill_rect(0, 27, 479, 271, s_th_bg);

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

    /* 错题列表 (每页 4 条; 多页时左右半屏翻页; 最新在前) */
    int ly = 46;
    for (int r = s_weak_list_page * per; r < n && r < s_weak_list_page * per + per; r++) {
        char tt[130];
        if (weak_get_wrong(s_weak_subj, weak_show_idx(r), tt, sizeof(tt)) == 0) {
            char line[160];
            snprintf(line, sizeof(line), "%d. %s", r + 1, tt);   /* 行号=显示序 (1=最新) */
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

/* 执行 AI 分析 (阻塞): topics 为已拼接的错题文本 */
static void weak_ai_do(const char *topics)
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
    const char *ai = ai_analyze_weakness(s_subjects[s_weak_subj],
                     topics[0] ? topics : "暂无具体错题内容，请给出该科学习建议");
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

static void weak_ai_run(void)            /* 旧入口: 全部错题 (最多 5 题) */
{
    char topics[1000] = "";
    int n = weak_count(s_weak_subj);
    for (int i = 0; i < n && i < 5; i++) {           /* 最新在前 */
        char t[130];
        if (weak_get_wrong(s_weak_subj, weak_show_idx(i), t, sizeof(t)) == 0) {
            char one[160];
            snprintf(one, sizeof(one), "%d.%s；", i + 1, t);
            if (strlen(topics) + strlen(one) < sizeof(topics))
                strcat(topics, one);
        }
    }
    weak_ai_do(topics);
}

/* ---------- 薄弱点: 逐题选择分析 ---------- */
static uint8_t s_sel_wrong[20];
static int s_sel_page = 0;
static int s_sel_warn = 0;   /* "请先勾选"提示页: 点击任意处返回 */

/* ---------- 收藏: AI 选题分析 (勾选 ≤5 题, 结果全屏翻页) ---------- */
static uint8_t s_sel_fav[64];
static int s_sel_fav_page = 0;
/* 分析结果指针: 指向 ai_analyze_weakness 的静态缓冲 (结果页停留期间
 * 无 AI 调用不会覆盖; 不复制, 省 8KB BSS — ESP32 静态内存紧张) */
static const char *s_fav_ai_ptr = "";

static void redraw_sel_row(int i);
static void redraw_sel_count(void);
static void draw_weak_select_body(void);

static void draw_weak_select(void)      /* 选题页: 复选框 + 翻页 + 底部按钮 (首次进入: 全屏) */
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char t[48];
    snprintf(t, sizeof(t), "选题分析: %s", s_subjects[s_weak_subj]);
    lcd_draw_text(8, 5, t, s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    draw_weak_select_body();
}

/* 局部刷新: 只清内容区 (顶栏保留) */
static void draw_weak_select_body(void)
{
    lcd_fill_rect(0, 27, 479, 271, s_th_bg);

    int n = weak_count(s_weak_subj);
    int per = 4;
    int total_pages = (n + per - 1) / per;
    if (total_pages < 1) total_pages = 1;
    if (s_sel_page >= total_pages) s_sel_page = total_pages - 1;
    redraw_sel_count();
    int ly = 48;
    for (int i = s_sel_page * per; i < n && i < s_sel_page * per + per; i++) {
        redraw_sel_row(i);
        ly += 32;
    }
    if (n == 0)
        lcd_draw_text(8, ly, "(暂无错题)", s_th_border, s_th_bg);
    else if (total_pages > 1)
        lcd_draw_text(8, ly, "左翻上页 右翻下页", s_th_border, s_th_bg);

    /* 底部: [全选/清空] [分析选中] [返回] */
    lcd_draw_rect(10, 240, 156, 262, s_th_border);
    text_center2(83, 244, "全选/清空", s_th_fg, s_th_bg);
    lcd_draw_rect(162, 240, 308, 262, s_th_bar);
    text_center2(235, 244, "分析选中", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(314, 240, 470, 262, s_th_border);
    text_center2(392, 244, "返回", s_th_fg, s_th_bg);
}

/* 局部重画单个勾选行 (勾选切换时用, 避免整屏闪烁) */
static void redraw_sel_row(int i)
{
    int row = i - s_sel_page * 4;
    if (row < 0 || row >= 4)
        return;
    int ly = 48 + row * 32;
    int cbx = 16, cby = ly + 1;
    lcd_fill_rect(cbx, cby, cbx + 22, cby + 22, s_th_bg);      /* 先清框内 */
    lcd_draw_rect(cbx, cby, cbx + 22, cby + 22,
                  s_sel_wrong[i] ? s_th_bar : s_th_border);
    if (s_sel_wrong[i])
        text_center2(cbx + 11, cby + 2, "√", s_th_bar_fg, s_th_bg);
    char line[160];
    char tt[130];
    if (weak_get_wrong(s_weak_subj, weak_show_idx(i), tt, sizeof(tt)) == 0)
        snprintf(line, sizeof(line), "%d. %s", i + 1, tt);
    else
        snprintf(line, sizeof(line), "%d.", i + 1);
    int bl = strlen(line), px = 0, cut = 0;
    for (int j = 0; j < bl && px < 400; ) {
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
    lcd_fill_rect(50, ly, 470, ly + 16, s_th_bg);              /* 清行文字区 */
    lcd_draw_text(50, ly, line, s_th_fg, s_th_bg);
}

/* 局部刷新顶部"已选 x/y"计数行 */
static void redraw_sel_count(void)
{
    int n = weak_count(s_weak_subj);
    int sel = 0;
    for (int i = 0; i < n && i < 20; i++)
        if (s_sel_wrong[i]) sel++;
    char cnt[48];
    snprintf(cnt, sizeof(cnt), "已选 %d/%d 题  最多5题", sel, n);
    lcd_fill_rect(0, 27, 479, 45, s_th_bg);
    lcd_draw_text(8, 30, cnt, s_th_fg, s_th_bg);
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
        /* 单行: [科目] 内容 (截断, 左侧留移除按钮) */
        char line[96];
        snprintf(line, sizeof(line), "[%s] %s", g_fav_q.subject, g_fav_q.content);
        int bl = strlen(line), px = 0, cut = 0;
        for (int j = 0; j < bl && px < 340; ) {
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
        /* 右侧 [移除] 按钮 */
        lcd_draw_rect(384, y + 4, 470, y + 30, s_th_border);
        text_center2(427, y + 8, "移除", s_th_fg, s_th_bg);
    }
    if (n == 0)
        text_center(130, "还没有收藏题目", s_th_border, s_th_bg);

    /* 底部: [AI 分析] (常驻) + 翻页按钮 (多页时) */
    lcd_draw_rect(10, 230, 130, 264, s_th_bar);
    text_center2(70, 234, "AI 分析", s_th_bar_fg, s_th_bar);
    if (total_pages > 1) {
        lcd_draw_rect(136, 230, 232, 264, s_th_border);
        text_center2(184, 234, "< 上页", s_fav_page > 0 ? s_th_fg : s_th_border, s_th_bg);
        lcd_draw_rect(238, 230, 334, 264, s_th_border);
        char pg[24];
        snprintf(pg, sizeof(pg), "%d/%d", s_fav_page + 1, total_pages);
        text_center2(286, 234, pg, s_th_fg, s_th_bg);
        lcd_draw_rect(340, 230, 470, 264, s_th_border);
        text_center2(405, 234, "下页 >", s_fav_page + 1 < total_pages ? s_th_fg : s_th_border, s_th_bg);
    } else {
        text_center(254, "点行查看 · 右侧移除", s_th_border, s_th_bg);
    }
}

static void text_center2(int cx, int y, const char *s, uint16_t fg, uint16_t bg)
{
    lcd_draw_text(cx - text_width(s) / 2, y, s, fg, bg);
}

/* 按 16px 字体像素宽截断 UTF-8 串 (CJK≈17px, ASCII≈8px); 截断时尾部加 "..".
 * src 与 dst 可同一缓冲 (就地截断) */
static void trunc_px(const char *src, char *dst, int dstsz, int maxpx)
{
    int px = 0, j = 0;
    while (src[j] && j < dstsz - 8) {
        uint8_t ch = (uint8_t)src[j];
        int cl = (ch < 0x80) ? 1 : ((ch & 0xE0) == 0xC0) ? 2 :
                 ((ch & 0xF0) == 0xE0) ? 3 : ((ch & 0xF8) == 0xF0) ? 4 : 1;
        int w = (ch < 0x80) ? 8 : 17;
        if (px + w > maxpx)
            break;
        memmove(dst + j, src + j, (size_t)cl);
        j += cl;
        px += w;
    }
    int truncated = (src[j] != 0);
    dst[j] = 0;
    if (truncated) {
        int tl = (int)strlen(dst);
        if (tl + 3 < dstsz)
            strcat(dst, "..");
    }
}

/* ---------- 收藏: AI 选题分析 ---------- */
static void draw_page_chrome(const char *title, int page, int total_pages); /* 定义在下方 */
static void draw_fav_select_body(void);
static void redraw_fav_row(int i);
static void redraw_fav_count(void);
static void draw_fav_ai_result(void);

static void draw_fav_select(void)      /* 选题页 (首次进入: 全屏) */
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "收藏分析: 勾选题", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    draw_fav_select_body();
}

static void draw_fav_select_body(void)
{
    lcd_fill_rect(0, 27, 479, 271, s_th_bg);

    int n = fav_count();
    int per = 4;
    int total_pages = (n + per - 1) / per;
    if (total_pages < 1) total_pages = 1;
    if (s_sel_fav_page >= total_pages) s_sel_fav_page = total_pages - 1;
    redraw_fav_count();
    int ly = 48;
    for (int i = s_sel_fav_page * per; i < n && i < s_sel_fav_page * per + per; i++) {
        redraw_fav_row(i);
        ly += 32;
    }
    if (n == 0)
        lcd_draw_text(8, ly, "(暂无收藏)", s_th_border, s_th_bg);
    else if (total_pages > 1)
        lcd_draw_text(8, ly, "左翻上页 右翻下页", s_th_border, s_th_bg);

    /* 底部: [全选/清空] [分析选中] [返回] */
    lcd_draw_rect(10, 240, 156, 262, s_th_border);
    text_center2(83, 244, "全选/清空", s_th_fg, s_th_bg);
    lcd_draw_rect(162, 240, 308, 262, s_th_bar);
    text_center2(235, 244, "分析选中", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(314, 240, 470, 262, s_th_border);
    text_center2(392, 244, "返回", s_th_fg, s_th_bg);
}

/* 局部重画单个勾选行 (收藏题) */
static void redraw_fav_row(int i)
{
    int row = i - s_sel_fav_page * 4;
    if (row < 0 || row >= 4)
        return;
    int ly = 48 + row * 32;
    int cbx = 16, cby = ly + 1;
    lcd_fill_rect(cbx, cby, cbx + 22, cby + 22, s_th_bg);
    lcd_draw_rect(cbx, cby, cbx + 22, cby + 22,
                  s_sel_fav[i] ? s_th_bar : s_th_border);
    if (s_sel_fav[i])
        text_center2(cbx + 11, cby + 2, "√", s_th_bar_fg, s_th_bg);
    char line[160];
    if (fav_get(i) == 0)
        snprintf(line, sizeof(line), "%d.[%s] %s", i + 1,
                 g_fav_q.subject ? g_fav_q.subject : "?", g_fav_q.content);
    else
        snprintf(line, sizeof(line), "%d.", i + 1);
    int bl = strlen(line), px = 0, cut = 0;
    for (int j = 0; j < bl && px < 370; ) {
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
    lcd_fill_rect(50, ly, 470, ly + 16, s_th_bg);
    lcd_draw_text(50, ly, line, s_th_fg, s_th_bg);
}

static void redraw_fav_count(void)
{
    int n = fav_count();
    int sel = 0;
    for (int i = 0; i < n && i < 64; i++)
        if (s_sel_fav[i]) sel++;
    char cnt[48];
    snprintf(cnt, sizeof(cnt), "已选 %d/%d 题  最多5题", sel, n);
    lcd_fill_rect(0, 27, 479, 45, s_th_bg);
    lcd_draw_text(8, 30, cnt, s_th_fg, s_th_bg);
}

/* 执行收藏 AI 分析 (阻塞): 结果存 s_fav_ai_buf 供结果页翻页 */
static void fav_ai_do(const char *subj, const char *topics)
{
    ESP_LOGI(TAG, "fav ai run: wifi=%d subj=%s", s_wifi_state, subj);
    if (s_wifi_state != 2) {
        lcd_clear(s_th_bg);
        text_center(100, "请先连接 WiFi", RED, s_th_bg);
        text_center(130, "点击返回", s_th_border, s_th_bg);
        s_state = 22;
        return;
    }
    lcd_clear(s_th_bg);
    text_center(110, "AI 分析中... (最长60秒)", s_th_fg, s_th_bg);
    const char *ai = ai_analyze_weakness(subj,
                     topics[0] ? topics : "暂无具体题目内容，请给出学习建议");
    if (ai[0]) {
        s_fav_ai_ptr = ai;           /* 指向静态结果缓冲, 结果页期间不会被覆盖 */
        s_wpage = 0;
        s_state = 23;
        draw_fav_ai_result();
    } else {
        lcd_clear(s_th_bg);
        text_center(100, "AI 分析失败", RED, s_th_bg);
        text_center(130, "检查网络 / API Key", s_th_border, s_th_bg);
        text_center(150, "点击返回", s_th_border, s_th_bg);
        s_state = 22;
    }
}

/* 收藏 AI 分析结果页 (可翻页, 底部 [返回收藏]) */
static void draw_fav_ai_result(void)
{
    text_wrap_skip(10, -1, s_fav_ai_ptr, 0, 0, 460, 18, 0, 0, &s_wtotal);
    int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
    if (total_pages < 1) total_pages = 1;
    if (s_wpage >= total_pages) s_wpage = total_pages - 1;
    draw_page_chrome("收藏 AI 分析", s_wpage, total_pages);
    text_wrap_skip(10, 36, s_fav_ai_ptr, s_th_fg, s_th_bg, 460, 18,
                   FULL_LINES, s_wpage * FULL_LINES, NULL);
    lcd_draw_rect(10, 250, 130, 268, s_th_border);
    text_center2(70, 254, "返回收藏", s_th_fg, s_th_bg);
}

/* ---------- 答题页 ---------- */
#define OPT_Y0   118           /* 选项区起始 */
#define OPT_H    40
#define OPT_GAP  8

/* 分页控件: [label][−][n/N][+] 高 16 (位于 y..y+15) */
static void draw_pager(int x, int y, const char *label, int page, int pages)
{
    if (pages <= 1)
        return;
    lcd_draw_text(x, y + 1, label, s_th_border, s_th_bg);
    int b = x + 46;
    lcd_draw_rect(b, y, b + 30, y + 15, s_th_border);
    text_center2(b + 15, y + 1, "-", s_th_fg, s_th_bg);
    char pg[24];
    snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages);
    draw_small_str(b + 66 - (int)strlen(pg) * 6, y + 4, pg, s_th_fg, s_th_bg);
    b += 66;
    lcd_draw_rect(b, y, b + 30, y + 15, s_th_border);
    text_center2(b + 15, y + 1, "+", s_th_fg, s_th_bg);
}

/* 题干区 (y=28..117): 4 行/页自适应分页 + 控件行 (题目/选项) */
static void draw_question_area(const quiz_q_t *q)
{
    lcd_fill_rect(0, 28, 479, 117, s_th_bg);   /* 局部刷新时先清 */
    text_wrap_skip(10, -1, q->content, 0, 0, 460, 18, 0, 0, &s_qtotal);
    int qpages = (s_qtotal + 3) / 4;
    if (qpages > 1) {
        if (s_qpage >= qpages) s_qpage = qpages - 1;
        text_wrap_skip(10, 30, q->content, s_th_fg, s_th_bg, 460, 18, 4,
                       s_qpage * 4, NULL);
    } else {
        s_qpage = 0;
        text_wrap_skip(10, 30, q->content, s_th_fg, s_th_bg, 460, 18, 4, 0, NULL);
    }
    draw_pager(10, 102, "题目", s_qpage, qpages);
    if (q->is_choice == 1)
        draw_pager(300, 102, "选项", s_opt_page, s_opt_pages);
}

/* 选项总页数 (每页 2 行/选项) */
static int opt_pages_of(const quiz_q_t *q)
{
    int omax = 1;
    for (int i = 0; i < 4; i++) {
        int l = text_wrap_skip(10, -1, q->options[i], 0, 0, 186, 16, 0, 0, NULL);
        if (l > omax) omax = l;
    }
    return (omax + 1) / 2;
}

static void draw_option(int i);   /* 定义在下方 */

/* 选项区 (y=118..206): 按钮 + 当前页文字 (每页 2 行) */
static void draw_option_area(const quiz_q_t *q)
{
    lcd_fill_rect(0, 118, 479, 207, s_th_bg);  /* 局部刷新时先清 */
    for (int i = 0; i < 4; i++)
        draw_option(i);
    for (int i = 0; i < 4; i++) {
        int x = (i % 2) ? 284 : 50, y = OPT_Y0 + 10 + (i / 2) * (OPT_H + OPT_GAP);
        int sel = (i == s_opt_sel && !s_answered);
        uint16_t fg = sel ? s_th_bar_fg : s_th_fg;
        text_wrap_skip(x, y, q->options[i], fg, sel ? s_th_sel : s_th_bg,
                       186, 16, 2, s_opt_page * 2, NULL);
    }
}

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
    text_wrap_skip(x, y, q->options[i], sel ? s_th_bar_fg : s_th_fg,
                   sel ? s_th_sel : s_th_bg, 186, 16, 2, s_opt_page * 2, NULL);
}

static void draw_quiz(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    int qsubj = subj_of(q->subject);
    lcd_clear(s_th_bg);

    /* 顶栏: AI 题带考点名 (截断, 给收藏区留位) */
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char top[64];
    if (q->topic && q->topic[0] && s_qlist[s_qidx] == question_count) {
        char tb[40];
        trunc_px(q->topic, tb, sizeof(tb), 70);
        snprintf(top, sizeof(top), "%s·%s  %d/%d", q->subject, tb,
                 s_qidx + 1, s_qcount);
    } else {
        snprintf(top, sizeof(top), "%s  %d/%d", q->subject, s_qidx + 1, s_qcount);
    }
    lcd_draw_text(8, 5, top, s_th_bar_fg, s_th_bar);
    /* 收藏状态 (选择/填空/解答题均可收藏; 收藏题查看页也显示, 点★可取消收藏) */
    lcd_draw_text(190, 5, fav_contains(q) ? "★已藏" : "☆收藏",
                  fav_contains(q) ? RED : s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    /* 题干 (自适应分页) + 控件行 */
    if (q->is_choice != 1) {
        s_opt_pages = 1;
        s_opt_page = 0;
    } else {
        s_opt_pages = opt_pages_of(q);
        if (s_opt_page >= s_opt_pages)
            s_opt_page = s_opt_pages - 1;
    }
    draw_question_area(q);

    if (q->is_choice == 1) {
        draw_option_area(q);
        if (s_answered) {
            const char *res = (s_opt_sel == q->answer_idx) ? "回答正确" : "回答错误";
            lcd_draw_text(10, 212, res, (s_opt_sel == q->answer_idx) ? GREEN : RED, s_th_bg);
            /* 收藏按钮 (收藏题查看页也可取消) */
            lcd_draw_rect(340, 210, 470, 230, s_th_border);
            lcd_draw_text(346, 213, fav_contains(q) ? "★已藏" : "☆收藏本题",
                          fav_contains(q) ? RED : s_th_fg, s_th_bg);
            if (q->explanation && q->explanation[0]) {
                int el = 0;
                text_wrap_skip(10, 232, q->explanation, s_th_fg, s_th_bg,
                               460, 16, 2, 0, &el);
                if (el > 2)
                    lcd_draw_text(110, 212, "点解析翻页", s_th_border, s_th_bg);
            }
        } else {
            int qpages = (s_qtotal + 3) / 4;
            if (qpages > 1)
                text_center(230, "点题目翻页  短按选答案  长按提交", s_th_border, s_th_bg);
            else
                text_center(230, "短按选答案  长按提交", s_th_border, s_th_bg);
        }
    } else if (q->is_choice == 2) {        /* 键盘填空 (英文/数字答案) */
        if (s_answered) {
            lcd_draw_text(10, 212, s_fill_ok ? "回答正确" : "回答错误",
                          s_fill_ok ? GREEN : RED, s_th_bg);
            /* 答错且该考点处于薄弱强化区: 红色标记 */
            if (!s_fill_ok && qsubj >= 0 && s_qlist[s_qidx] == question_count &&
                q->topic && quiz_topic_weak(qsubj, q->topic))
                lcd_draw_text(96, 212, "薄弱考点·重点看解析", RED, s_th_bg);
            lcd_draw_text(10, 228, "我的答案:  ", s_th_fg, s_th_bg);
            lcd_draw_text(108, 228, s_fill_buf[0] ? s_fill_buf : "(空)",
                          s_fill_ok ? s_th_fg : RED, s_th_bg);
            lcd_draw_text(10, 244, "正确答案:  ", s_th_fg, s_th_bg);
            lcd_draw_text(108, 244, q->answer_text ? q->answer_text : "",
                          s_fill_ok ? s_th_border : GREEN, s_th_bg);
            text_center(262, "点下方看解析 · 短按下一题", s_th_border, s_th_bg);
        } else {
            lcd_draw_rect(10, 118, 470, 206, s_th_border);
            if (s_fill_buf[0]) {
                char fb[80];
                snprintf(fb, sizeof(fb), "已输入: %s", s_fill_buf);
                text_center2(240, 150, fb, s_th_fg, s_th_bg);
                text_center2(240, 172, "点输入框重新输入", s_th_border, s_th_bg);
            } else {
                text_center2(240, 150, "点此输入答案", s_th_bar, s_th_bg);
                text_center2(240, 172, "(仅英文/数字, 软键盘)", s_th_border, s_th_bg);
            }
            text_center(230, "点输入框弹键盘 · 长按跳过", s_th_border, s_th_bg);
        }
    } else {
        if (s_show_ans) {
            lcd_draw_text(10, 160, "参考答案:", s_th_fg, s_th_bg);
            text_wrap(10, 178, q->answer_text, s_th_bar, s_th_bg, 460, 16, 2);
            int el = 0;
            if (q->explanation && q->explanation[0]) {
                text_wrap_skip(10, 214, q->explanation, s_th_fg, s_th_bg,
                               460, 16, 2, 0, &el);
            }
            lcd_draw_text(10, 246, el > 2 ? "点击解析翻页  短按下一题" : "短按下一题",
                          s_th_border, s_th_bg);
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
        text_center(254, "点击返回", s_th_border, s_th_bg);
    else if (page == 0)
        text_center(254, "左返回 右翻页", s_th_border, s_th_bg);
    else if (page + 1 >= total_pages)
        text_center(254, "左翻上页 右返回", s_th_border, s_th_bg);
    else
        text_center(254, "左翻上页 右翻下页", s_th_border, s_th_bg);
}

static void draw_explain(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    /* 解答题/填空: 参考答案+解析 合并分页; 选择题: 仅解析 */
    static char expbuf[2048];
    const char *src = q->explanation ? q->explanation : "";
    if (q->is_choice != 1) {
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

/* ---------- 知识库 (科目 → 主题池 6 槽; 自定义拼音主题经 AI 转中文) ---------- */
static const char *s_kb_topics[9][4] = {
    { "函数与导数", "三角函数与数列", "立体几何与解析几何", "概率与统计" },
    { "力学", "电磁学", "热学·光学·原子物理", "物理实验专题" },
    { "基本概念与化学计算", "元素化合物", "有机化学", "化学反应原理" },
    { "分子与细胞(必修一)", "遗传与进化(必修二)", "稳态与环境(选必一)", "生物技术与工程(基因工程/PCR等)" },
    { "时态与语态", "从句与虚拟语气", "词汇与短语", "写作与阅读技巧" },
    { "文言文阅读", "古诗词鉴赏", "现代文阅读", "作文写作" },
    { "中国古代史", "中国近现代史", "世界史", "历史论述题方法" },
    { "经济与社会", "政治与法治", "哲学与文化", "国际政治与经济" },
    { "自然地理", "人文地理", "区域地理", "地图与图表判读" },
};
static int s_kb_subj = 0;
static int s_kb_slot = 0;        /* 当前操作的槽 0..2 */
static int s_kb_custom_mode = 0; /* 1=自定义主题 (拼音转中文后生成) */
static int s_kb_list_page = 0;   /* 主题列表页码 (每页 5 行) */
/* 知识库全文直接引用 weak_get_kb 缓冲, 不再单独缓存 (省 24KB 内存) */

/* 槽 i 显示名: 已生成=槽内名称; 空=预置名(i<4) 或 "(空槽)" */
static const char *kb_slot_name(int i)
{
    const char *n = weak_get_kb_name(s_kb_subj, i);
    if (n[0])
        return n;
    if (i < 4)
        return s_kb_topics[s_kb_subj][i];
    return "(空槽)";
}

/* 加载当前槽内容; 返回 "" 表示未生成 (直接引用 weak_get_kb 静态缓冲) */
static const char *kb_load(void)
{
    return weak_get_kb(s_kb_subj, s_kb_slot);
}

/* 找空槽; 无空槽返回 -1 */
static int kb_find_empty(void)
{
    for (int i = 0; i < WEAK_KB_CNT; i++)
        if (!weak_get_kb_name(s_kb_subj, i)[0])
            return i;
    return -1;
}

static void draw_kb_subject(void)      /* 选科 */
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    lcd_draw_text(8, 5, "知识库: 选择科目", s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);
    for (int i = 0; i < 9; i++) {
        int x = 10 + (i % 3) * 158, y = 40 + (i / 3) * 70;
        lcd_fill_rect(x, y, x + 148, y + 60, s_th_bg);
        lcd_draw_rect(x, y, x + 148, y + 60, s_th_border);
        text_center2(x + 74, y + 22, s_subjects[i], s_th_fg, s_th_bg);
    }
}

static void draw_kb_full(void);        /* 前置声明 */

/* 主题列表页: 6 槽 (名称+状态+删除), 每页 5 行可翻页 */
static void draw_kb_topics(void)
{
    lcd_clear(s_th_bg);
    lcd_fill_rect(0, 0, 479, 26, s_th_bar);
    char t[48];
    snprintf(t, sizeof(t), "知识库: %s (选主题)", s_subjects[s_kb_subj]);
    lcd_draw_text(8, 5, t, s_th_bar_fg, s_th_bar);
    draw_ip_bar(1);

    int per = 5;
    int total_pages = (WEAK_KB_CNT + per - 1) / per;
    if (s_kb_list_page >= total_pages) s_kb_list_page = total_pages - 1;

    for (int r = 0; r < per; r++) {
        int slot = s_kb_list_page * per + r;
        if (slot >= WEAK_KB_CNT)
            break;
        int y = 30 + r * 36;
        const char *n = weak_get_kb_name(s_kb_subj, slot);
        lcd_draw_rect(10, y, 470, y + 32, s_th_border);
        char line[80];
        snprintf(line, sizeof(line), "%d. %s", slot + 1, kb_slot_name(slot));
        int bl = strlen(line), px = 0, cut = 0;
        for (int j = 0; j < bl && px < 300; ) {
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
        lcd_draw_text(16, y + 8, line, s_th_fg, s_th_bg);
        if (n[0]) {                         /* 已生成: 右侧删除按钮 */
            lcd_draw_rect(400, y + 4, 466, y + 28, s_th_border);
            text_center2(433, y + 8, "删除", s_th_fg, s_th_bg);
        } else {
            lcd_draw_text(330, y + 8, "未生成", s_th_border, s_th_bg);
        }
    }

    if (total_pages > 1) {                  /* 翻页行 */
        lcd_draw_rect(10, 212, 156, 236, s_th_border);
        text_center2(83, 216, "< 上页", s_kb_list_page > 0 ? s_th_fg : s_th_border, s_th_bg);
        lcd_draw_rect(162, 212, 308, 236, s_th_border);
        char pg[16];
        snprintf(pg, sizeof(pg), "%d/%d", s_kb_list_page + 1, total_pages);
        text_center2(235, 216, pg, s_th_fg, s_th_bg);
        lcd_draw_rect(314, 212, 470, 236, s_th_border);
        text_center2(392, 216, "下页 >", s_kb_list_page + 1 < total_pages ? s_th_fg : s_th_border, s_th_bg);
    } else {
        char cap[80];
        int used = weak_kb_used_slots(s_kb_subj);
        int remain = weak_kb_remain_bytes(s_kb_subj);
        snprintf(cap, sizeof(cap),
                 "容量: 已用 %d/%d 槽 · 剩余 %dKB / %dKB",
                 used, WEAK_KB_CNT, remain / 1024,
                 weak_kb_capacity_bytes() / 1024);
        text_center(214, cap, s_th_bar, s_th_bg);
        text_center(230, "点行查看 · 未生成点行生成 · 右侧删除", s_th_border, s_th_bg);
    }

    /* 底部: [自定义主题] [返回] */
    lcd_draw_rect(10, 240, 250, 264, s_th_bar);
    text_center2(130, 244, "自定义主题", s_th_bar_fg, s_th_bar);
    lcd_draw_rect(256, 240, 470, 264, s_th_border);
    text_center2(363, 244, "返回", s_th_fg, s_th_bg);
}

/* 简易 Markdown 清洗 (知识库 AI 输出用):
 * - 行首 # 标题符 / > 引用符 删除 (内容保留)
 * - 行首 - * + 列表符 → · (分隔线, 如三个连字符或三个星号, 整行丢弃)
 * - 行内 * ` _ 强调/代码标记删除
 * - 连续空行压缩为一个 */
static void md_clean(const char *src, char *dst, int dstsz)
{
    int j = 0;
    int line_start = 1;
    int prev_nl = 1;
    for (const char *p = src; *p && j < dstsz - 1; p++) {
        char c = *p;
        if (c == '\n') {
            if (!prev_nl) {
                dst[j++] = '\n';
                prev_nl = 1;
            }
            line_start = 1;
            continue;
        }
        if (line_start) {
            line_start = 0;
            if (c == '#') {                        /* 标题行: 去井号+空格 */
                while (*p == '#' || *p == ' ' || *p == '\t') p++;
                c = *p;
                if (c == '\n' || c == 0) {
                    if (!prev_nl) { dst[j++] = '\n'; prev_nl = 1; }
                    line_start = 1;
                    continue;
                }
            } else if (c == '>') {                 /* 引用: 去符号 */
                if (p[1] == ' ') p++;
                continue;
            } else if (c == '-' || c == '*' || c == '_') {
                /* 分隔线: --- *** ___ 整行丢弃 */
                if ((p[1] == c && p[2] == c) && (p[3] == '\n' || p[3] == 0)) {
                    while (*p && *p != '\n') p++;
                    p--;
                    continue;
                }
                if (p[1] == ' ') {                 /* 列表项 → · (2字节, 保证原地安全) */
                    p++;
                    if (j < dstsz - 2) {
                        dst[j++] = 0xC2;
                        dst[j++] = 0xB7;
                    }
                    prev_nl = 0;
                    continue;
                }
                if (c == '*' || c == '_')
                    continue;                      /* 行首裸符号丢弃 */
            }
        }
        if (c == '*' || c == '`' || c == '_' || c == '#')
            continue;                              /* 行内强调/代码标记 */
        dst[j++] = c;
        prev_nl = 0;
    }
    while (j > 0 && (dst[j - 1] == '\n' || dst[j - 1] == ' '))
        j--;
    dst[j] = 0;

    /* 兜底分段: AI 偶发把 "1.xxx 2.yyy" 挤成一段不换行.
     * 扫描 "数字." 后跟 空格/汉字/行尾 的编号, 前面非行首/换行/数字时补 \n
     * (前一个字符是空格则直接替换), 让每个编号点单独成行. */
    {
        int k = 0;
        while (dst[k] && k < dstsz - 3) {
            if (dst[k] >= '0' && dst[k] <= '9') {
                int kk = k;
                while (dst[kk] >= '0' && dst[kk] <= '9')
                    kk++;
                if (dst[kk] == '.' &&
                    (dst[kk + 1] == ' ' || dst[kk + 1] == 0 ||
                     (uint8_t)dst[kk + 1] >= 0x80) &&   /* 空格/行尾/汉字 */
                    k > 0 && dst[k - 1] != '\n' &&
                    !(dst[k - 1] >= '0' && dst[k - 1] <= '9') &&
                    dst[k - 1] != '.') {
                    if (dst[k - 1] == ' ')
                        dst[k - 1] = '\n';          /* 空格原位变换行 */
                    else if (strlen(dst) < (size_t)dstsz - 1) {   /* 防越界 */
                        memmove(dst + k + 1, dst + k, strlen(dst + k) + 1);
                        dst[k] = '\n';
                    }
                    k = kk + 1;
                    continue;
                }
            }
            k++;
        }
    }
}

static void kb_run(void)               /* 生成知识库 (阻塞) */
{
    ESP_LOGI(TAG, "kb run: wifi=%d subj=%d slot=%d custom=%d",
             s_wifi_state, s_kb_subj, s_kb_slot, s_kb_custom_mode);
    if (s_wifi_state != 2) {
        lcd_clear(s_th_bg);
        text_center(100, "请先连接 WiFi", RED, s_th_bg);
        text_center(130, "点击返回", s_th_border, s_th_bg);
        s_state = 18;
        return;
    }
    /* 空间检查: 生成后该科知识库区必须剩余 ≥ 1 槽 (整槽物理占用) */
    int kb_remain = weak_kb_remain_bytes(s_kb_subj);
    if (kb_remain < WEAK_KB_SLOT) {
        lcd_clear(s_th_bg);
        char mbuf[64];
        snprintf(mbuf, sizeof(mbuf), "知识库空间不足: 剩余 %dKB (需 ≥16KB)",
                 kb_remain / 1024);
        text_center(100, mbuf, RED, s_th_bg);
        text_center(130, "请删除已有主题后重试", s_th_border, s_th_bg);
        s_state = 18;
        return;
    }
    /* 主题名: 自定义输入(拼音)先经 AI 转中文 */
    char name[64];
    if (s_kb_custom_mode) {
        const char *cn = ai_translate_topic(s_subjects[s_kb_subj], s_kb_custom);
        snprintf(name, sizeof(name), "%s", cn[0] ? cn : s_kb_custom);
    } else {
        snprintf(name, sizeof(name), "%s", kb_slot_name(s_kb_slot));
    }
    lcd_clear(s_th_bg);
    char sbuf[96];
    snprintf(sbuf, sizeof(sbuf), "AI 生成中... (%s)", name);
    text_center(110, sbuf, s_th_fg, s_th_bg);
    text_center(140, "最长 5 分钟, 失败自动重试", s_th_border, s_th_bg);
    const char *kb = ai_get_knowledge(s_subjects[s_kb_subj], name);
    if (kb[0]) {
        weak_set_kb(s_kb_subj, s_kb_slot, name, kb);
        s_kb_page = 0;
        s_state = 19;
        draw_kb_full();
    } else {
        lcd_clear(s_th_bg);
        text_center(100, "生成失败", RED, s_th_bg);
        text_center(130, ai_last_error(), s_th_fg, s_th_bg);
        text_center(150, "点击重试 / 长按返回", s_th_border, s_th_bg);
        s_state = 18;
    }
}

static void draw_kb_full(void)         /* 知识库全文页 (可翻页) */
{
    const char *kb = kb_load();          /* 直接引用 weak_get_kb 缓冲 */
    if (kb[0])                           /* 原地清洗 markdown (省 24KB 静态) */
        md_clean(kb, (char *)kb, KB_OUT_MAX);
    text_wrap_skip(10, -1, kb, 0, 0, 460, 18, 0, 0, &s_wtotal);
    int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
    if (total_pages < 1) total_pages = 1;
    if (s_kb_page >= total_pages) s_kb_page = total_pages - 1;
    char t[96];
    snprintf(t, sizeof(t), "知识库: %s·%s", s_subjects[s_kb_subj], kb_slot_name(s_kb_slot));
    draw_page_chrome(t, s_kb_page, total_pages);
    text_wrap_skip(10, 36, kb, s_th_fg, s_th_bg, 460, 18,
                   FULL_LINES, s_kb_page * FULL_LINES, NULL);
    /* 底部左侧: [重新生成] */
    lcd_draw_rect(10, 250, 130, 268, s_th_border);
    text_center2(70, 254, "重新生成", s_th_fg, s_th_bg);
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
        s_qidx = 0; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
        s_state = 1;
        draw_quiz();
    } else {
        lcd_clear(s_th_bg);
        text_center(100, "出题失败", RED, s_th_bg);
        text_center(120, ai_last_error(), s_th_fg, s_th_bg);   /* E 错误码 */
        text_center(142, "点击重试 / 长按返回", s_th_border, s_th_bg);
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
        text_center2(x + 74, y + 22, s_subjects[i], s_th_fg, s_th_bg);   /* 夜间主题下 BLACK 不可见 */
        /* 该科最薄弱板块 (无错题记录则不显示) */
        const char *wt = quiz_weak_topic(i);
        if (wt[0]) {
            char wbuf[48];
            snprintf(wbuf, sizeof(wbuf), "薄弱:%s", wt);
            trunc_px(wbuf, wbuf, sizeof(wbuf), 132);
            lcd_draw_text(x + 8, y + 41, wbuf, RED, s_th_bg);
        }
    }
}

/* 收藏/取消收藏 (返回 1=已取消). fav_get() 会改写 g_fav_q 的静态缓冲,
 * 所以先复制 content 再逐槽比较 (收藏题查看时 q 就是 g_fav_q) */
static int fav_toggle(const quiz_q_t *q)
{
    char qbuf[2048];
    if (!q->content)
        return 0;
    strncpy(qbuf, q->content, sizeof(qbuf) - 1);
    qbuf[sizeof(qbuf) - 1] = 0;
    if (fav_contains(q)) {
        for (int i = 0; i < fav_count(); i++) {
            if (fav_get(i) == 0 && strcmp(g_fav_q.content, qbuf) == 0) {
                fav_remove(i);
                return 1;
            }
        }
        return 0;
    }
    fav_add(q);
    return 0;
}

/* 答题页返回: 收藏题回收藏列表, 其余回菜单 */
static void quiz_back(void)
{
    if (s_qlist[0] == question_count + 1) {
        s_state = 11;
        draw_fav_list();
    } else {
        s_state = 0;
        draw_menu();
    }
}

/* ---------- 触摸输入 (上升沿触发, 坐标已按 chip max 缩放) ---------- */
static void ui_touch(int sx, int sy)
{
    if (s_state == 0) {                     /* 菜单 */
        if (sx >= 10 && sx <= 100 && sy >= 238 && sy <= 264) {  /* AI 出题 */
            s_state = 8;
            draw_ai_subject();
            return;
        }
        if (sx >= 102 && sx <= 192 && sy >= 238 && sy <= 264) { /* 知识库 */
            s_state = 17;
            draw_kb_subject();
            return;
        }
        if (sx >= 194 && sx <= 284 && sy >= 238 && sy <= 264) { /* 收藏 */
            s_state = 11;
            draw_fav_list();
            return;
        }
        if (sx >= 286 && sx <= 376 && sy >= 238 && sy <= 264) { /* 薄弱点 */
            s_state = 12;
            draw_weak_subject();
            return;
        }
        if (sx >= 378 && sx <= 470 && sy >= 238 && sy <= 264) {  /* 设置 */
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
    } else if (s_state == 11) {             /* 收藏夹: 点选查看 / AI 分析 / 底部翻页 */
        int n = fav_count();
        int total_pages = (n + 5 - 1) / 5;
        if (total_pages < 1) total_pages = 1;
        if (sy >= 230) {                     /* 底部: [AI 分析] + 翻页 */
            if (sx < 130) {                  /* AI 分析: 进入选题页 */
                s_sel_fav_page = 0;
                s_sel_warn = 0;
                s_state = 22;
                draw_fav_select();
            } else if (total_pages > 1) {
                if (sx < 232 && s_fav_page > 0) {
                    s_fav_page--;
                    draw_fav_list();
                } else if (sx >= 340 && s_fav_page + 1 < total_pages) {
                    s_fav_page++;
                    draw_fav_list();
                }
            }
            return;
        }
        if (sy >= 30 && sy <= 218) {
            int i = s_fav_page * 5 + (sy - 30) / 38;
            if (i >= 0 && i < n) {
                if (sx >= 384) {            /* 右侧 [移除]: 直接删藏 */
                    fav_remove(i);
                    draw_fav_list();
                    return;
                }
                if (fav_get(i) == 0) {
                    s_qcount = 1;
                    s_qlist[0] = question_count + 1;   /* 收藏题 */
                    s_qidx = 0; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
                    s_state = 1;
                    draw_quiz();
                }
            }
        }
    } else if (s_state == 12) {             /* 薄弱点选科 */
        if (sy < 26) {                      /* 顶栏: 返回主菜单 */
            if (sx > 330) { s_state = 0; draw_menu(); }
            return;
        }
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
        if (sy < 26) {                      /* 顶栏: 返回主菜单 */
            if (sx > 330) { s_state = 0; draw_menu(); }
            return;
        }
        if (sy >= 240) {
            if (sx < 156) {                 /* AI 总结 (无总结时无效) */
                if (weak_get_ai(s_weak_subj)[0]) {
                    s_wpage = 0;
                    s_state = 16;
                    draw_weak_full();
                }
            } else if (sx < 308) {          /* AI 分析: 进入选题页 */
                s_state = 20;
                s_sel_page = 0;
                draw_weak_select();
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
            /* 仅"全屏查看总结 >"入口行触发全屏; 预览文字区留给翻页 */
            if (sy >= ly + 40 && sy <= ly + 56) {
                s_wpage = 0;
                s_state = 16;
                draw_weak_full();
                return;
            }
        }
        /* 列表区左右半屏翻页 (局部刷新, 顶栏不动) */
        if (sx < 240) {
            if (s_weak_list_page > 0) {
                s_weak_list_page--;
                draw_weak_page_body();
            }
        } else {
            if (s_weak_list_page + 1 < total_pages) {
                s_weak_list_page++;
                draw_weak_page_body();
            }
        }
    } else if (s_state == 14) {             /* 薄弱点失败: 点击返回 */
        s_state = 13;
        draw_weak_page();
    } else if (s_state == 20) {             /* 选题分析: 勾选/翻页/底部按钮 */
        if (sy < 26) {                      /* 顶栏: 返回错题页 */
            if (sx > 330) { s_state = 13; draw_weak_page(); }
            return;
        }
        if (s_sel_warn) {                   /* "请先勾选"提示页: 点击任意处返回 */
            s_sel_warn = 0;
            draw_weak_select();
            return;
        }
        int n = weak_count(s_weak_subj);
        if (sy >= 240) {                    /* 底部按钮 */
            if (sx < 156) {                 /* 全选/清空 (全选最多勾 5 题) */
                int sel = 0;
                for (int i = 0; i < n && i < 20; i++)
                    if (s_sel_wrong[i]) sel++;
                if (sel == n) {             /* 全选状态 → 清空 */
                    for (int i = 0; i < n && i < 20; i++)
                        s_sel_wrong[i] = 0;
                } else {                    /* 勾选前 min(n,5) 题 */
                    int limit = n < 5 ? n : 5;
                    for (int i = 0; i < n && i < 20; i++)
                        s_sel_wrong[i] = i < limit;
                }
                draw_weak_select_body();
            } else if (sx < 308) {          /* 分析选中 */
                char topics[1500] = "";
                int sel = 0;
                for (int i = 0; i < n && i < 20; i++) {
                    if (s_sel_wrong[i]) {
                        char t[130];
                        /* 勾选索引=显示序 (0=最新), 与列表/编号一致 */
                        if (weak_get_wrong(s_weak_subj, weak_show_idx(i),
                                           t, sizeof(t)) == 0) {
                            char one[160];
                            snprintf(one, sizeof(one), "%d.%s；", sel + 1, t);
                            if (strlen(topics) + strlen(one) < sizeof(topics))
                                strcat(topics, one);
                            sel++;
                        }
                    }
                }
                if (sel == 0) {             /* 未选择: 提示页, 点击任意处返回 */
                    lcd_clear(s_th_bg);
                    text_center(110, "请先勾选要分析的题", RED, s_th_bg);
                    text_center(140, "点击返回", s_th_border, s_th_bg);
                    s_sel_warn = 1;
                    return;
                }
                weak_ai_do(topics);
            } else {                        /* 返回错题页 */
                s_state = 13;
                draw_weak_page();
            }
            return;
        }
        if (sy >= 48 && sy <= 238 && n > 0) {   /* 本页行点击: 勾选; 其余区域: 翻页 */
            int total_pages = (n + 3) / 4;
            int page_first = s_sel_page * 4;
            int i = page_first + (sy - 48) / 32;
            /* 只有点击当前页可见的行才勾选; 点空白/提示行/下一页行位一律翻页 */
            if (i >= page_first && i < page_first + 4 && i < n && i < 20) {
                if (!s_sel_wrong[i]) {      /* 新勾选: 限 5 题 (单次分析上限) */
                    int sel = 0;
                    for (int j = 0; j < n && j < 20; j++)
                        if (s_sel_wrong[j]) sel++;
                    if (sel >= 5) {
                        lcd_clear(s_th_bg);
                        text_center(110, "一次最多分析 5 题", RED, s_th_bg);
                        text_center(140, "请先取消部分勾选", s_th_border, s_th_bg);
                        text_center(160, "点击返回", s_th_border, s_th_bg);
                        s_sel_warn = 1;
                        return;
                    }
                }
                s_sel_wrong[i] = !s_sel_wrong[i];
                redraw_sel_row(i);          /* 局部刷新: 单行 + 计数行 */
                redraw_sel_count();
                return;
            }
            if (sx < 240) {
                if (s_sel_page > 0) { s_sel_page--; draw_weak_select_body(); }
            } else {
                if (s_sel_page + 1 < total_pages) { s_sel_page++; draw_weak_select_body(); }
            }
        }
    } else if (s_state == 22) {             /* 收藏选题: 勾选/翻页/底部按钮 */
        if (sy < 26) {                      /* 顶栏: 返回收藏列表 */
            if (sx > 330) { s_state = 11; draw_fav_list(); }
            return;
        }
        if (s_sel_warn) {                   /* "请先勾选"/"无收藏"提示页: 点击任意处返回 */
            s_sel_warn = 0;
            draw_fav_select();
            return;
        }
        int n = fav_count();
        if (sy >= 240) {                    /* 底部按钮 */
            if (sx < 156) {                 /* 全选/清空 (全选最多勾 5 题) */
                int sel = 0;
                for (int i = 0; i < n && i < 64; i++)
                    if (s_sel_fav[i]) sel++;
                if (sel == 0) {             /* 未选 → 勾选前 min(n,5) 题 */
                    int limit = n < 5 ? n : 5;
                    for (int i = 0; i < limit; i++)
                        s_sel_fav[i] = 1;
                } else {                    /* 已选 → 清空 */
                    for (int i = 0; i < n && i < 64; i++)
                        s_sel_fav[i] = 0;
                }
                draw_fav_select_body();
            } else if (sx < 308) {          /* 分析选中 */
                char topics[1500] = "";
                int sel = 0;
                const char *subj_first = NULL;
                int mixed = 0;
                for (int i = 0; i < n && i < 64; i++) {
                    if (s_sel_fav[i]) {
                        if (fav_get(i) == 0) {
                            char one[220];
                            snprintf(one, sizeof(one), "%d.[%s]%s；", sel + 1,
                                     g_fav_q.subject ? g_fav_q.subject : "?",
                                     g_fav_q.content);
                            if (!subj_first)
                                subj_first = g_fav_q.subject;
                            else if (strcmp(subj_first, g_fav_q.subject) != 0)
                                mixed = 1;
                            if (strlen(topics) + strlen(one) < sizeof(topics))
                                strcat(topics, one);
                            sel++;
                        }
                    }
                }
                if (sel == 0) {             /* 未选择: 提示页 */
                    lcd_clear(s_th_bg);
                    text_center(110, "请先勾选要分析的题", RED, s_th_bg);
                    text_center(140, "点击返回", s_th_border, s_th_bg);
                    s_sel_warn = 1;
                    return;
                }
                fav_ai_do(mixed ? "综合" : (subj_first ? subj_first : "综合"),
                          topics);
            } else {                        /* 返回收藏列表 */
                s_state = 11;
                draw_fav_list();
            }
            return;
        }
        if (sy >= 48 && sy <= 238 && n > 0) {   /* 行点击: 勾选; 其余: 翻页 */
            int total_pages = (n + 3) / 4;
            int page_first = s_sel_fav_page * 4;
            int i = page_first + (sy - 48) / 32;
            if (i >= page_first && i < page_first + 4 && i < n && i < 64) {
                if (!s_sel_fav[i]) {        /* 新勾选: 限 5 题 */
                    int sel = 0;
                    for (int j = 0; j < n && j < 64; j++)
                        if (s_sel_fav[j]) sel++;
                    if (sel >= 5) {
                        lcd_clear(s_th_bg);
                        text_center(110, "一次最多分析 5 题", RED, s_th_bg);
                        text_center(140, "请先取消部分勾选", s_th_border, s_th_bg);
                        text_center(160, "点击返回", s_th_border, s_th_bg);
                        s_sel_warn = 1;
                        return;
                    }
                }
                s_sel_fav[i] = !s_sel_fav[i];
                redraw_fav_row(i);
                redraw_fav_count();
                return;
            }
            if (sx < 240) {
                if (s_sel_fav_page > 0) { s_sel_fav_page--; draw_fav_select_body(); }
            } else {
                if (s_sel_fav_page + 1 < total_pages) { s_sel_fav_page++; draw_fav_select_body(); }
            }
        }
    } else if (s_state == 23) {             /* 收藏分析结果: 左翻上/右翻下, [返回收藏] */
        if (sy < 26) {                      /* 顶栏: 返回收藏列表 */
            if (sx > 330) { s_state = 11; draw_fav_list(); }
            return;
        }
        if (sy >= 250 && sx <= 130) {       /* 底部 [返回收藏] */
            s_state = 11;
            draw_fav_list();
            return;
        }
        int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
        if (sx < 240) {
            if (s_wpage > 0) {
                s_wpage--;
                draw_fav_ai_result();
            } else {
                s_state = 11;
                draw_fav_list();
            }
        } else {
            if (s_wpage + 1 < total_pages) {
                s_wpage++;
                draw_fav_ai_result();
            } else {
                s_state = 11;
                draw_fav_list();
            }
        }
    } else if (s_state == 1) {              /* 答题 */
        const quiz_q_t *q = get_question(s_qlist[s_qidx]);
        if (sy < 26) {
            if (sx > 330) {                 /* 顶栏右侧: 返回 */
                quiz_back();
                return;
            }
            if (sx >= 170 && sx <= 260) {   /* ★ 收藏/取消收藏 (含填空/解答题) */
                fav_toggle(q);
                if (s_qlist[0] == question_count + 1) {
                    /* 收藏题: 取消收藏后回收藏列表 */
                    s_state = 11;
                    draw_fav_list();
                } else {
                    draw_quiz();
                }
                return;
            }
        }
        /* 控件行 (y=102..117): 题目/选项 -/+ 翻页 */
        if (sy >= 102 && sy <= 117) {
            int qpages = (s_qtotal + 3) / 4;
            if (sx >= 56 && sx <= 152 && qpages > 1) {   /* 题目控件 */
                if (sx <= 86) {
                    if (s_qpage > 0) s_qpage--;
                } else if (sx >= 122 && s_qpage + 1 < qpages) {
                    s_qpage++;
                }
                draw_question_area(q);
                return;
            }
            if (sx >= 346 && sx <= 442 && s_opt_pages > 1) {  /* 选项控件 */
                if (sx <= 376) {
                    if (s_opt_page > 0) s_opt_page--;
                } else if (sx >= 412 && s_opt_page + 1 < s_opt_pages) {
                    s_opt_page++;
                }
                draw_option_area(q);
                return;
            }
        }
        /* 题目区 (y=30..101): 多页时点击翻下一页 (末页回首页) */
        if (sy >= 30 && sy <= 101) {
            int qpages = (s_qtotal + 3) / 4;
            if (qpages > 1) {
                if (s_qpage + 1 < qpages) s_qpage++;
                else s_qpage = 0;
                draw_question_area(q);
                return;
            }
        }
        if (q->is_choice == 1) {
            if (sy >= 118 && sy <= 206) {   /* 选项区: 点哪个答哪个 */
                int i = (sy >= 166) ? 2 : 0;
                i += (sx >= 244) ? 1 : 0;
                if (!s_answered) {
                    s_opt_sel = i;
                    ui_submit();
                }
            } else if (s_answered && sy >= 210 && sy <= 232 && sx > 330) {
                /* 收藏本题 (答完按钮); 收藏题查看页=取消并回列表 */
                fav_toggle(q);
                if (s_qlist[0] == question_count + 1) {
                    s_state = 11;
                    draw_fav_list();
                } else {
                    draw_quiz();
                }
            } else if (s_answered && sy > 214) {  /* 解析区: 进入解析页 */
                s_exp_page = 0;
                s_state = 3;
                draw_explain();
            }
        } else if (q->is_choice == 2) {     /* 填空: 点输入区弹键盘 */
            if (!s_answered && sy >= 118 && sy <= 206) {
                kb_open(4, s_fill_buf);
                s_state = 5;
                kb_draw();
            } else if (s_answered && sy > 210) {  /* 解析区: 进入解析页 */
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
                    else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0; draw_quiz(); }
                } else if (s_qidx == s_qcount - 1) { quiz_finished(); }
                else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0; draw_quiz(); }
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
                else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0; s_state = 1; draw_quiz(); }
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
        if (sy < 26) {                      /* 顶栏: 返回主菜单 */
            if (sx > 330) { s_state = 0; draw_menu(); }
            return;
        }
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
    } else if (s_state == 17) {             /* 知识库选科 */
        int col = (sx - 10) / 158, row = (sy - 40) / 70;
        if (col >= 0 && col < 3 && row >= 0 && row < 3) {
            s_kb_subj = row * 3 + col;
            s_kb_list_page = 0;
            s_state = 21;
            draw_kb_topics();
        }
    } else if (s_state == 21) {             /* 知识库主题列表 */
        int per = 5;
        int total_pages = (WEAK_KB_CNT + per - 1) / per;
        if (sy >= 240) {                    /* 底部按钮 */
            if (sx < 250) {                 /* 自定义主题 */
                s_kb_slot = kb_find_empty();
                if (s_kb_slot < 0) {        /* 已满 */
                    lcd_clear(s_th_bg);
                    text_center(110, "知识库已满 (6 个主题)", RED, s_th_bg);
                    text_center(140, "先删除不需要的主题", s_th_border, s_th_bg);
                    text_center(160, "点击返回", s_th_border, s_th_bg);
                    s_state = 18;
                    return;
                }
                s_kb_custom_mode = 1;
                kb_open(3, s_kb_custom);
                s_state = 5;
                kb_draw();
            } else {                        /* 返回选科 */
                s_state = 17;
                draw_kb_subject();
            }
            return;
        }
        if (sy >= 212 && sy <= 236 && total_pages > 1) {  /* 翻页行 */
            if (sx < 156 && s_kb_list_page > 0) {
                s_kb_list_page--;
                draw_kb_topics();
            } else if (sx >= 314 && s_kb_list_page + 1 < total_pages) {
                s_kb_list_page++;
                draw_kb_topics();
            }
            return;
        }
        if (sy >= 30 && sy <= 205) {        /* 主题行 */
            int slot = s_kb_list_page * per + (sy - 30) / 36;
            if (slot >= 0 && slot < WEAK_KB_CNT) {
                const char *n = weak_get_kb_name(s_kb_subj, slot);
                if (sx >= 400 && n[0]) {    /* 右侧 [删除] */
                    weak_clear_kb(s_kb_subj, slot);
                    draw_kb_topics();
                    return;
                }
                s_kb_slot = slot;
                if (n[0]) {                 /* 已生成: 查看全文 */
                    kb_load();
                    s_kb_page = 0;
                    s_state = 19;
                    draw_kb_full();
                } else if (slot < 4) {      /* 未生成预置: 直接生成 */
                    s_kb_custom_mode = 0;
                    kb_run();
                } else {                    /* 空扩展槽: 走自定义输入 */
                    s_kb_custom_mode = 1;
                    kb_open(3, s_kb_custom);
                    s_state = 5;
                    kb_draw();
                }
            }
        }
    } else if (s_state == 18) {             /* 知识库失败/无网/已满: 点击重试 */
        if (s_kb_custom_mode && !s_kb_custom[0]) {  /* 空主题名/已满提示: 回列表 */
            s_state = 21;
            draw_kb_topics();
        } else if (s_wifi_state == 2) {
            kb_run();
        } else {
            s_state = 21;
            draw_kb_topics();
        }
    } else if (s_state == 19) {             /* 知识库全文: 左翻上/右翻下, [重新生成] */
        if (sy >= 250 && sx <= 130) {       /* 底部 [重新生成] */
            kb_run();
            return;
        }
        int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
        if (sx < 240) {
            if (s_kb_page > 0) {
                s_kb_page--;
                draw_kb_full();
            } else {
                s_state = 21;
                draw_kb_topics();
            }
        } else {
            if (s_kb_page + 1 < total_pages) {
                s_kb_page++;
                draw_kb_full();
            } else {
                s_state = 21;
                draw_kb_topics();
            }
        }
    } else if (s_state == 4) {              /* 设置列表 */
        if (sy >= 248) {                    /* 底部按钮 */
            if (sx < 240) {                 /* 扫描 WiFi */
                lcd_clear(s_th_bg);
                text_center(130, "扫描中...", s_th_border, s_th_bg);
                wifi_scan();
                s_state = 6;
                draw_ap_list();
            } else {                        /* 手动输入 SSID */
                kb_open(0, s_wifi_ssid);
                s_state = 5;
                kb_draw();
            }
        } else if (sy >= 178 && sy <= 200) { /* 亮度: 左减右加 */
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
        } else if (sy >= 154 && sy <= 176) { /* 主题: 点击切换 */
            theme_apply((s_theme + 1) % 3);
            settings_save();
            draw_settings();
        } else if (sy >= 202 && sy <= 224) { /* 年级/难度: 左半=年级, 右半=难度 */
            if (sx < 240)
                s_grade = (s_grade + 1) % 3;
            else
                s_diff = (s_diff + 1) % 3;
            settings_save();
            draw_settings();
        } else if (sy >= 30 && sy <= 150) {
            int i = (sy - 30) / 30;
            if (i == 3) {                   /* 填空比例: 左减右加 5% */
                if (sx < 240) {
                    if (s_fill_pct > 0)
                        s_fill_pct -= 5;
                } else {
                    if (s_fill_pct < 100)
                        s_fill_pct += 5;
                }
                settings_save();
                draw_settings();
            } else if (i == 2) {            /* API Key: 网页输入 */
                if (s_wifi_state == 2)
                    http_server_start();
                s_state = 7;
                draw_api_page();
            } else if (i >= 0 && i < 2) {
                kb_open(i, i == 0 ? s_wifi_ssid : s_wifi_pass);
                s_state = 5;
                kb_draw();
            }
        }
    } else if (s_state == 6) {              /* WiFi 列表: 点选 AP */
        if (sy >= 30 && s_ap_count > 0) {
            int i = (sy - 30) / 32;
            if (i >= 0 && i < s_ap_count && i < 7) {
                memcpy(s_wifi_ssid, s_ap_list[i].ssid, 32);
                s_wifi_ssid[32] = 0;
                kb_open(1, s_wifi_pass);   /* 输入密码 */
                s_state = 5;
                kb_draw();
            }
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
                s_qidx = 0; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
                s_state = 1;
                draw_quiz();
            }
        }
    } else if (s_state == 1) {          /* 答题 */
        const quiz_q_t *q = get_question(s_qlist[s_qidx]);
        if (ev == 2) {                  /* 长按 */
            if (q->is_choice == 1 && !s_answered) {
                ui_submit();            /* 选择题: 提交答案 */
            } else if (q->is_choice == 2 && !s_answered) {
                /* 填空未答: 长按跳过 (不判分) */
                if (s_qidx == s_qcount - 1) {
                    quiz_finished();} else {
                    s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
                    draw_quiz();
                }
            } else if (s_qidx == s_qcount - 1) {
                quiz_finished();} else {
                s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
                draw_quiz();
            }
        } else if (ev == 1) {           /* 短按 */
            if (q->is_choice == 1) {
                if (!s_answered) {
                    s_opt_sel = (s_opt_sel + 1) % 4;
                    redraw_option_full(s_opt_sel);           /* 新选中 */
                    redraw_option_full((s_opt_sel + 3) % 4); /* 旧选中 */
                } else {
                    if (s_qidx == s_qcount - 1) {
                        quiz_finished();} else {
                        s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
                        draw_quiz();
                    }
                }
            } else if (q->is_choice == 2) {
                /* 填空: 答完后短按下一题 (未答时短按无操作) */
                if (s_answered) {
                    if (s_qidx == s_qcount - 1) {
                        quiz_finished();} else {
                        s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
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
                        s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0;
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
            else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0; s_state = 1; draw_quiz(); }
        } else if (ev == 1) {
            if (s_exp_page + 1 < total_pages) {
                s_exp_page++;
                draw_explain();
            } else {
                if (s_qidx == s_qcount - 1) { quiz_finished(); }
                else { s_qidx++; s_opt_sel = 0; s_answered = 0; s_show_ans = 0; s_fill_buf[0] = 0; s_fill_ok = 0; s_state = 1; draw_quiz(); }
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
    } else if (s_state == 5) {          /* 键盘: 长按返回 */
        if (ev == 2) {
            if (kb_field() == 3) {      /* 知识库主题输入 → 回主题列表 */
                s_state = 21;
                draw_kb_topics();
            } else if (kb_field() == 4) { /* 填空 → 回题目页 */
                s_state = 1;
                draw_quiz();
            } else {
                s_state = 4;
                draw_settings();
            }
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
    } else if (s_state == 17 || s_state == 18 || s_state == 21) { /* 知识库: 长按返回菜单 */
        if (ev == 2) {
            s_state = 0;
            draw_menu();
        }
    } else if (s_state == 19) {             /* 知识库全文: 短按翻页, 长按回主题 */
        int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
        if (ev == 2) {
            s_state = 21;
            draw_kb_topics();
        } else if (ev == 1) {
            if (s_kb_page + 1 < total_pages) {
                s_kb_page++;
                draw_kb_full();
            } else {
                s_state = 21;
                draw_kb_topics();
            }
        }
    } else if (s_state == 20) {             /* 选题分析: 短按翻页, 长按返回 */
        int n = weak_count(s_weak_subj);
        int total_pages = (n + 3) / 4;
        if (total_pages < 1) total_pages = 1;
        if (ev == 2) {
            s_state = 13;
            draw_weak_page();
        } else if (ev == 1) {
            if (s_sel_page + 1 < total_pages) {
                s_sel_page++;
                draw_weak_select_body();
            } else {
                s_sel_page = 0;
                draw_weak_select_body();
            }
        }
    } else if (s_state == 22) {             /* 收藏选题: 短按翻页, 长按返回收藏 */
        int n = fav_count();
        int total_pages = (n + 3) / 4;
        if (total_pages < 1) total_pages = 1;
        if (ev == 2) {
            s_state = 11;
            draw_fav_list();
        } else if (ev == 1) {
            if (s_sel_fav_page + 1 < total_pages) {
                s_sel_fav_page++;
                draw_fav_select_body();
            } else {
                s_sel_fav_page = 0;
                draw_fav_select_body();
            }
        }
    } else if (s_state == 23) {             /* 收藏分析结果: 短按翻页, 长按返回收藏 */
        int total_pages = (s_wtotal + FULL_LINES - 1) / FULL_LINES;
        if (ev == 2) {
            s_state = 11;
            draw_fav_list();
        } else if (ev == 1) {
            if (s_wpage + 1 < total_pages) {
                s_wpage++;
                draw_fav_ai_result();
            } else {
                s_state = 11;
                draw_fav_list();
            }
        }
    }
}

/* 提交答案 (长按选项时调用), 局部刷新提速 */
static void ui_submit(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    if (q->is_choice != 1 || s_answered)
        return;
    s_answered = 1;
    s_total++;
    int ok = (s_opt_sel == q->answer_idx);
    int si = subj_of(q->subject);
    if (ok) {
        s_correct++;
    } else if (si >= 0) {
        /* 答错: 记录到该科薄弱点 */
        weak_add_wrong(si, q->content);
    }
    /* AI 题: 记录知识点作答统计 (答对答错都记, 驱动薄弱板块强化选题) */
    if (si >= 0 && s_qlist[s_qidx] == question_count)
        quiz_record_answer(si, g_ai_q.topic, ok);

    /* 只刷新选项+答案区 (题目/顶栏不动; 控件行重画) */
    lcd_fill_rect(0, 104, 479, 271, s_th_bg);
    draw_pager(10, 102, "题目", s_qpage, (s_qtotal + 3) / 4);
    draw_pager(300, 102, "选项", s_opt_page, s_opt_pages);
    for (int i = 0; i < 4; i++)
        draw_option(i);
    for (int i = 0; i < 4; i++) {
        int x = (i % 2) ? 284 : 50, y = OPT_Y0 + 10 + (i / 2) * (OPT_H + OPT_GAP);
        text_wrap_skip(x, y, q->options[i], s_th_fg, s_th_bg, 186, 16, 2,
                       s_opt_page * 2, NULL);
    }
    const char *res = (s_opt_sel == q->answer_idx) ? "回答正确" : "回答错误";
    lcd_draw_text(10, 212, res, (s_opt_sel == q->answer_idx) ? GREEN : RED, s_th_bg);
    /* 答错且该考点处于薄弱强化区: 红色标记 (避开收藏按钮/翻页提示) */
    if (!ok && si >= 0 && s_qlist[s_qidx] == question_count &&
        g_ai_q.topic && quiz_topic_weak(si, g_ai_q.topic))
        lcd_draw_text(270, 212, "薄弱考点", RED, s_th_bg);
    /* 收藏按钮 + 解析翻页提示 (同排, 复用答案条) */
    lcd_draw_rect(340, 210, 470, 230, s_th_border);
    lcd_draw_text(346, 213, fav_contains(q) ? "★已藏" : "☆收藏本题",
                  fav_contains(q) ? RED : s_th_fg, s_th_bg);
    if (q->explanation && q->explanation[0]) {
        int el = 0;
        text_wrap_skip(10, 232, q->explanation, s_th_fg, s_th_bg, 460, 16, 2, 0, &el);
        if (el > 2)
            lcd_draw_text(110, 212, "点解析翻页", s_th_border, s_th_bg);
    }
}

/* 填空判分: 双方去掉所有空格 + 忽略大小写.
 * AI 答案偶有 "2, 2" (逗号后空格) / 英文短语带空格, 去掉空格后比较更宽容 */
static int fill_match(const char *a, const char *b)
{
    char pa[64], pb[64];
    int j = 0;
    for (const char *p = a; *p && j < 63; p++)
        if (*p != ' ' && *p != '\t')
            pa[j++] = *p;
    pa[j] = 0;
    j = 0;
    for (const char *p = b; *p && j < 63; p++)
        if (*p != ' ' && *p != '\t')
            pb[j++] = *p;
    pb[j] = 0;
    return strcasecmp(pa, pb) == 0;
}

/* 填空题判分 (键盘 Enter 后调用): 忽略大小写 + 首尾空格比较 */
static void ui_submit_fill(void)
{
    const quiz_q_t *q = get_question(s_qlist[s_qidx]);
    if (q->is_choice != 2 || s_answered)
        return;
    /* 去首尾空格 */
    char ans[64];
    int i = 0, j = 0;
    while (s_fill_buf[i] == ' ')
        i++;
    for (; s_fill_buf[i] && j < (int)sizeof(ans) - 1; i++)
        ans[j++] = s_fill_buf[i];
    while (j > 0 && ans[j - 1] == ' ')
        j--;
    ans[j] = 0;

    s_answered = 1;
    s_total++;
    int ok = (ans[0] && q->answer_text &&
              fill_match(ans, q->answer_text));
    int si = subj_of(q->subject);
    if (ok) {
        s_correct++;
        s_fill_ok = 1;
    } else {
        s_fill_ok = 0;
        /* 答错: 记录到该科薄弱点 */
        if (si >= 0)
            weak_add_wrong(si, q->content);
    }
    /* AI 题: 记录知识点作答统计 (答对答错都记, 驱动薄弱板块强化选题) */
    if (si >= 0 && s_qlist[s_qidx] == question_count)
        quiz_record_answer(si, g_ai_q.topic, ok);
    draw_quiz();
}

void app_main(void)
{
    ESP_LOGI(TAG, "st6201 quiz app start");

    gfx_init();            /* SPI 总线 + LCD 初始化 */
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
            if (s_state == 5) {              /* 键盘: 按下立即高亮 (脏矩形) */
                int sx = t_px * LCD_WIDTH / s_xmax;
                int sy = t_py * LCD_HEIGHT / s_ymax;
                kb_press(sx, sy);
            }
        } else if (tnow && prev_touch && !t_long_sent) {
            if (s_state == 5) {
                kb_hold(now - t_press_at);   /* 键盘: 退格长按连删 */
            } else if (now - t_press_at >= 900) {
                int lp_ok = (s_state == 4 || s_state == 6 || s_state == 7
                             || s_state == 8 || s_state == 10 || s_state == 11
                             || s_state == 12 || s_state == 13 || s_state == 14
                             || s_state == 15 || s_state == 16
                             || s_state == 17 || s_state == 18 || s_state == 19
                             || s_state == 20 || s_state == 21
                             || s_state == 22 || s_state == 23);
                if (s_state == 1)
                    lp_ok = (t_py < 26 * s_ymax / LCD_HEIGHT);  /* 顶栏长按返回 */
                else if (s_state == 3)
                    lp_ok = 1;                /* 解析页长按返回答题 */
                if (lp_ok) {
                    t_long_sent = 1;
                    ESP_LOGI(TAG, "touch long press");
                    if (s_state == 1)
                        quiz_back();          /* 顶栏长按 = 返回 */
                    else if (s_state == 3) {
                        s_state = 1;          /* 解析页长按 = 返回答题页 */
                        draw_quiz();
                    } else {
                        ui_handle(2);         /* 触摸长按 = 返回 */
                    }
                }
            }
        } else if (!tnow && prev_touch && !t_long_sent) {
            int sx = t_px * LCD_WIDTH / s_xmax;
            int sy = t_py * LCD_HEIGHT / s_ymax;
            ESP_LOGI(TAG, "tap %d,%d", sx, sy);
            if (s_state == 5) {
                /* 顶栏返回按钮 (放弃输入): 填空→题目页, 主题→列表, 其他→设置 */
                if (sy < 26 && sx >= 240 && sx < 330) {
                    int f = kb_field();
                    if (f == 4) {
                        s_state = 1;
                        draw_quiz();
                    } else if (f == 3) {
                        s_state = 21;
                        draw_kb_topics();
                    } else {
                        s_state = 4;
                        draw_settings();
                    }
                } else if (kb_release(sx, sy)) {
                    int f = kb_field();
                    if (f == 0) {
                        strncpy(s_wifi_ssid, kb_buffer(), sizeof(s_wifi_ssid) - 1);
                        s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = 0;
                    } else if (f == 1) {
                        strncpy(s_wifi_pass, kb_buffer(), sizeof(s_wifi_pass) - 1);
                        s_wifi_pass[sizeof(s_wifi_pass) - 1] = 0;
                        wifi_connect_now();
                    } else if (f == 2) {
                        strncpy(s_api_key, kb_buffer(), sizeof(s_api_key) - 1);
                        s_api_key[sizeof(s_api_key) - 1] = 0;
                    } else if (f == 4) {      /* 填空答案: 判分 */
                        strncpy(s_fill_buf, kb_buffer(), sizeof(s_fill_buf) - 1);
                        s_fill_buf[sizeof(s_fill_buf) - 1] = 0;
                        s_state = 1;
                        ui_submit_fill();
                        goto kb_done;
                    } else {                  /* 知识库自定义主题 */
                        strncpy(s_kb_custom, kb_buffer(), sizeof(s_kb_custom) - 1);
                        s_kb_custom[sizeof(s_kb_custom) - 1] = 0;
                        if (!s_kb_custom[0]) {  /* 空主题名: 提示 */
                            lcd_clear(s_th_bg);
                            text_center(110, "主题名不能为空", RED, s_th_bg);
                            text_center(140, "点击返回主题列表", s_th_border, s_th_bg);
                            s_state = 18;
                            goto kb_done;
                        }
                        s_kb_custom_mode = 1;
                        s_state = 18;
                        kb_run();
                        goto kb_done;
                    }
                    s_state = 4;
                    draw_settings();
                kb_done: ;
                }
            } else {
                ui_touch(sx, sy);
            }
        }
        prev_touch = tnow;

        /* 顶栏时间刷新: 每分钟检查一次 (分钟变化才重画时间区) */
        static uint32_t t_last_clock = 0;
        if (s_time_str[0] && now - t_last_clock > 30000) {
            t_last_clock = now;
            char old[16];
            strncpy(old, s_time_str, sizeof(old) - 1);
            old[sizeof(old) - 1] = 0;
            time_refresh();
            if (strcmp(old, s_time_str) != 0 && s_state != 5) {
                lcd_fill_rect(240, 2, 330, 15, s_th_bar);   /* 清时间区 */
                draw_small_str(240, 4, s_time_str, s_th_bar_fg, s_th_bar);
            }
        }

        /* BOOT 键兜底 */
        uint32_t press_ms = 0;
        int ev = btn_scan(&press_ms);
        if (ev)
            ui_handle(ev);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}