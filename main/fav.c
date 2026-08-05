/**
 * fav.c — 题目收藏 (favorites 分区裸写, 1024B/槽位, 容量 = 分区大小/1KB)
 *
 * 槽位 0: [magic][count]
 * 槽位 N: [magic][len][data]  data 用 \x01 分隔字段:
 *         subject \x01 content \x01 optA \x01 optB \x01 optC \x01 optD
 *         \x01 answer_idx \x01 explanation
 */
#include "fav.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "fav";

/* 旧布局 (v4) favorites 分区绝对地址: 新布局迁至 0x330000 后,
 * 首次启动从旧位置迁移收藏 (旧区随后被 weakness v5 迁移擦除) */
#define FAV_OLD_OFF 0x2A0000

#define FAV_SLOT  1024
#define FAV_MAGIC 0x4656   /* "FV" */
#define FAV_DELETED 0x0000 /* 已删除标记 (flash 只能 1→0 写, 写 0 即删) */

typedef struct {
    uint16_t magic;
    uint16_t len;
    uint8_t data[FAV_SLOT - 4];
} __attribute__((packed)) fav_slot_t;

quiz_q_t g_fav_q;

static void fav_migrate_old(void);   /* 定义在下方 (旧分区收藏迁移) */

static const esp_partition_t *s_part;
static char s_fv_subj[33], s_fv_content[512], s_fv_opts[4][256], s_fv_expl[512];
static char s_fv_ans[64];        /* 填空题答案文本 (选择题为空) */
static char s_fv_segs[10][520];

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
        fav_migrate_old();                       /* 新分区就绪后迁移旧收藏 */
    }
}

/* 旧分区 (v4, 0x2A0000) 收藏迁移: 逐槽读旧写新 (旧区将被 weak 迁移擦除) */
static void fav_migrate_old(void)
{
    esp_flash_t *fl = esp_flash_default_chip;
    fav_slot_t slot;
    if (esp_flash_read(fl, &slot, FAV_OLD_OFF, 4) != ESP_OK) {
        ESP_LOGW(TAG, "old partition read failed");
        return;
    }
    if (slot.magic != FAV_MAGIC) {
        ESP_LOGI(TAG, "no old favorites (magic=0x%04X)", slot.magic);
        return;
    }
    int slots = (int)(s_part->size / FAV_SLOT);
    int moved = 0;
    for (int i = 1; i < slots; i++) {
        if (esp_flash_read(fl, &slot, FAV_OLD_OFF + (uint32_t)i * FAV_SLOT,
                           sizeof(slot)) != ESP_OK)
            break;
        if (slot.magic != FAV_MAGIC || slot.len <= 1 || slot.len >= FAV_SLOT - 4)
            continue;
        if (esp_partition_write(s_part, (uint32_t)i * FAV_SLOT, &slot,
                                sizeof(slot)) != ESP_OK)
            break;
        moved++;
    }
    ESP_LOGI(TAG, "migrated %d favorites from old partition", moved);
}

/* 槽位映射缓存: 扫描一次记录每个有效收藏的物理槽号, 避免每次读取全分区扫.
 * s_map_cnt = -1 表示缓存失效 (add/remove 后置 -1) */
static int s_slot_map[512];
static int s_map_cnt = -1;

static void fav_refresh(void)
{
    int n = 0;
    uint16_t magic;
    int slots = (int)(s_part->size / FAV_SLOT);   /* 按分区实际大小 (256KB=256 槽) */
    for (int i = 1; i < slots; i++) {
        esp_partition_read(s_part, (uint32_t)i * FAV_SLOT, &magic, 2);
        if (magic == FAV_MAGIC)
            s_slot_map[n++] = i;
    }
    s_slot_map[n] = -1;
    s_map_cnt = n;
}

/* count 不存储: 扫描全部槽位统计有效 magic (删除槽标记 0x0000 跳过) */
int fav_count(void)
{
    if (!s_part)
        return 0;
    if (s_map_cnt < 0)
        fav_refresh();
    return s_map_cnt;
}

/* 第 idx 个有效收藏的物理槽号 (0-based, 最新收藏在前); -1=不存在 */
static int fav_slot_of(int idx)
{
    if (!s_part || idx < 0)
        return -1;
    if (s_map_cnt < 0)
        fav_refresh();
    if (idx >= s_map_cnt)
        return -1;
    return s_slot_map[s_map_cnt - 1 - idx];   /* 最新在前 */
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
    if (!s_part || !q || !q->content)
        return 0;
    if (s_map_cnt < 0)
        fav_refresh();
    for (int i = 0; i < s_map_cnt; i++) {       /* 只扫有效槽 (缓存) */
        fav_slot_t slot;
        esp_partition_read(s_part, (uint32_t)s_slot_map[i] * FAV_SLOT, &slot, sizeof(slot));
        if (slot.magic != FAV_MAGIC || slot.len <= 1)
            continue;
        slot.data[slot.len] = 0;
        char *c = strchr((char *)slot.data, '\x01');
        if (c && strcmp(c + 1, q->content) == 0)
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
    if (cnt >= (int)(s_part->size / FAV_SLOT) - 1)
        return -1;
    char buf[FAV_SLOT - 4];
    ESP_LOGI(TAG, "add: subj=%s cnt=%d", q->subject ? q->subject : "?", cnt);
    /* v6 槽位: 追加 段8=is_choice 段9=answer_text, 支持填空/解答题收藏;
     * 旧 v5 槽位 (8 段) 读取时按选择题处理 (fav_get 兼容) */
    snprintf(buf, sizeof(buf),
             "%s\x01%s\x01%s\x01%s\x01%s\x01%s\x01%d\x01%s\x01%d\x01%s",
             q->subject ? q->subject : "",
             q->content ? q->content : "",
             q->options[0] ? q->options[0] : "",
             q->options[1] ? q->options[1] : "",
             q->options[2] ? q->options[2] : "",
             q->options[3] ? q->options[3] : "",
             q->answer_idx,
             q->explanation ? q->explanation : "",
             q->is_choice,
             q->answer_text ? q->answer_text : "");
    fav_slot_t slot;
    slot.magic = FAV_MAGIC;
    slot.len = (uint16_t)strlen(buf);
    memset(slot.data, 0, sizeof(slot.data));
    memcpy(slot.data, buf, slot.len);
    /* 找第一个从未写过的槽 (0xFFFF).
     * 删除槽 (0x0000) 复用需先擦除所在扇区 (会波及同扇区其他收藏), 直接跳过 */
    int slot_no = -1;
    uint16_t magic;
    int slots = (int)(s_part->size / FAV_SLOT);
    for (int i = 1; i < slots; i++) {
        esp_partition_read(s_part, (uint32_t)i * FAV_SLOT, &magic, 2);
        if (magic == 0xFFFF) {
            slot_no = i;
            break;
        }
    }
    if (slot_no < 0)
        return -1;
    esp_partition_write(s_part, (uint32_t)slot_no * FAV_SLOT, &slot, sizeof(slot));
    s_map_cnt = -1;             /* 缓存失效 */
    return 0;   /* count 由 fav_count() 扫描得出 */
}

int fav_remove(int idx)
{
    if (!s_part)
        return -1;
    int phys = fav_slot_of(idx);
    if (phys < 0)
        return -1;
    /* flash 只能 1→0 写, 搬移/清 0xFF 都会失败 (旧数据按位与残留);
     * 把 magic 写成 0x0000 标记删除即可, 数据残留无害 */
    uint16_t z = FAV_DELETED;
    int r = esp_partition_write(s_part, (uint32_t)phys * FAV_SLOT, &z, 2);
    s_map_cnt = -1;             /* 缓存失效 */
    return r == ESP_OK ? 0 : -1;
}

int fav_get(int idx)
{
    if (!s_part)
        return -1;
    int phys = fav_slot_of(idx);
    if (phys < 0)
        return -1;
    fav_slot_t slot;
    esp_partition_read(s_part, (uint32_t)phys * FAV_SLOT, &slot, sizeof(slot));
    if (slot.magic != FAV_MAGIC || slot.len <= 1)
        return -1;
    slot.data[slot.len] = 0;

    /* 按 \x01 分割 (最多 10 段, 复制到静态缓冲) */
    int n = 0;
    char *p = (char *)slot.data;
    while (p && n < 10) {
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
    /* v6 槽位: 段8=is_choice 段9=answer_text; 旧 v5 槽位 (8 段) 视为选择题 */
    int is_choice = (n > 8) ? atoi(s_fv_segs[8]) : 1;
    if (is_choice < 0 || is_choice > 2)
        is_choice = 1;
    strncpy(s_fv_ans, (n > 9) ? s_fv_segs[9] : "", sizeof(s_fv_ans) - 1);
    s_fv_ans[sizeof(s_fv_ans) - 1] = 0;

    memset(&g_fav_q, 0, sizeof(g_fav_q));
    g_fav_q.subject = s_fv_subj;
    g_fav_q.content = s_fv_content;
    g_fav_q.options[0] = s_fv_opts[0];
    g_fav_q.options[1] = s_fv_opts[1];
    g_fav_q.options[2] = s_fv_opts[2];
    g_fav_q.options[3] = s_fv_opts[3];
    g_fav_q.answer_idx = (uint8_t)ans;
    g_fav_q.is_choice = (uint8_t)is_choice;
    g_fav_q.answer_text = s_fv_ans;
    g_fav_q.explanation = s_fv_expl;
    return 0;
}

int fav_contains(const quiz_q_t *q)
{
    if (!s_part || !q || !q->content)
        return 0;
    if (s_map_cnt < 0)
        fav_refresh();
    for (int i = 0; i < s_map_cnt; i++) {       /* 只扫有效槽 (缓存) */
        fav_slot_t slot;
        esp_partition_read(s_part, (uint32_t)s_slot_map[i] * FAV_SLOT, &slot, sizeof(slot));
        if (slot.magic != FAV_MAGIC || slot.len <= 1)
            continue;
        slot.data[slot.len] = 0;
        char *c = strchr((char *)slot.data, '\x01');
        if (c && strcmp(c + 1, q->content) == 0)
            return 1;
    }
    return 0;
}
