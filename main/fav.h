#ifndef FAV_H
#define FAV_H
#include "questions.h"

/* 当前查看的收藏题 (get_question(question_count+1) 返回) */
extern quiz_q_t g_fav_q;

void fav_init(void);
int  fav_count(void);
int  fav_add(const quiz_q_t *q);          /* 0=成功 */
int  fav_remove(int idx);                 /* 0=成功 */
int  fav_get(int idx);                    /* 填充 g_fav_q, 0=成功 */
int  fav_contains(const quiz_q_t *q);     /* 1=已收藏 (按 content 比较) */
void fav_debug(void);                     /* 调试: 打印分区头 */

#endif
