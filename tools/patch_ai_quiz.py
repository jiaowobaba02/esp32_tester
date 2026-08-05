#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ai_quiz.c 修复补丁:
1. ai_begin/ai_end: 临界区防竞态 + 堆内存门槛
2. 新增 ai_post(): TLS 证书包优先, 硬编码链回退
3. 4 个 HTTP 调用点改用 ai_post (quiz/weak/kb/trans)
4. quiz 尝试次数 4->3
每处替换都断言唯一匹配, 失败即中止."""
import sys

path = '/media/user/6AF00DA6F00D7A19/esp32-tester/main/ai_quiz.c'
src = open(path, encoding='utf-8').read()
orig = src

def rep(old, new, label):
    global src
    n = src.count(old)
    if n != 1:
        print(f"[FAIL] {label}: 匹配 {n} 次 (期望 1), 中止")
        sys.exit(1)
    src = src.replace(old, new)
    print(f"[OK] {label}")

# ---------- R1: ai_begin/ai_end 竞态 + 堆门槛 ----------
rep("""/* AI 请求互斥: 一次只允许一个 (heap 有限, 并发两个 32KB 缓冲 + TLS 会耗尽内存) */
static volatile int s_ai_busy = 0;

static int ai_begin(void)
{
    if (s_ai_busy) {
        ai_set_err("AI 正在生成中，请稍候");
        return -1;
    }
    s_ai_busy = 1;
    return 0;
}

static void ai_end(void)
{
    s_ai_busy = 0;
}""",
"""/* AI 请求互斥: 一次只允许一个 (heap 有限, 并发两个 32KB 缓冲 + TLS 会耗尽内存).
 * 临界区防竞态: UI 任务与 httpd /aitest 任务可能同时进入 */
static volatile int s_ai_busy = 0;
static portMUX_TYPE s_ai_mux = portMUX_INITIALIZER_UNLOCKED;

/* TLS 全程所需内存: 响应缓冲 32KB + mbedtls 连接约 30KB + cJSON 解析 ~20KB.
 * 低于门槛直接失败 (不发起 TLS), 避免生成中途内存耗尽 */
#define AI_MIN_HEAP (90 * 1024)

static int ai_begin(void)
{
    portENTER_CRITICAL(&s_ai_mux);
    if (s_ai_busy) {
        portEXIT_CRITICAL(&s_ai_mux);
        ai_set_err("AI 正在生成中，请稍候");
        return -1;
    }
    s_ai_busy = 1;
    portEXIT_CRITICAL(&s_ai_mux);

    if (esp_get_free_heap_size() < AI_MIN_HEAP) {
        ESP_LOGE(TAG, "ai begin: heap too low (%lu)",
                 (unsigned long)esp_get_free_heap_size());
        ai_set_err("内存不足，请重启设备");
        s_ai_busy = 0;
        return -1;
    }
    return 0;
}

static void ai_end(void)
{
    s_ai_busy = 0;
}""", "R1 ai_begin/ai_end")

# ---------- R2: 插入 ai_post() ----------
rep("""    } else if (evt->event_id == HTTP_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected (len=%d)", r ? r->len : -1);
    }
    return ESP_OK;
}

/* 清洗并转义文本为 JSON 字符串内容 (错题分析用):""",
"""    } else if (evt->event_id == HTTP_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected (len=%d)", r ? r->len : -1);
    }
    return ESP_OK;
}

/* 执行一次 DeepSeek POST. TLS 信任策略:
 *  1. esp_crt_bundle (系统证书包) — 备份版实测稳定的路径
 *  2. 验证失败 -> 回退硬编码链 s_ds_cert (中间+根, 覆盖服务器不下发中间证书的节点)
 * 返回 perform 结果; 两种信任方式都失败返回 ESP_FAIL.
 * resp->body_timeout 由调用方预设 (header 到达后放宽读超时). */
static esp_err_t ai_post(ai_resp_t *resp, const char *body, int timeout_ms)
{
    for (int mode = 0; mode < 2; mode++) {
        esp_http_client_config_t cfg = {
            .url = "https://api.deepseek.com/chat/completions",
            .method = HTTP_METHOD_POST,
            .event_handler = ai_http_handler,
            .user_data = resp,
            .timeout_ms = timeout_ms,   /* 连接/响应头超时 */
            /* TCP keepalive: 服务器生成期间连接空闲, 移动 CGNAT 约 15 秒会清掉
             * 空闲映射导致断连; 空闲 5 秒发探测包刷新映射 */
            .keep_alive_enable = true,
            .keep_alive_idle = 5,
            .keep_alive_interval = 5,
            .keep_alive_count = 3,
            .alpn_protos = s_alpn,
            .user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/126.0 Safari/537.36",
        };
        if (mode == 1)
            cfg.cert_pem = s_ds_cert;   /* 回退: 硬编码中间+根链 */
        else
            cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "http init fail mode=%d (heap=%lu)", mode,
                     (unsigned long)esp_get_free_heap_size());
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        /* 强制明文响应: DeepSeek 对 >16KB 响应自动 gzip, 而我们不解压 */
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_post_field(client, body, (int)strlen(body));

        resp->len = 0;
        resp->overflow = 0;
        esp_err_t err = esp_http_client_perform(client);
        esp_http_client_cleanup(client);
        if (err == ESP_OK)
            return ESP_OK;
        ESP_LOGW(TAG, "perform mode=%d (0=bundle 1=pinned) failed: %s (len=%d heap=%lu)",
                 mode, esp_err_to_name(err), resp->len,
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return ESP_FAIL;
}

/* 清洗并转义文本为 JSON 字符串内容 (错题分析用):""",
"R2 插入 ai_post")

# ---------- R3: quiz cfg 删除 + 4->3 次 ----------
rep("""    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .user_data = &resp,
        .timeout_ms = 20000,       /* 连接/响应头超时: 网络不通时快速失败重试 */
        /* TCP keepalive: 服务器生成题目期间连接空闲, 移动 CGNAT 约 15 秒
         * 会清掉空闲映射导致断连; 空闲 5 秒发探测包刷新映射 */
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .alpn_protos = s_alpn,
        .user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/126.0 Safari/537.36",
        .cert_pem = s_ds_cert,   /* 信任 DeepSeek 专属证书链 */
    };

    /* 最多 4 次尝试: 网络抖动/重复/解析失败时换随机知识点重出 */
    for (int attempt = 0; attempt < 4; attempt++) {""",
"""    /* 最多 3 次尝试: 网络抖动/重复/解析失败时换随机知识点重出 */
    for (int attempt = 0; attempt < 3; attempt++) {""",
"R3 quiz cfg 删除 + 3 次")

# ---------- R4: quiz perform 块 ----------
rep("""        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "quiz init fail (heap=%lu)", (unsigned long)esp_get_free_heap_size());
            ai_set_err("内存不足");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;                    /* 换知识点重试 */
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        /* 强制明文响应: DeepSeek 对 >16KB 响应自动 gzip, 而我们不解压 */
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_post_field(client, body, (int)strlen(body));

        resp.len = 0;
        resp.overflow = 0;
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            int st = esp_http_client_get_status_code(client);
            ESP_LOGE(TAG, "quiz http failed: %s status=%d", esp_err_to_name(err), st);
        }
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            /* 网络瞬时故障 (如 TLS 后连接被 RST) 常见: 重试, 不直接判失败 */
            ai_set_err("网络连接失败，正在重试");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }""",
"""        esp_err_t err = ai_post(&resp, body, 20000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "quiz post failed: %s (heap=%lu)", esp_err_to_name(err),
                     (unsigned long)esp_get_free_heap_size());
            /* 网络瞬时故障 (如 TLS 后连接被 RST) 常见: 重试, 不直接判失败 */
            ai_set_err("网络连接失败，正在重试");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }""",
"R4 quiz perform")

# ---------- R5: weak cfg 删除 ----------
rep("""    resp.cap = RESP_SIZE;
    resp.body_timeout = 90000;
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .user_data = &resp,
        .timeout_ms = 90000,
        .keep_alive_enable = true,   /* 防移动 CGNAT 空闲断连 */
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .alpn_protos = s_alpn,
        .cert_pem = s_ds_cert,   /* 信任 DeepSeek 专属证书链 */
    };

    /* 失败自动重试 (最多 2 次) */""",
"""    resp.cap = RESP_SIZE;
    resp.body_timeout = 90000;

    /* 失败自动重试 (最多 2 次) */""",
"R5 weak cfg 删除")

# ---------- R6: weak perform 块 ----------
rep("""        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "weak init fail (heap=%lu)", (unsigned long)esp_get_free_heap_size());
            ai_set_err("HTTP 客户端创建失败");
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        resp.len = 0;
        resp.overflow = 0;
        esp_err_t err = esp_http_client_perform(client);
        int st = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
        ESP_LOGI(TAG, "weak perform[%d]: err=%s status=%d len=%d", attempt,
                 esp_err_to_name(err), st, resp.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "weak http failed: %s status=%d", esp_err_to_name(err), st);
        }
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            ai_set_err(esp_err_to_name(err));
            if (attempt < 1) {
                ESP_LOGW(TAG, "weak http fail, retry");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }""",
"""        esp_err_t err = ai_post(&resp, body, 90000);
        ESP_LOGI(TAG, "weak perform[%d]: err=%s len=%d", attempt,
                 esp_err_to_name(err), resp.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "weak http failed: %s", esp_err_to_name(err));
            ai_set_err(esp_err_to_name(err));
            if (attempt < 1) {
                ESP_LOGW(TAG, "weak http fail, retry");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }""",
"R6 weak perform")

# ---------- R7: kb cfg 删除 ----------
rep("""    resp.cap = RESP_SIZE;
    resp.body_timeout = 300000;   /* 知识库 3000 字生成可达 1-3 分钟 */
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .user_data = &resp,
        .timeout_ms = 300000,        /* 3000 字输出约 1-3 分钟 */
        .keep_alive_enable = true,   /* 防移动 CGNAT 空闲断连 */
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .alpn_protos = s_alpn,
        .cert_pem = s_ds_cert,   /* 信任 DeepSeek 专属证书链 */
    };

    /* 失败自动重试 (最多 3 次): 网络瞬时错误 / 服务端偶发异常 */""",
"""    resp.cap = RESP_SIZE;
    resp.body_timeout = 300000;   /* 知识库 3000 字生成可达 1-3 分钟 */

    /* 失败自动重试 (最多 3 次): 网络瞬时错误 / 服务端偶发异常 */""",
"R7 kb cfg 删除")

# ---------- R8: kb perform 块 ----------
rep("""        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "kb init fail (heap=%lu)", (unsigned long)esp_get_free_heap_size());
            ai_set_err("HTTP 客户端创建失败");
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        ESP_LOGI(TAG, "kb keylen=%d", (int)strlen(s_api_key));
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        resp.len = 0;
        resp.overflow = 0;
        ESP_LOGI(TAG, "kb start[%d]: resp=%p cap=%d heap=%lu", attempt,
                 (void *)resp.buf, resp.cap,
                 (unsigned long)esp_get_free_heap_size());
        esp_err_t err = esp_http_client_perform(client);
        int st = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
        ESP_LOGI(TAG, "kb perform[%d]: err=%s status=%d len=%d overflow=%d",
                 attempt, esp_err_to_name(err), st, resp.len, resp.overflow);
        if (resp.len > 0) {
            resp.buf[resp.len] = 0;
            ESP_LOGI(TAG, "kb resp head: %.120s", resp.buf);
        }
        esp_http_client_cleanup(client);""",
"""        ESP_LOGI(TAG, "kb start[%d]: resp=%p cap=%d heap=%lu", attempt,
                 (void *)resp.buf, resp.cap,
                 (unsigned long)esp_get_free_heap_size());
        esp_err_t err = ai_post(&resp, body, 300000);
        ESP_LOGI(TAG, "kb perform[%d]: err=%s len=%d overflow=%d",
                 attempt, esp_err_to_name(err), resp.len, resp.overflow);
        if (resp.len > 0) {
            resp.buf[resp.len] = 0;
            ESP_LOGI(TAG, "kb resp head: %.120s", resp.buf);
        }""",
"R8 kb perform")

# ---------- R9: trans cfg 删除 ----------
rep("""    resp.cap = RESP_SIZE;
    resp.body_timeout = 30000;
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = ai_http_handler,
        .user_data = &resp,
        .timeout_ms = 30000,
        .keep_alive_enable = true,   /* 防移动 CGNAT 空闲断连 */
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .alpn_protos = s_alpn,
        .cert_pem = s_ds_cert,   /* 信任 DeepSeek 专属证书链 */
    };

    /* 失败自动重试 (最多 2 次) */""",
"""    resp.cap = RESP_SIZE;
    resp.body_timeout = 30000;

    /* 失败自动重试 (最多 2 次) */""",
"R9 trans cfg 删除")

# ---------- R10: trans perform 块 ----------
rep("""        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "trans init fail");
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        resp.len = 0;
        resp.overflow = 0;
        esp_err_t err = esp_http_client_perform(client);
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "trans http failed: %s", esp_err_to_name(err));
            if (attempt < 1) {
                ESP_LOGW(TAG, "trans http fail, retry");
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            continue;
        }""",
"""        esp_err_t err = ai_post(&resp, body, 30000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "trans http failed: %s", esp_err_to_name(err));
            if (attempt < 1) {
                ESP_LOGW(TAG, "trans http fail, retry");
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            continue;
        }""",
"R10 trans perform")

open(path, 'w', encoding='utf-8', newline='').write(src)
print(f"\n完成: {len(orig)} -> {len(src)} bytes")
