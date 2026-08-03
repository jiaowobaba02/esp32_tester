/**
 * ai_quiz.c — DeepSeek API 出题 (联网模式)
 *
 * 调用 https://api.deepseek.com/chat/completions 生成一道高中选择题,
 * 解析 AI 返回的 JSON (题目内容/选项/答案/解析) 存入 g_ai_q。
 *
 * 依赖: esp_http_client + esp_crt_bundle + cJSON (esp-idf 自带)
 * 需要: s_api_key (设置页配置, 存在 NVS)
 */
#include "ai_quiz.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"      /* esp_get_free_heap_size */
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ai";

quiz_q_t g_ai_q;

/* 响应缓冲: 请求期间动态分配 (24KB 常驻会挤占堆, 导致 http client init 失败) */
static char *s_resp = NULL;
static int s_resp_cap = 0;
static int s_resp_len;
#define RESP_SIZE 16384

static char *resp_alloc(void)
{
    if (!s_resp) {
        s_resp = malloc(RESP_SIZE);
        if (s_resp)
            s_resp_cap = RESP_SIZE;
    }
    return s_resp;
}

static void resp_free(void)
{
    if (s_resp) {
        free(s_resp);
        s_resp = NULL;
        s_resp_cap = 0;
    }
}

extern char s_api_key[65];
extern int s_grade;
static const char *grade_names[3] = { "高一", "高二", "高三" };

static esp_err_t ai_http_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_resp && s_resp_len + evt->data_len < s_resp_cap) {
            memcpy(s_resp + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* 从 JSON 字符串提取 "field":"value" (value 内转义已解码) */
static int json_get_str(const char *json, const char *field, char *out, int outsz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", field);
    const char *p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    int j = 0;
    while (*p && *p != '"' && j < outsz - 1) {
        if (*p == '\\' && p[1]) {
            /* 常见转义 */
            if (p[1] == 'n') { out[j++] = ' '; }
            else if (p[1] == 't') { out[j++] = ' '; }
            else out[j++] = p[1];
            p += 2;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = 0;
    return 0;
}

/* 在 content 里找 JSON 对象 (第一个 { 到最后一个 }) */
static char *extract_json(const char *content, char *buf, int bufsz)
{
    const char *s = strchr(content, '{');
    const char *e = strrchr(content, '}');
    if (!s || !e || e <= s)
        return NULL;
    int len = (int)(e - s + 1);
    if (len >= bufsz)
        len = bufsz - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    return buf;
}

int ai_generate_question(const char *subject)
{
    if (!s_api_key[0]) {
        ESP_LOGE(TAG, "no api key");
        return -1;
    }

    char body[1400];
    int g = (s_grade >= 0 && s_grade <= 2) ? s_grade : 2;
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。\"},"
        "{\"role\":\"user\",\"content\":\"请出一道高中%s%s选择题（难度适合%s学生），严格按以下 JSON 格式输出："
        "{\\\"content\\\":\\\"题目内容\\\",\\\"options\\\":{\\\"A\\\":\\\"选项A\\\",\\\"B\\\":\\\"选项B\\\","
        "\\\"C\\\":\\\"选项C\\\",\\\"D\\\":\\\"选项D\\\"},\\\"answer\\\":\\\"A\\\",\\\"explanation\\\":\\\"解析\\\"}\"}"
        "],\"max_tokens\":800,\"temperature\":0.8}",
        grade_names[g], subject, grade_names[g]);

    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    if (!resp_alloc()) {
        ESP_LOGE(TAG, "quiz no mem (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        return -1;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "quiz init fail (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        resp_free();
        return -1;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    s_resp_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        int st = esp_http_client_get_status_code(client);
        ESP_LOGE(TAG, "quiz http failed: %s status=%d", esp_err_to_name(err), st);
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        resp_free();
        return -1;
    }
    s_resp[s_resp_len] = 0;
    ESP_LOGI(TAG, "resp %d bytes", s_resp_len);

    /* 解析: choices[0].message.content */
    cJSON *root = cJSON_Parse(s_resp);
    if (!root) {
        ESP_LOGE(TAG, "json parse fail: %.200s", s_resp);
        resp_free();
        return -1;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *msg = choices && cJSON_GetArraySize(choices) > 0
                     ? cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message")
                     : NULL;
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    if (!content || !cJSON_IsString(content)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "no content in response");
        resp_free();
        return -1;
    }
    const char *cstr = cJSON_GetStringValue(content);

    /* content 里提取题目 JSON */
    static char qjson[2048];
    if (!extract_json(cstr, qjson, sizeof(qjson))) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "no json in content");
        resp_free();
        return -1;
    }
    cJSON_Delete(root);

    cJSON *q = cJSON_Parse(qjson);
    if (!q) {
        ESP_LOGE(TAG, "question json parse fail: %.200s", qjson);
        resp_free();
        return -1;
    }
    cJSON *jc = cJSON_GetObjectItem(q, "content");
    cJSON *jo = cJSON_GetObjectItem(q, "options");
    cJSON *ja = cJSON_GetObjectItem(q, "answer");
    cJSON *je = cJSON_GetObjectItem(q, "explanation");
    if (!jc || !cJSON_IsString(jc) || !jo || !ja || !cJSON_IsString(ja)) {
        cJSON_Delete(q);
        ESP_LOGE(TAG, "question json missing fields");
        resp_free();
        return -1;
    }

    static char content_buf[1024];
    snprintf(content_buf, sizeof(content_buf), "%s", cJSON_GetStringValue(jc));
    memset(&g_ai_q, 0, sizeof(g_ai_q));
    g_ai_q.subject = subject;   /* 指向调用方字符串 (静态) */
    g_ai_q.content = content_buf;
    static char opts_buf[4][256];
    for (int i = 0; i < 4; i++) {
        cJSON *o = cJSON_GetObjectItem(jo, (char[]){'A' + i, 0});
        if (o && cJSON_IsString(o)) {
            snprintf(opts_buf[i], sizeof(opts_buf[i]), "%s", cJSON_GetStringValue(o));
        } else {
            opts_buf[i][0] = 0;
        }
        g_ai_q.options[i] = opts_buf[i];
    }
    const char *ans = cJSON_GetStringValue(ja);
    g_ai_q.answer_idx = (ans && ans[0] >= 'A' && ans[0] <= 'D') ? ans[0] - 'A' : 0;
    g_ai_q.is_choice = 1;
    if (je && cJSON_IsString(je)) {
        static char expl_buf[1024];
        snprintf(expl_buf, sizeof(expl_buf), "%s", cJSON_GetStringValue(je));
        g_ai_q.explanation = expl_buf;
    } else {
        g_ai_q.explanation = "";
    }
    cJSON_Delete(q);
    ESP_LOGI(TAG, "AI CONTENT[%d]: %s", (int)strlen(g_ai_q.content), g_ai_q.content);
    resp_free();
    return 0;
}

/* ---------- AI 薄弱点分析 ---------- */
static char s_weak_out[4096];   /* 逐题分析输出更长, 原 1600 会截断 */

const char *ai_analyze_weakness(const char *subject, const char *topics)
{
    s_weak_out[0] = 0;
    if (!s_api_key[0])
        return s_weak_out;

    /* topics 需 JSON 转义 (错题内容可能含引号/反斜杠) */
    static char esc_t[1200];
    int j = 0;
    for (const char *p = topics; *p && j < (int)sizeof(esc_t) - 6; p++) {
        if (*p == '"' || *p == '\\')
            esc_t[j++] = '\\';
        esc_t[j++] = *p;
    }
    esc_t[j] = 0;

    char body[2200];
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是高中学习辅导老师。\"},"
        "{\"role\":\"user\",\"content\":\"以下是高中%s的错题（每题已编号，如 1.xxx；2.xxx）：%s。"
        "请逐题简要分析：每道题用『题N』开头单独一段，指出该题考查的知识点、主要错因、一句改进建议，"
        "每题1-2行，不要JSON，不要总结性的废话。\"}"
        "],\"max_tokens\":800,\"temperature\":0.5}",
        subject, esc_t);

    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    if (!resp_alloc()) {
        ESP_LOGE(TAG, "weak no mem (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        return s_weak_out;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "weak init fail (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        resp_free();
        return s_weak_out;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    s_resp_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        int st = esp_http_client_get_status_code(client);
        ESP_LOGE(TAG, "weak http failed: %s status=%d", esp_err_to_name(err), st);
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        resp_free();
        return s_weak_out;
    }
    s_resp[s_resp_len] = 0;

    cJSON *root = cJSON_Parse(s_resp);
    if (!root) {
        ESP_LOGE(TAG, "weak resp parse fail: %.200s", s_resp);
        resp_free();
        return s_weak_out;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *msg = choices && cJSON_GetArraySize(choices) > 0
                     ? cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message")
                     : NULL;
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    if (!content || !cJSON_IsString(content)) {
        ESP_LOGE(TAG, "weak resp no content: %.200s", s_resp);
        cJSON_Delete(root);
        resp_free();
        return s_weak_out;
    }
    {
        const char *cstr = cJSON_GetStringValue(content);
        /* 去掉可能的前后空白/引号 */
        while (*cstr == ' ' || *cstr == '\n' || *cstr == '"')
            cstr++;
        int len = (int)strlen(cstr);
        while (len > 0 && (cstr[len-1] == ' ' || cstr[len-1] == '\n' || cstr[len-1] == '"'))
            len--;
        if (len >= (int)sizeof(s_weak_out))
            len = sizeof(s_weak_out) - 1;
        memcpy(s_weak_out, cstr, len);
        s_weak_out[len] = 0;
    }
    cJSON_Delete(root);
    resp_free();
    ESP_LOGI(TAG, "weak analysis: %.60s", s_weak_out);
    return s_weak_out;
}

/* ---------- AI 知识库: 生成某科某主题的核心知识点 ---------- */
static char s_kb_out[4096];

const char *ai_get_knowledge(const char *subject, const char *topic)
{
    s_kb_out[0] = 0;
    if (!s_api_key[0])
        return s_kb_out;

    char body[1500];
    int g = (s_grade >= 0 && s_grade <= 2) ? s_grade : 2;
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是高中辅导老师，知识面广，表达简洁有条理。\"},"
        "{\"role\":\"user\",\"content\":\"请整理高中%s《%s》中「%s」的核心知识点："
        "分点列出（每点一行，用数字编号），包括：重要概念定义、关键公式/原理、常见易错点、典型例题提示。"
        "尽量全面细致，控制在900字以内，直接输出文字，不要JSON。\"}"
        "],\"max_tokens\":2000,\"temperature\":0.4}",
        grade_names[g], subject, topic);

    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    if (!resp_alloc()) {
        ESP_LOGE(TAG, "kb no mem (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        return s_kb_out;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "kb init fail (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        resp_free();
        return s_kb_out;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    ESP_LOGI(TAG, "kb keylen=%d", (int)strlen(s_api_key));
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    s_resp_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        int st = esp_http_client_get_status_code(client);
        s_resp[s_resp_len] = 0;
        ESP_LOGE(TAG, "kb http failed: %s status=%d errbody=%.300s",
                 esp_err_to_name(err), st, s_resp);
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        resp_free();
        return s_kb_out;
    }
    s_resp[s_resp_len] = 0;

    cJSON *root = cJSON_Parse(s_resp);
    if (!root) {
        ESP_LOGE(TAG, "kb resp parse fail: %.200s", s_resp);
        resp_free();
        return s_kb_out;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *msg = choices && cJSON_GetArraySize(choices) > 0
                     ? cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message")
                     : NULL;
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    if (!content || !cJSON_IsString(content)) {
        ESP_LOGE(TAG, "kb resp no content: %.200s", s_resp);
        cJSON_Delete(root);
        resp_free();
        return s_kb_out;
    }
    {
        const char *cstr = cJSON_GetStringValue(content);
        while (*cstr == ' ' || *cstr == '\n' || *cstr == '"')
            cstr++;
        int len = (int)strlen(cstr);
        while (len > 0 && (cstr[len-1] == ' ' || cstr[len-1] == '\n' || cstr[len-1] == '"'))
            len--;
        if (len >= (int)sizeof(s_kb_out))
            len = sizeof(s_kb_out) - 1;
        memcpy(s_kb_out, cstr, len);
        s_kb_out[len] = 0;
    }
    cJSON_Delete(root);
    resp_free();
    ESP_LOGI(TAG, "kb generated: %d bytes", (int)strlen(s_kb_out));
    return s_kb_out;
}

/* ---------- AI 拼音/英文主题名 → 中文主题名 ---------- */
static char s_trans_buf[64];

const char *ai_translate_topic(const char *text)
{
    s_trans_buf[0] = 0;
    if (!s_api_key[0])
        return s_trans_buf;

    char body[800];
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是主题名转换器。\"},"
        "{\"role\":\"user\",\"content\":\"把以下拼音或中英混合的主题名转换成规范的中文主题名"
        "（如PCR yuanli→PCR原理，dna fuzhi→DNA复制）。只输出转换后的中文主题名，"
        "不要任何解释、标点或多余文字。输入：%s\"}"
        "],\"max_tokens\":30,\"temperature\":0.1}",
        text);

    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    if (!resp_alloc())
        return s_trans_buf;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "trans init fail");
        resp_free();
        return s_trans_buf;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    s_resp_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trans http failed: %s", esp_err_to_name(err));
        resp_free();
        return s_trans_buf;
    }
    s_resp[s_resp_len] = 0;

    cJSON *root = cJSON_Parse(s_resp);
    if (!root) {
        resp_free();
        return s_trans_buf;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *msg = choices && cJSON_GetArraySize(choices) > 0
                     ? cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message")
                     : NULL;
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    if (content && cJSON_IsString(content)) {
        const char *cstr = cJSON_GetStringValue(content);
        while (*cstr == ' ' || *cstr == '\n' || *cstr == '"')
            cstr++;
        int len = (int)strlen(cstr);
        while (len > 0 && (cstr[len-1] == ' ' || cstr[len-1] == '\n' || cstr[len-1] == '"'))
            len--;
        if (len >= (int)sizeof(s_trans_buf))
            len = sizeof(s_trans_buf) - 1;
        memcpy(s_trans_buf, cstr, len);
        s_trans_buf[len] = 0;
    }
    cJSON_Delete(root);
    resp_free();
    ESP_LOGI(TAG, "trans: '%s' -> '%s'", text, s_trans_buf);
    return s_trans_buf;
}
