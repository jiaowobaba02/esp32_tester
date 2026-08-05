/**
 * weak.c — 每科薄弱点记录 (weakness 分区, 9 科 × 128KB 槽位, v5 布局)
 *
 * 槽位布局 (每科 128KB):
 *   [0]      magic (2) + 错题数 (2)
 *   [4]      错题区: 每项 [len(4)][content(≤124)], 最多 20 项
 *   [8192]   AI 简要分析文本 (≤8KB, 以 \0 结尾)
 *   [16384]  知识库区 (112KB): 7 主题槽 × 16KB
 * v4 → v5 迁移: 旧错题逐科搬入 (旧 64KB/科), 旧知识库 (截断乱码) 丢弃重建
 */
#include "weak.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "weak";

#define WEAK_SLOT   131072
#define WEAK_MAGIC  0x574B
#define WEAK_MAX    20
#define WEAK_AI_OFF 8192
#define WEAK_AI_SIZE 8192     /* AI 区 [8192, 16384): 2 扇区 (总结 ≤8KB) */
#define WEAK_KB_OFF 16384     /* 知识库区 [16384, 131072): 7 主题槽 × 16KB */
#define WEAK_KB_NAME_SZ 64    /* 槽内主题名区 (含 \0) */
#define WEAK_CSTR   124   /* 每项 4+124=128 字节, len 用 4 字节 (flash 对齐) */
#define WEAK_KB_VER 5

static void weak_migrate_v4(void);   /* 定义在下方 (v4 → v5 布局迁移) */

static const esp_partition_t *s_part;
static char s_ai_buf[8192];
static char s_kb_buf[16384];   /* 知识库内容缓冲 (单槽最大) */
static char s_kb_name_buf[WEAK_KB_NAME_SZ];

void weak_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                      "weakness");
    if (!s_part) {
        ESP_LOGE(TAG, "partition not found!");
        return;
    }
    ESP_LOGI(TAG, "partition found: off=0x%lx size=0x%lx",
             (unsigned long)s_part->address, (unsigned long)s_part->size);
    uint16_t magic = 0;
    esp_partition_read(s_part, 0, &magic, 2);
    if (magic != WEAK_MAGIC) {
        esp_partition_erase_range(s_part, 0, s_part->size);
        uint32_t init = WEAK_MAGIC;          /* 只写 magic (count 用扫描) */
        esp_partition_write(s_part, 0, &init, 4);
    }
    /* KB 区格式版本: v5 = 每科 128KB (错题 8KB + AI 8KB + KB 7×16KB) */
    nvs_handle_t h;
    int32_t ver = 0;
    esp_err_t ne = nvs_open("weak", NVS_READWRITE, &h);
    ESP_LOGI(TAG, "nvs weak open=%s", esp_err_to_name(ne));
    if (ne == ESP_OK) {
        esp_err_t ge = nvs_get_i32(h, "kb_ver", &ver);
        ESP_LOGI(TAG, "kb_ver get=%s val=%ld", esp_err_to_name(ge), (long)ver);
        if (ver != WEAK_KB_VER) {
            if (ver == 4) {
                weak_migrate_v4();          /* v4 → v5: 错题迁移, KB 重建 */
            } else {                        /* 更旧/未知布局: 整分区重建 */
                esp_err_t ee = esp_partition_erase_range(s_part, 0, s_part->size);
                ESP_LOGI(TAG, "full erase ret=%s", esp_err_to_name(ee));
            }
            nvs_set_i32(h, "kb_ver", WEAK_KB_VER);
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

/* v4 → v5 迁移: 旧布局每科 64KB (0..0x90000), 新布局每科 128KB.
 * 旧错题区 [4..4096) 逐科读出 → 整体擦除旧区域 (含旧 KB 乱码数据,
 * 及已被 fav_init 迁移走的旧收藏残留) → 按新布局写回错题。
 * 一次性迁移, 首次升级启动约多花 ~6 秒 (144 扇区擦除)。 */
static void weak_migrate_v4(void)
{
    enum { OLD_SLOT = 65536 };
    static char items[9][WEAK_MAX][WEAK_CSTR + 1];
    static int cnt[9];
    for (int si = 0; si < 9; si++) {
        cnt[si] = 0;
        for (int i = 0; i < WEAK_MAX; i++) {
            uint32_t len = 0;
            esp_partition_read(s_part, si * OLD_SLOT + 4 + i * (4 + WEAK_CSTR),
                               &len, 4);
            if (len == 0 || len == 0xFFFFFFFF || len > WEAK_CSTR)
                break;
            esp_partition_read(s_part, si * OLD_SLOT + 4 + i * (4 + WEAK_CSTR) + 4,
                               items[si][cnt[si]], len);
            items[si][cnt[si]][len] = 0;
            cnt[si]++;
        }
    }
    ESP_LOGI(TAG, "migrate v4->v5: erasing old 576KB area (~6s)...");
    esp_partition_erase_range(s_part, 0, 9 * OLD_SLOT);
    uint32_t magic = WEAK_MAGIC;
    esp_partition_write(s_part, 0, &magic, 4);
    for (int si = 0; si < 9; si++) {
        for (int i = 0; i < cnt[si]; i++) {
            uint32_t len = (uint32_t)strlen(items[si][i]);
            uint32_t pos = (uint32_t)si * WEAK_SLOT + 4 + i * (4 + WEAK_CSTR);
            esp_partition_write(s_part, pos, &len, 4);
            esp_partition_write(s_part, pos + 4, items[si][i], len);
        }
        if (cnt[si])
            ESP_LOGI(TAG, "migrate: subj %d kept %d wrongs", si, cnt[si]);
    }
    ESP_LOGI(TAG, "migrate done (kb v5)");
}

static uint32_t off_of(int subject_idx)
{
    return (uint32_t)subject_idx * WEAK_SLOT;
}

/* count 不存储: 扫描错题区 (flash 不能 0→1 写, 存 count 无法更新) */
int weak_count(int subject_idx)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8)
        return 0;
    uint32_t base = off_of(subject_idx);
    for (int i = 0; i < WEAK_MAX; i++) {
        uint32_t len = 0;
        esp_partition_read(s_part, base + 4 + (uint32_t)i * (4 + WEAK_CSTR), &len, 4);
        if (len == 0 || len == 0xFFFFFFFF)
            return i;
    }
    return WEAK_MAX;
}

int weak_add_wrong(int subject_idx, const char *content)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8 || !content) {
        ESP_LOGE(TAG, "add wrong: bad args part=%p", (void *)s_part);
        return -1;
    }
    int c = weak_count(subject_idx);
    if (c >= WEAK_MAX)
        return -1;
    ESP_LOGI(TAG, "add wrong: subj=%d cnt=%d %.30s", subject_idx, c, content);
    /* 去重 */
    char tmp[WEAK_CSTR + 1];
    for (int i = 0; i < c; i++) {
        if (weak_get_wrong(subject_idx, i, tmp, sizeof(tmp)) == 0 &&
            strcmp(tmp, content) == 0)
            return 1;
    }
    uint32_t base = off_of(subject_idx);
    uint32_t len = (uint32_t)strlen(content);
    if (len > WEAK_CSTR) {
        len = WEAK_CSTR;
        /* UTF-8 边界回退: 避免切断半个汉字 (导致 JSON 非法) */
        while (len > 0 && ((uint8_t)content[len] & 0xC0) == 0x80)
            len--;
        if (len > 0 && (uint8_t)content[len] >= 0xC0)
            len--;
    }
    uint32_t pos = base + 4 + (uint32_t)c * (4 + WEAK_CSTR);   /* 每项 128, 4 对齐 */
    esp_partition_write(s_part, pos, &len, 4);
    esp_partition_write(s_part, pos + 4, content, len);
    return 0;   /* count 由 weak_count() 扫描得出 */
}

int weak_get_wrong(int subject_idx, int idx, char *buf, int bufsz)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8)
        return -1;
    int c = weak_count(subject_idx);
    if (idx < 0 || idx >= c)
        return -1;
    uint32_t pos = off_of(subject_idx) + 4 + (uint32_t)idx * (4 + WEAK_CSTR);
    uint32_t len = 0;
    esp_partition_read(s_part, pos, &len, 4);
    if (len > (uint32_t)bufsz - 1)
        len = (uint32_t)bufsz - 1;
    esp_partition_read(s_part, pos + 4, buf, len);
    buf[len] = 0;
    return 0;
}

const char *weak_get_ai(int subject_idx)
{
    s_ai_buf[0] = 0;
    if (!s_part || subject_idx < 0 || subject_idx > 8)
        return s_ai_buf;
    uint32_t pos = off_of(subject_idx) + WEAK_AI_OFF;
    esp_partition_read(s_part, pos, s_ai_buf, sizeof(s_ai_buf) - 1);
    s_ai_buf[sizeof(s_ai_buf) - 1] = 0;
    return s_ai_buf;
}

void weak_set_ai(int subject_idx, const char *text)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8 || !text)
        return;
    uint32_t pos = off_of(subject_idx) + WEAK_AI_OFF;
    uint32_t len = (uint32_t)strlen(text);
    if (len >= WEAK_AI_SIZE) {                /* 不得超过 AI 区 */
        len = WEAK_AI_SIZE - 2;
        while (len > 0 && ((uint8_t)text[len] & 0xC0) == 0x80)   /* UTF-8 边界回退 */
            len--;
        if (len > 0 && (uint8_t)text[len] >= 0xC0)
            len--;
    }
    /* flash 只能 1→0 写: 覆盖写会与旧数据按位与产生乱码。
     * AI 区 [4096, 8192) 恰好 1 个整扇区, 先擦除再写 (不影响错题区) */
    esp_partition_erase_range(s_part, pos, WEAK_AI_SIZE);
    esp_partition_write(s_part, pos, text, len + 1);
}

/* ---------- 知识库 (每科 7 主题槽, 每槽 16KB = 4 整扇区)
 * 槽布局: [0..63] 主题名 (\0 结尾) + [64..] 内容 (\0 结尾); 空槽全 0xFF */
const char *weak_get_kb_name(int subject_idx, int slot)
{
    s_kb_name_buf[0] = 0;
    if (!s_part || subject_idx < 0 || subject_idx > 8 ||
        slot < 0 || slot >= WEAK_KB_CNT)
        return s_kb_name_buf;
    uint32_t pos = off_of(subject_idx) + WEAK_KB_OFF + (uint32_t)slot * WEAK_KB_SLOT;
    esp_partition_read(s_part, pos, s_kb_name_buf, WEAK_KB_NAME_SZ);
    s_kb_name_buf[WEAK_KB_NAME_SZ - 1] = 0;
    if ((uint8_t)s_kb_name_buf[0] == 0xFF)   /* 空槽 */
        s_kb_name_buf[0] = 0;
    return s_kb_name_buf;
}

const char *weak_get_kb(int subject_idx, int slot)
{
    s_kb_buf[0] = 0;
    if (!s_part || subject_idx < 0 || subject_idx > 8 ||
        slot < 0 || slot >= WEAK_KB_CNT)
        return s_kb_buf;
    uint32_t pos = off_of(subject_idx) + WEAK_KB_OFF + (uint32_t)slot * WEAK_KB_SLOT;
    uint8_t hdr = 0;
    esp_partition_read(s_part, pos, &hdr, 1);
    if (hdr == 0xFF)                          /* 空槽 */
        return s_kb_buf;
    esp_partition_read(s_part, pos + WEAK_KB_NAME_SZ, s_kb_buf, sizeof(s_kb_buf) - 1);
    s_kb_buf[sizeof(s_kb_buf) - 1] = 0;
    return s_kb_buf;
}

void weak_set_kb(int subject_idx, int slot, const char *name, const char *text)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8 ||
        slot < 0 || slot >= WEAK_KB_CNT || !text)
        return;
    uint32_t pos = off_of(subject_idx) + WEAK_KB_OFF + (uint32_t)slot * WEAK_KB_SLOT;
    uint32_t len = (uint32_t)strlen(text);
    if (len >= WEAK_KB_SLOT - WEAK_KB_NAME_SZ - 1) {   /* 不得超过槽内内容区 */
        len = WEAK_KB_SLOT - WEAK_KB_NAME_SZ - 2;
        while (len > 0 && ((uint8_t)text[len] & 0xC0) == 0x80)   /* UTF-8 边界回退 */
            len--;
        if (len > 0 && (uint8_t)text[len] >= 0xC0)
            len--;
    }
    esp_partition_erase_range(s_part, pos, WEAK_KB_SLOT);   /* 整扇区擦除再写 */
    esp_partition_write(s_part, pos, name, (uint32_t)strlen(name) + 1);
    esp_partition_write(s_part, pos + WEAK_KB_NAME_SZ, text, len + 1);
}

void weak_clear_kb(int subject_idx, int slot)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8 ||
        slot < 0 || slot >= WEAK_KB_CNT)
        return;
    uint32_t pos = off_of(subject_idx) + WEAK_KB_OFF + (uint32_t)slot * WEAK_KB_SLOT;
    esp_partition_erase_range(s_part, pos, WEAK_KB_SLOT);
}


/* ---------- 知识库容量查询 (容量显示 / 生成前余量检查) ---------- */
int weak_kb_used_slots(int subject_idx)
{
    if (!s_part || subject_idx < 0 || subject_idx > 8)
        return 0;
    int n = 0;
    for (int i = 0; i < WEAK_KB_CNT; i++)
        if (weak_get_kb_name(subject_idx, i)[0])
            n++;
    return n;
}

int weak_kb_capacity_bytes(void)
{
    return WEAK_SLOT - WEAK_KB_OFF;   /* 每科知识库区总容量 */
}

/* 剩余按整槽计 (每槽 4 扇区擦写, 物理占用即整槽) */
int weak_kb_remain_bytes(int subject_idx)
{
    return weak_kb_capacity_bytes()
           - weak_kb_used_slots(subject_idx) * WEAK_KB_SLOT;
}
