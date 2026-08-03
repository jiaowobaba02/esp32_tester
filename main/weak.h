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

#endif
