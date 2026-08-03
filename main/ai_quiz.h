#ifndef AI_QUIZ_H
#define AI_QUIZ_H
#include "questions.h"

/* AI 生成的题目 (全局, 答题时通过 get_question(question_count) 获取) */
extern quiz_q_t g_ai_q;

/* 调用 DeepSeek API 生成一道高中 subject 选择题.
 * 返回 0=成功 (g_ai_q 填充), -1=失败 (未联网/无 Key/API 错误) */
int ai_generate_question(const char *subject);

/* AI 薄弱点分析: 根据错题 topics 返回简要分析文本 (静态缓冲) */
const char *ai_analyze_weakness(const char *subject, const char *topics);

#endif
