#ifndef AI_QUIZ_H
#define AI_QUIZ_H
#include "questions.h"

/* 知识库文本上限: 槽内容区 16320B (16384-64 主题名区) 内取整 (ai_quiz.c/main.c 共用) */
#define KB_OUT_MAX 16300   /* ≈5433 字 (UTF-8 3 字节/字) + \0; 实际提示词限 3000 字 */

/* AI 生成的题目 (全局, 答题时通过 get_question(question_count) 获取) */
extern quiz_q_t g_ai_q;

/* 调用 DeepSeek API 生成一道高中 subject 的题: 按科目概率出 选择题 或
 * 键盘填空题 (is_choice=2, 答案限 ASCII 数字/英文, 由 s_fill_prob 控制).
 * 出题难度由 main.c 的 s_diff 控制 (0=基础 1=中等 2=较难).
 * 知识点选题按薄弱统计加权 (quiz_* 接口), 强化薄弱板块.
 * 返回 0=成功 (g_ai_q 填充, 含 topic 考点), -1=失败 (未联网/无 Key/API 错误) */
int ai_generate_question(const char *subject);

/* 薄弱板块统计: 记录一次 AI 题作答 (correct=1 答对 / 0 答错).
 * 答对答错都记, 用于计算各知识点掌握度 (NVS 持久化). topic 未知则忽略 */
void quiz_record_answer(int subject_idx, const char *topic, int correct);

/* 该科当前最薄弱的板块名 (需强化的), 无则返回 "" */
const char *quiz_weak_topic(int subject_idx);

/* 该知识点是否处于薄弱强化区 (UI 红色标记用) */
int quiz_topic_weak(int subject_idx, const char *topic);

/* AI 薄弱点分析: 根据错题 topics 返回简要分析文本 (静态缓冲) */
const char *ai_analyze_weakness(const char *subject, const char *topics);

/* AI 知识库: 生成 subject 科目 topic 主题的核心知识点 (静态缓冲, 失败返回 "") */
const char *ai_get_knowledge(const char *subject, const char *topic);

/* AI 拼音/英文主题名 → 中文主题名 (subject 用于结合学科术语, 静态缓冲, 失败返回 "") */
const char *ai_translate_topic(const char *subject, const char *text);

/* 最近一次 AI 操作失败原因 (UI 提示用, 成功时可能为旧值; 带 E 码) */
const char *ai_last_error(void);
int ai_last_code(void);   /* 失败错误码: 1无Key 2无WiFi 3忙 4内存 5网络 6空响应
                            * 7内容非JSON 8字段缺失 9填空校验 10重复 11截断 12全败 */

#endif
