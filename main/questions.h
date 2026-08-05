#ifndef QUESTIONS_H
#define QUESTIONS_H
#include <stdint.h>
typedef struct {
    const char *subject;
    const char *content;
    const char *options[4];
    uint8_t is_choice;
    uint8_t answer_idx;
    const char *answer_text;
    const char *explanation;
    const char *topic;      /* 考点 (仅 AI 题; 静态题/收藏题为 NULL) */
} quiz_q_t;
extern const int question_count;
const quiz_q_t *get_question(int i);
#endif