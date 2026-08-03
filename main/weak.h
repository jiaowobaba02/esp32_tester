#ifndef WEAK_H
#define WEAK_H

/* 薄弱点: 9 科 × 32KB 槽位 (weakness 分区)
 * 每科: 错题记录 (≤20 题) + AI 简要分析文本 */

void weak_init(void);
int  weak_count(int subject_idx);              /* 该科错题数 */
int  weak_add_wrong(int subject_idx, const char *content);  /* 0=成功 */
int  weak_get_wrong(int subject_idx, int idx, char *buf, int bufsz); /* 错题内容 */
const char *weak_get_ai(int subject_idx);      /* AI 说明 (无则 "") */
void weak_set_ai(int subject_idx, const char *text);
/* 知识库: 每科 6 主题槽, 槽内 [名称(64B)+内容]; 空槽返回 "" */
#define WEAK_KB_CNT 6
const char *weak_get_kb_name(int subject_idx, int slot);
const char *weak_get_kb(int subject_idx, int slot);
void weak_set_kb(int subject_idx, int slot, const char *name, const char *text);
void weak_clear_kb(int subject_idx, int slot);

#endif
