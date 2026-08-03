/**
 * weak.c — 每科薄弱点记录 (weakness 分区, 9 科 × 32KB 槽位)
 *
 * 槽位布局 (每科 32KB):
 *   [0]    magic (2) + 错题数 (2)
 *   [4]    错题区: 每项 [len(2)][content(≤120)], 最多 20 项
 *   [4096] AI 简要分析文本 (≤28KB, 以 \0 结尾)
 */
#include "weak.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "weak";

#define WEAK_SLOT   32768
#define WEAK_MAGIC  0x574B
#define WEAK_MAX    20
#define WEAK_AI_OFF 4096
#define WEAK_CSTR   124   /* 每项 4+124=128 字节, len 用 4 字节 (flash 对齐) */

static const esp_partition_t *s_part;
static char s_ai_buf[1024];

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
    esp_partition_write(s_part, pos, text, (uint32_t)strlen(text) + 1);
}
