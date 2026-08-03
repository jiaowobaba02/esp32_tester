/**
 * fav.c — 题目收藏 (favorites 分区裸写, 1024B/槽位, 最多 511 题)
 *
 * 槽位 0: [magic][count]
 * 槽位 N: [magic][len][data]  data 用 \x01 分隔字段:
 *         subject \x01 content \x01 optA \x01 optB \x01 optC \x01 optD
 *         \x01 answer_idx \x01 explanation
 */
#include "fav.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "fav";

#define FAV_SLOT  1024
#define FAV_MAGIC 0x4656   /* "FV" */

typedef struct {
    uint16_t magic;
    uint16_t len;
    uint8_t data[FAV_SLOT - 4];
} __attribute__((packed)) fav_slot_t;

quiz_q_t g_fav_q;

static const esp_partition_t *s_part;
static char s_fv_subj[33], s_fv_content[512], s_fv_opts[4][256], s_fv_expl[512];
static char s_fv_segs[8][520];

void fav_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                      "favorites");
    if (!s_part) {
        ESP_LOGE(TAG, "partition not found!");
        return;
    }
    ESP_LOGI(TAG, "partition found: off=0x%lx size=0x%lx",
             (unsigned long)s_part->address, (unsigned long)s_part->size);
    uint16_t magic = 0;
    esp_partition_read(s_part, 0, &magic, 2);
    if (magic != FAV_MAGIC) {
        esp_partition_erase_range(s_part, 0, s_part->size);
        uint16_t init = FAV_MAGIC;               /* 只写 magic (count 用扫描) */
        esp_partition_write(s_part, 0, &init, 4);
    }
}

/* count 不存储: 扫描槽位 (flash 不能 0→1 写, 存 count 无法更新) */
int fav_count(void)
{
    if (!s_part)
        return 0;
    uint16_t magic;
    for (int i = 1; i <= 511; i++) {
        esp_partition_read(s_part, (uint32_t)i * FAV_SLOT, &magic, 2);
        if (magic != FAV_MAGIC)
            return i - 1;
    }
    return 511;
}

/* 调试: 打印分区头 */
void fav_debug(void)
{
    if (!s_part)
        return;
    uint16_t magic = 0;
    esp_partition_read(s_part, 0, &magic, 2);
    ESP_LOGI(TAG, "hdr: magic=0x%04X count=%d (scan)", magic, fav_count());
}

static int fav_is_dup(const quiz_q_t *q)
{
    for (int i = 0; i < fav_count(); i++) {
        fav_slot_t slot;
        esp_partition_read(s_part, (i + 1) * FAV_SLOT, &slot, sizeof(slot));
        if (slot.magic != FAV_MAGIC || slot.len <= 1)
            continue;
        slot.data[slot.len] = 0;
        if (strcmp((char *)slot.data + strlen(q->subject) + 1, q->content) == 0)
            return 1;
    }
    return 0;
}

int fav_add(const quiz_q_t *q)
{
    if (!s_part) {
        ESP_LOGE(TAG, "add: no partition");
        return -1;
    }
    if (fav_is_dup(q))
        return 1;                      /* 已收藏 */
    int cnt = fav_count();
    if (cnt >= 511)
        return -1;
    char buf[FAV_SLOT - 4];
    ESP_LOGI(TAG, "add: subj=%s cnt=%d", q->subject ? q->subject : "?", cnt);
    snprintf(buf, sizeof(buf), "%s\x01%s\x01%s\x01%s\x01%s\x01%s\x01%d\x01%s",
             q->subject ? q->subject : "",
             q->content ? q->content : "",
             q->options[0] ? q->options[0] : "",
             q->options[1] ? q->options[1] : "",
             q->options[2] ? q->options[2] : "",
             q->options[3] ? q->options[3] : "",
             q->answer_idx,
             q->explanation ? q->explanation : "");
    fav_slot_t slot;
    slot.magic = FAV_MAGIC;
    slot.len = (uint16_t)strlen(buf);
    memset(slot.data, 0, sizeof(slot.data));
    memcpy(slot.data, buf, slot.len);
    esp_partition_write(s_part, (cnt + 1) * FAV_SLOT, &slot, sizeof(slot));
    return 0;   /* count 由 fav_count() 扫描得出 */
}

int fav_remove(int idx)
{
    if (!s_part)
        return -1;
    int cnt = fav_count();
    if (idx < 0 || idx >= cnt)
        return -1;
    /* 后续槽位移前 */
    fav_slot_t slot;
    for (int i = idx + 1; i < cnt; i++) {
        esp_partition_read(s_part, (i + 1) * FAV_SLOT, &slot, sizeof(slot));
        esp_partition_write(s_part, (i) * FAV_SLOT, &slot, sizeof(slot));
    }
    /* 清最后一个槽位 */
    memset(&slot, 0xFF, sizeof(slot));
    esp_partition_write(s_part, cnt * FAV_SLOT, &slot, sizeof(slot));
    return 0;   /* count 由 fav_count() 扫描得出 */
}

int fav_get(int idx)
{
    if (!s_part)
        return -1;
    int cnt = fav_count();
    if (idx < 0 || idx >= cnt)
        return -1;
    fav_slot_t slot;
    esp_partition_read(s_part, (idx + 1) * FAV_SLOT, &slot, sizeof(slot));
    if (slot.magic != FAV_MAGIC || slot.len <= 1)
        return -1;
    slot.data[slot.len] = 0;

    /* 按 \x01 分割 (最多 8 段, 复制到静态缓冲) */
    int n = 0;
    char *p = (char *)slot.data;
    while (p && n < 8) {
        char *sep = strchr(p, '\x01');
        if (sep)
            *sep = 0;
        strncpy(s_fv_segs[n], p, sizeof(s_fv_segs[n]) - 1);
        s_fv_segs[n][sizeof(s_fv_segs[n]) - 1] = 0;
        n++;
        p = sep ? sep + 1 : NULL;
    }
    if (n < 7)
        return -1;

    strncpy(s_fv_subj, s_fv_segs[0], sizeof(s_fv_subj) - 1);
    s_fv_subj[sizeof(s_fv_subj) - 1] = 0;
    strncpy(s_fv_content, s_fv_segs[1], sizeof(s_fv_content) - 1);
    s_fv_content[sizeof(s_fv_content) - 1] = 0;
    for (int i = 0; i < 4; i++) {
        strncpy(s_fv_opts[i], (n > 2 + i) ? s_fv_segs[2 + i] : "",
                sizeof(s_fv_opts[i]) - 1);
        s_fv_opts[i][sizeof(s_fv_opts[i]) - 1] = 0;
    }
    int ans = atoi(s_fv_segs[6]);
    strncpy(s_fv_expl, (n > 7) ? s_fv_segs[7] : "", sizeof(s_fv_expl) - 1);
    s_fv_expl[sizeof(s_fv_expl) - 1] = 0;

    memset(&g_fav_q, 0, sizeof(g_fav_q));
    g_fav_q.subject = s_fv_subj;
    g_fav_q.content = s_fv_content;
    g_fav_q.options[0] = s_fv_opts[0];
    g_fav_q.options[1] = s_fv_opts[1];
    g_fav_q.options[2] = s_fv_opts[2];
    g_fav_q.options[3] = s_fv_opts[3];
    g_fav_q.answer_idx = (uint8_t)ans;
    g_fav_q.is_choice = 1;
    g_fav_q.explanation = s_fv_expl;
    return 0;
}

int fav_contains(const quiz_q_t *q)
{
    if (!s_part || !q || !q->content)
        return 0;
    for (int i = 0; i < fav_count(); i++) {
        fav_slot_t slot;
        esp_partition_read(s_part, (i + 1) * FAV_SLOT, &slot, sizeof(slot));
        if (slot.magic != FAV_MAGIC || slot.len <= 1)
            continue;
        slot.data[slot.len] = 0;
        char *c = strchr((char *)slot.data, '\x01');
        if (c && strcmp(c + 1, q->content) == 0)
            return 1;
    }
    return 0;
}
