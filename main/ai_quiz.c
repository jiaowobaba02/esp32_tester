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
#include "ds_cert.h"        /* 硬编码 DeepSeek 证书链 (信任锚, 替代 bundle) */
#include "esp_log.h"
#include "esp_system.h"      /* esp_get_free_heap_size */
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>            /* log2: 薄弱板块对数权重 */

static const char *TAG = "ai";

quiz_q_t g_ai_q;

/* ALPN: 标准客户端都协商 http/1.1, 无 ALPN 的 TLS 连接易被网关区别对待 */
static const char *s_alpn[] = { "\x08http/1.1", NULL };

/* 响应缓冲: 每请求独立分配, 经 esp_http_client user_data 传给事件回调.
 * 原全局单缓冲在 UI(main 任务) 与 /aitest(httpd 任务) 并发时会互相破坏.
 * 流式下只累积 content 纯文本: 知识库限 3000 字 ≈ 9-10KB (上限 16300 与
 * KB_OUT_MAX 对齐). 16KB 即可, 把内存留给 TLS 握手 (实测 32KB buf 时
 * 握手期 heap 仅剩 ~40KB, RSA 签名验证分配失败). */
#define RESP_SIZE 16384

typedef struct {
    char *buf;          /* 累积的 content 文本 (SSE delta 拼接, 非原始 JSON) */
    int cap;
    int len;
    int overflow;   /* 缓冲满: 数据被丢弃 (静默截断) */
    int body_timeout;   /* 收到响应头后读 body 的超时 (毫秒, 调用方设置) */
    int finish_length;  /* 流中 finish_reason=="length" (max_tokens 截断) */
    int linelen;        /* SSE 单行重组进度 (per-request, 防跨请求残留污染) */
} ai_resp_t;

static void ai_set_err(const char *e);   /* 定义在下方 */
static void ai_fail(int code, const char *e);   /* 定义在下方 */

/* AI 请求互斥: 一次只允许一个 (heap 有限, 并发两个 32KB 缓冲 + TLS 会耗尽内存).
 * 临界区防竞态: UI 任务与 httpd /aitest 任务可能同时进入 */
static volatile int s_ai_busy = 0;
static portMUX_TYPE s_ai_mux = portMUX_INITIALIZER_UNLOCKED;

/* TLS 全程所需内存: 峰值 = 响应缓冲 16KB + mbedtls 握手 ~26KB = 42KB
 * (IN 缓冲已调 8KB + ASYMMETRIC_CONTENT_LEN; cJSON 解析在 TLS 释放后).
 * 实测设备稳态空闲堆 ~78KB, 门槛 60KB: 低于则直接失败, 避免生成中途 OOM */
#define AI_MIN_HEAP (60 * 1024)

static int ai_begin(void)
{
    portENTER_CRITICAL(&s_ai_mux);
    if (s_ai_busy) {
        portEXIT_CRITICAL(&s_ai_mux);
        ai_fail(3, "AI 正在生成中，请稍候");
        return -1;
    }
    s_ai_busy = 1;
    portEXIT_CRITICAL(&s_ai_mux);

    if (esp_get_free_heap_size() < AI_MIN_HEAP) {
        ESP_LOGE(TAG, "ai begin: heap too low (%lu)",
                 (unsigned long)esp_get_free_heap_size());
        ai_fail(4, "内存不足，请重启设备");
        s_ai_busy = 0;
        return -1;
    }
    return 0;
}

static void ai_end(void)
{
    s_ai_busy = 0;
}

extern char s_api_key[65];
extern int s_grade;
extern int s_wifi_state;   /* main.c: 0=未连接 1=连接中 2=已连接 */
static const char *grade_names[3] = { "高一", "高二", "高三" };

static char s_sse_line[4096];   /* SSE 单行重组缓冲 (s_ai_busy 保证单请求, 静态安全) */

/* 从一行 SSE JSON 中提取 delta.content 值, 手工解码一层转义 (不分配内存).
 * 行是完整的 (行重组逐字节累积到 \n), 行内转义序列完整.
 * 解码一层: \\\\ -> \\, \\" -> " (SSE 对题目 JSON 的再转义);
 * \\n \\uXXXX 等保留原样 (题目 JSON 本身的合法转义, 交给 cJSON 解码).
 * 拼接出的 resp.buf 即题目 JSON 原文, extract_json 的 cJSON 可直接解析.
 * 注意: 不能原样保留 (键名会变成 \\"content\\" 导致 cJSON 字段查找失败),
 * 也不能全解码 (\\n 变空格 / \\u 变 uXXXX 会破坏内容). */
static int sse_content_raw(const char *json, char *out, int outsz)
{
    /* delta.content 键: 兼容 "content":" 与 "content": " (冒号后空格).
     * 题目 JSON 分片里的 \\"content\\" 带反斜杠, 不会误匹配 */
    const char *k = strstr(json, "\"content\":");
    if (!k)
        return 0;
    k += 10;                        /* 跳过 "content": */
    while (*k == ' ' || *k == '\t')
        k++;
    if (*k != '"')
        return 0;                   /* content 为 null/数字: 跳过 */
    k++;
    int j = 0;
    while (*k && *k != '"' && j < outsz - 3) {
        if (*k == '\\' && k[1]) {
            if (k[1] == '\\' || k[1] == '"') {
                out[j++] = k[1];    /* 解码一层: \\\\ -> \\, \\" -> " */
            } else {
                out[j++] = '\\';    /* \\n \\uXXXX 等保留 */
                out[j++] = k[1];
            }
            k += 2;
        } else {
            out[j++] = *k++;
        }
    }
    out[j] = 0;
    return j > 0;
}

static esp_err_t ai_http_handler(esp_http_client_event_t *evt)
{
    ai_resp_t *r = (ai_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r && r->buf) {
        /* 流式 SSE 解析: 行重组 → "data: {json}" → 提取 delta.content 追加.
         * 服务器边生成边发, 连接无空闲期, 规避移动 CGNAT ~15s 清映射断链 */
        int linelen = r->linelen;   /* per-request 行进度 (跨请求零残留) */
        if (r->len == 0 && evt->data_len > 0)
            ESP_LOGI(TAG, "body head: %.200s", (const char *)evt->data);  /* 诊断 */
        for (int i = 0; i < evt->data_len; i++) {
            char c = ((const char *)evt->data)[i];
            if (c == '\n') {
                s_sse_line[linelen] = 0;
                if (strncmp(s_sse_line, "data:", 5) == 0) {
                    const char *p = s_sse_line + 5;
                    while (*p == ' ') p++;
                    if (*p == '{') {
                        /* 行重组保证行完整 (跨 ON_DATA 分片逐字节累积),
                         * 行内转义序列必然完整: 手工提取 content 值并
                         * 保留转义原样 (\\\" -> \\\", \\\\uXXXX 原样),
                         * 拼接出的 resp.buf 是"一层转义"的题目 JSON,
                         * 最后由 extract_json 的 cJSON 统一解码.
                         * 不用每行 cJSON_Parse: 大量小块分配/释放会把
                         * 堆打碎, 第二次请求的 16KB 缓冲分配失败 ->
                         * "一次启动只能生成一道题" */
                        char tmp[1024];
                        if (sse_content_raw(p, tmp, sizeof(tmp)) && tmp[0]) {
                            int tl = (int)strlen(tmp);
                            if (r->len + tl < r->cap) {
                                memcpy(r->buf + r->len, tmp, (size_t)tl);
                                r->len += tl;
                            } else {
                                r->overflow = 1;
                            }
                        }
                        if (strstr(p, "\"finish_reason\":\"length\""))
                            r->finish_length = 1;
                    }
                }
                linelen = 0;
            } else if (linelen < (int)sizeof(s_sse_line) - 1) {
                s_sse_line[linelen++] = c;
            }
        }
        r->linelen = linelen;   /* 保存行进度 (跨 ON_DATA 分片累积) */
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        /* 两阶段超时: 响应头已到说明服务器在接受/生成,
         * 读 body 放宽到各请求自己的 body_timeout (慢生成也能等到) */
        if (r && r->body_timeout > 0)
            esp_http_client_set_timeout_ms(evt->client, r->body_timeout);
        ESP_LOGI(TAG, "http status=%d", esp_http_client_get_status_code(evt->client));  /* 诊断 */
    } else if (evt->event_id == HTTP_EVENT_DISCONNECTED) {
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
        resp->finish_length = 0;
        resp->linelen = 0;
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

/* 清洗并转义文本为 JSON 字符串内容 (错题分析用):
 * - 非法/孤立 UTF-8 字节序列 → 空格 (旧数据可能含半个汉字)
 * - 控制字符 (换行/回车/制表等) → 空格 (JSON 字符串不允许裸控制字符)
 * - " 和 \ → 反斜杠转义 */
static void esc_for_json(const char *src, char *dst, int dstsz)
{
    int j = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && j < dstsz - 8) {
        if (*p == '"' || *p == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)*p++;
        } else if (*p < 0x20) {
            dst[j++] = ' ';
            p++;
        } else if (*p < 0x80) {
            dst[j++] = (char)*p++;
        } else {
            /* 多字节 UTF-8: 校验后续字节完整性 */
            int len;
            if ((*p & 0xE0) == 0xC0) len = 2;
            else if ((*p & 0xF0) == 0xE0) len = 3;
            else if ((*p & 0xF8) == 0xF0) len = 4;
            else { dst[j++] = ' '; p++; continue; }   /* 孤立续字节 */
            int ok = 1;
            for (int k = 1; k < len; k++)
                if ((p[k] & 0xC0) != 0x80) { ok = 0; break; }
            if (!ok || j + len >= dstsz - 1) { dst[j++] = ' '; p++; continue; }
            for (int k = 0; k < len; k++)
                dst[j++] = (char)*p++;
        }
    }
    dst[j] = 0;
}

/* 截断到 dstsz-1 字节且不切断 UTF-8 字符 (避免半个汉字).
 * ⚠️ 回退必须只在"真的截断"时执行: 内容完整时, 若以 3 字节中文/全角标点
 * 结尾 (尾字节 0x80-0xBF 是续字节), 无条件回退会把整个最后字符吃掉
 * -> "中文题干/选项最后一个字丢失" (英语以 ASCII 结尾不受影响) */
static void utf8_truncate(const char *src, char *dst, int dstsz)
{
    int len = (int)strlen(src);
    if (len >= dstsz - 1) {
        len = dstsz - 1;
        while (len > 0 && ((uint8_t)src[len - 1] & 0xC0) == 0x80)
            len--;                      /* 回退续字节 */
        if (len > 0 && (uint8_t)src[len - 1] >= 0xC0)
            len--;                      /* 回退被截断的首字节 */
    }
    memcpy(dst, src, (size_t)len);
    dst[len] = 0;
}

/* ================= 出题多样化: 随机知识点 + 防重复 + 难度 ================= */

/* 各科目知识点池 (每科 22 个, 出题时随机抽, 避免 AI 总出同几道经典题) */
static const char *const s_topic_pool[9][22] = {
    { "集合与函数", "函数性质与图像", "基本初等函数", "函数零点", "三角函数",
      "三角恒等变换", "解三角形", "数列通项与求和", "不等式", "线性规划",
      "平面向量", "立体几何初步", "空间向量与坐标", "直线与圆", "椭圆与双曲线",
      "抛物线", "参数方程与极坐标", "导数与单调性", "导数与极值最值", "概率",
      "统计与分布列", "排列组合与二项式" },
    { "运动的描述", "匀变速直线运动", "受力分析与平衡", "牛顿运动定律", "曲线运动与平抛",
      "圆周运动", "万有引力与航天", "功和功率", "机械能守恒定律", "动量定理与守恒",
      "机械振动与机械波", "分子动理论", "气体实验定律", "静电场", "电容器与带电粒子",
      "恒定电流", "磁场与安培力", "带电粒子在磁场中运动", "电磁感应", "交变电流与变压器",
      "光学与全反射", "原子物理" },
    { "化学计量", "离子反应", "氧化还原反应", "原子结构与化学键", "元素周期律",
      "金属及其化合物", "非金属及其化合物", "化学反应速率", "化学平衡移动", "热化学与反应热",
      "电化学", "电解质溶液", "弱电解质的电离", "盐类水解", "沉淀溶解平衡",
      "烃及其性质", "烃的衍生物", "有机合成与推断", "同分异构体", "化学实验基本操作",
      "物质的分离与提纯", "综合推断" },
    { "细胞中的元素与化合物", "细胞结构与功能", "物质跨膜运输", "酶与ATP", "细胞呼吸",
      "光合作用", "细胞增殖", "细胞分化与衰老", "遗传的分子基础", "基因的表达",
      "遗传规律", "伴性遗传", "变异与育种", "基因工程", "内环境与稳态",
      "神经调节", "体液调节", "免疫调节", "植物激素调节", "种群与群落",
      "生态系统", "生物技术实践" },
    { "时态与语态", "主谓一致", "非谓语动词", "定语从句", "名词性从句",
      "状语从句", "虚拟语气", "倒装句与强调句", "情态动词", "介词与连词",
      "冠词与代词", "名词与数词", "形容词与副词", "动词与动词短语", "词义辨析",
      "固定搭配", "情景交际", "完形填空语境", "阅读理解", "七选五",
      "短文改错", "语法综合" },
    { "字音字形", "成语运用", "病句辨析", "标点符号", "词语运用",
      "文言实词", "文言虚词与句式", "文言翻译", "文言文阅读", "古诗词鉴赏",
      "诗歌意象与意境", "诗歌表达技巧", "文学常识", "文化常识", "名句默写",
      "语段衔接", "论述类文本阅读", "实用类文本阅读", "小说阅读", "散文阅读",
      "修辞手法", "作文立意与素材" },
    { "中国古代政治制度", "中国古代选官制度", "中国古代经济", "中国古代科技与文化", "近代列强侵华",
      "太平天国与义和团", "近代中国经济结构变动", "维新思想与三民主义", "新民主主义革命", "抗日战争与解放战争",
      "新中国政治建设", "新中国经济建设", "改革开放", "古希腊罗马", "西方近代政治制度",
      "西方思想解放运动", "两次工业革命", "两次世界大战", "苏俄与苏联建设", "罗斯福新政与战后资本主义",
      "二战后世界格局", "经济全球化" },
    { "商品与货币", "价格与供求", "消费与生产", "企业与劳动者", "投资与理财",
      "分配与公平", "市场经济与宏观调控", "对外开放与对外贸易", "公民的政治参与", "政府与依法行政",
      "我国的政党制度", "民族与宗教政策", "国际社会与外交", "哲学基本问题与派别", "唯物论与认识论",
      "辩证法", "实践与真理", "历史唯物主义", "价值判断与价值选择", "文化与生活",
      "文化传承与创新", "民族精神与社会主义核心价值观" },
    { "地球与地图", "等值线判读", "地球运动", "大气受热过程", "大气环流与气候",
      "天气系统", "水循环与洋流", "内力作用与地貌", "外力作用与地貌", "自然带",
      "人口", "城市", "农业区位", "工业区位", "交通与商业",
      "自然资源与能源", "自然灾害", "生态环境问题", "中国区域地理", "世界区域地理",
      "区域可持续发展", "地理信息技术" } };

static const char *const s_subj_names[9] = {
    "数学", "物理", "化学", "生物", "英语", "语文", "历史", "政治", "地理"
};

extern int s_diff;   /* AI 出题难度: 0=基础 1=中等 2=较难 (main.c 设置页) */
static const char *const s_diff_desc[3] = {
    "难度：基础。重点考查课本核心概念、定义与基本计算，直接运用所学即可作答，选项区分度高。",
    "难度：中等。接近高考常规题，需要一定的分析推理和简单综合。",
    "难度：较难。接近高考压轴题，可综合多个知识点、设置易错陷阱，计算与推理量较大。",
};

/* ================= 薄弱板块统计 (NVS): 人脑式强化选题 =================
 * 每科 22 个知识点各记录: 答错/答对次数 + 最近作答序号.
 * 出题时按“需要强度”加权随机选知识点, 模拟人脑记忆特点:
 *   - 正确率低 → 权重高 (不会的优先练)
 *   - 错题数按对数放大 → 错得越多越优先 (强化期)
 *   - 距上次作答越久 → 权重回升 (遗忘曲线, 已掌握的也会偶尔复习)
 * 存储: NVS 命名空间 "quiz", key st_0..st_8 (每科 132B blob) + seq */
typedef struct {
    uint8_t wrong;       /* 答错次数 (上限 15) */
    uint8_t correct;     /* 答对次数 (上限 30) */
    uint32_t last_seq;   /* 最近作答序号 (全局递增) */
} qz_stat_t;

static qz_stat_t s_qstat[9][22];
static uint32_t s_qz_seq = 0;
static int s_qz_loaded = 0;

static void qz_load(void)
{
    if (s_qz_loaded)
        return;
    nvs_handle_t h;
    if (nvs_open("quiz", NVS_READONLY, &h) == ESP_OK) {
        for (int si = 0; si < 9; si++) {
            char key[8];
            snprintf(key, sizeof(key), "st_%d", si);
            size_t sz = sizeof(s_qstat[si]);
            if (nvs_get_blob(h, key, s_qstat[si], &sz) != ESP_OK)
                memset(s_qstat[si], 0, sizeof(s_qstat[si]));
        }
        if (nvs_get_u32(h, "seq", &s_qz_seq) != ESP_OK)
            s_qz_seq = 0;
        nvs_close(h);
    } else {
        memset(s_qstat, 0, sizeof(s_qstat));
    }
    s_qz_loaded = 1;
}

static void qz_save(int si)
{
    nvs_handle_t h;
    if (nvs_open("quiz", NVS_READWRITE, &h) != ESP_OK)
        return;
    char key[8];
    snprintf(key, sizeof(key), "st_%d", si);
    nvs_set_blob(h, key, s_qstat[si], sizeof(s_qstat[si]));
    nvs_set_u32(h, "seq", s_qz_seq);
    nvs_commit(h);
    nvs_close(h);
}

/* 记录一次 AI 题作答 (答对/答错都记, 掌握度靠两者对比); topic 未知则忽略 */
void quiz_record_answer(int subject_idx, const char *topic, int correct)
{
    if (subject_idx < 0 || subject_idx > 8 || !topic || !topic[0])
        return;
    qz_load();
    int i = 0;
    for (; i < 22; i++)
        if (strcmp(topic, s_topic_pool[subject_idx][i]) == 0)
            break;
    if (i >= 22)
        return;
    qz_stat_t *st = &s_qstat[subject_idx][i];
    s_qz_seq++;
    if (correct) {
        if (st->correct < 30) st->correct++;
    } else {
        if (st->wrong < 15) st->wrong++;
    }
    st->last_seq = s_qz_seq;
    qz_save(subject_idx);
}

/* 知识点“需要强度” (越大越该练): 平滑正确率 + 错题对数强化 + 遗忘回升 */
static double qz_need(int si, int i)
{
    const qz_stat_t *st = &s_qstat[si][i];
    int w = st->wrong, c = st->correct;
    uint32_t gap = s_qz_seq - st->last_seq;
    double skill = (double)(c + 1) / (c + w + 2);   /* 平滑正确率 (0,1) */
    double need = (1.0 - skill) * 2.0;
    if (w > 0)
        need += 0.8 + 0.4 * log2(1.0 + (double)w);  /* 错题对数放大 */
    if (gap > 240)
        need += 0.6;                                /* 久未复习: 遗忘回升 */
    else if (gap > 60)
        need += 0.6 * (gap - 60) / 180.0;
    return need;
}

/* 该知识点是否处于薄弱强化区 (UI 红色标记用) */
int quiz_topic_weak(int subject_idx, const char *topic)
{
    if (subject_idx < 0 || subject_idx > 8 || !topic || !topic[0])
        return 0;
    qz_load();
    for (int i = 0; i < 22; i++)
        if (strcmp(topic, s_topic_pool[subject_idx][i]) == 0)
            return qz_need(subject_idx, i) >= 1.3;
    return 0;
}

/* 该科当前最薄弱的板块名 (静态池指针), 无则返回 "" */
const char *quiz_weak_topic(int subject_idx)
{
    if (subject_idx < 0 || subject_idx > 8)
        return "";
    qz_load();
    int best = -1;
    double bneed = 1.3;
    for (int i = 0; i < 22; i++) {
        double n = qz_need(subject_idx, i);
        if (n > bneed) {
            bneed = n;
            best = i;
        }
    }
    return best >= 0 ? s_topic_pool[subject_idx][best] : "";
}

/* 最近出过的题 (环形缓冲, 用于提示词去重 + 生成后校验) */
#define AI_HIST_N 6
#define AI_HIST_LEN 100
static char s_hist[AI_HIST_N][AI_HIST_LEN];
static const char *s_hist_subj[AI_HIST_N];
static const char *s_hist_topic[AI_HIST_N];
static int s_hist_n = 0;
static int s_hist_next = 0;

static void hist_push(const char *subject, const char *topic, const char *content)
{
    int slot = s_hist_next;
    s_hist_subj[slot] = subject;
    s_hist_topic[slot] = topic;
    int i = 0;
    for (const char *p = content; *p && i < AI_HIST_LEN - 1; p++) {
        if (*p == '\n' || *p == '\r' || *p == '"' || *p == '\\')
            s_hist[slot][i++] = ' ';   /* 换行/引号清掉, 保证可直接嵌入提示词 */
        else
            s_hist[slot][i++] = *p;
    }
    s_hist[slot][i] = 0;
    s_hist_next = (s_hist_next + 1) % AI_HIST_N;
    if (s_hist_n < AI_HIST_N)
        s_hist_n++;
}

/* 选知识点: 排除最近出过的, 从剩余里按“薄弱权重”加权随机
 * (权重 = 0.25 保底 + 需要强度×1.5; 薄弱板块可比已掌握板块高 10 倍以上,
 * 实现“错的板块以合理概率再次出现、不断强化”; 已掌握板块权重低但非零,
 * 配合遗忘回升偶尔复现, 模拟人脑间隔复习) */
static const char *pick_topic(int subj)
{
    qz_load();
    int cand[22], nc = 0;
    for (int i = 0; i < 22; i++) {
        int used = 0;
        for (int j = 0; j < s_hist_n; j++) {
            if (s_hist_topic[j] == s_topic_pool[subj][i]) { used = 1; break; }
        }
        if (!used)
            cand[nc++] = i;
    }
    if (nc == 0)                        /* 兜底: 知识点全用光则随便挑 */
        for (int i = 0; i < 22; i++) cand[nc++] = i;
    double wsum = 0.0;
    for (int k = 0; k < nc; k++)
        wsum += 0.25 + qz_need(subj, cand[k]) * 1.5;
    double r = (double)(esp_random() % 10000) / 10000.0 * wsum;
    for (int k = 0; k < nc; k++) {
        r -= 0.25 + qz_need(subj, cand[k]) * 1.5;
        if (r <= 0.0)
            return s_topic_pool[subj][cand[k]];
    }
    return s_topic_pool[subj][cand[nc - 1]];
}

/* 新题与最近出过的题去重: 开头 12 字出现在任一历史题中即视为重复 */
static int is_dup(const char *content)
{
    int lc = (int)strlen(content);
    for (int i = 0; i < s_hist_n; i++) {
        const char *h = s_hist[i];
        int lh = (int)strlen(h);
        if (lc < 12 || lh < 12)
            continue;
        char cc[13], hh[13];
        memcpy(cc, content, 12); cc[12] = 0;
        memcpy(hh, h, 12); hh[12] = 0;
        if (strstr(content, hh) || strstr(h, cc))
            return 1;
    }
    return 0;
}

static int subject_index(const char *subject)
{
    for (int i = 0; i < 9; i++)
        if (strcmp(subject, s_subj_names[i]) == 0)
            return i;
    return -1;
}


/* 从 content 提取题目 JSON: 逐 "{" 位置用 cJSON 尝试 (字符串感知).
 * 旧实现做括号配对扫描, 题目内容里的分段函数/集合等 { } 会破坏配对,
 * 出现"AI 内容合法却 no json in content"的假失败 (只能重试).
 * cJSON 自己处理字符串与转义: 对每个 "{" 起点的片段尝试解析,
 * 返回第一个含 content 字段的对象 (AI 的题目 JSON 必有该字段);
 * 内容里的 {x+1} 之类小对象无 content 字段, 自动跳过.
 * 解析出的对象必须释放, 否则每次失败请求泄漏 cJSON -> heap 耗尽 */
static char *extract_json(const char *content, char *buf, int bufsz)
{
    const char *rest = content;
    for (int k = 0; k < 8; k++) {          /* 最多尝试 8 个 { 位置 */
        const char *p = strchr(rest, '{');
        if (!p)
            break;
        rest = p + 1;
        int len = (int)strlen(p);
        if (len >= bufsz)
            len = bufsz - 1;
        memcpy(buf, p, (size_t)len);
        buf[len] = 0;
        cJSON *q = cJSON_Parse(buf);
        if (!q)
            continue;                      /* 非法/未闭合: 试下一个 { */
        cJSON *jc = cJSON_GetObjectItem(q, "content");
        int is_topic = jc && cJSON_IsString(jc);
        cJSON_Delete(q);
        if (is_topic)                      /* 有 content 字段即题目对象 */
            return buf;                    /* 缺 answer/options 由上层校验重试 */
    }
    return NULL;
}

/* 出题类型: 数理化英按设置页 s_fill_pct 概率出填空 (软键盘只能输
 * ASCII/符号页字符); 生物/语文/历史/政治/地理答案多为中文无法输入, 不出填空.
 * 实测 deepseek-v4-flash 多空数学填空答案错误率 ~30% (模型能力限制),
 * 建议填空比例不要过高 */
extern int s_fill_pct;   /* main.c 设置页: 填空概率 % (0-100, 默认 20) */

/* 清洗填空答案: 去首尾空白 + 去掉 AI 常见的尾随 . / , */
static void trim_answer(const char *src, char *dst, int dstsz)
{
    int i = 0, j = 0;
    while (src[i] == ' ' || src[i] == '\t')
        i++;
    while (src[i] && j < dstsz - 1)
        dst[j++] = src[i++];
    while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\t' ||
                     dst[j - 1] == '.' || dst[j - 1] == ','))
        j--;
    dst[j] = 0;
}

/* 软键盘符号页 (ui_keyboard.c 符号页2/3) 里能输入的 UTF-8 符号:
 * 答案里出现这些符号用户能原样打出, 校验放行 */
static const char *const kb_syms[] = {
    "×","÷","±","√","π","∞","°","²","³","¹","₂","₃","↑","↓","α","β","γ",
    "Δ","Ω","µ","∑","∫","≤","≥","≠","≈","→","←","½","¼","₀","₁","₄","₅",
    "₆","₇","₈","₉","₊","₋","₌","₍","₎","ₐ","ₑ","ₒ","ₓ","ₔ"
};
#define KB_SYMS_N (sizeof(kb_syms) / sizeof(kb_syms[0]))

/* 填空答案必须能通过键盘输入: 可打印 ASCII 或符号页符号, 非空, ≤40 字符.
 * 中文/希腊字母(ρ等)/无法输入的字符一律拒绝 → 换题重出.
 * 另检测 JSON 格式残留与自弃词 (AI 偶把格式示例/自我怀疑写进答案) */
static int fill_ans_ok(const char *s)
{
    if (strstr(s, "json") || strstr(s, "content") || strstr(s, "answer") ||
        strstr(s, "错误") || strstr(s, "不对") || strstr(s, "无法") ||
        strstr(s, "不确定"))
        return 0;
    int len = 0;
    while (*s) {
        uint8_t c = (uint8_t)*s;
        if (c < 0x20)
            return 0;
        if (c <= 0x7E) {                 /* 可打印 ASCII */
            s++;
            len++;
        } else {
            int matched = 0;
            for (int k = 0; k < KB_SYMS_N; k++) {
                const char *sym = kb_syms[k];
                int sl = (int)strlen(sym);
                if (strncmp(s, sym, (size_t)sl) == 0) {
                    s += sl;
                    len++;
                    matched = 1;
                    break;
                }
            }
            if (!matched)
                return 0;
        }
        if (len > 40)
            return 0;
    }
    return len > 0;
}

int ai_generate_question(const char *subject)
{
    ESP_LOGI(TAG, "aiq start subj=%s heap=%lu", subject ? subject : "?",
             (unsigned long)esp_get_free_heap_size());   /* 诊断 heap */
    if (!s_api_key[0]) {
        ESP_LOGE(TAG, "no api key");
        return -1;
    }
    /* WiFi 未连接时立即失败, 避免 TLS 连接空等 90 秒超时 */
    if (s_wifi_state != 2) {
        ESP_LOGE(TAG, "no wifi (state=%d)", s_wifi_state);
        ai_fail(2, s_wifi_state == 1 ? "WiFi 连接中，请稍候再试" : "未连接 WiFi");
        return -1;
    }
    if (ai_begin() != 0)
        return -1;
    ai_resp_t resp = { 0 };
    resp.buf = malloc(RESP_SIZE);
    if (!resp.buf) {
        ESP_LOGE(TAG, "quiz no mem (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        ai_fail(4, "内存不足");
        ai_end();
        return -1;
    }
    resp.cap = RESP_SIZE;
    resp.body_timeout = 90000;   /* header 后等 body: 慢生成上限 90s */

    int g = (s_grade >= 0 && s_grade <= 2) ? s_grade : 2;
    int d = (s_diff >= 0 && s_diff <= 2) ? s_diff : 1;
    int subj = subject_index(subject);

    /* 最多 3 次尝试: 网络抖动/重复/解析失败时换随机知识点重出 */
    for (int attempt = 0; attempt < 3; attempt++) {

        const char *topic = (subj >= 0) ? pick_topic(subj) : "";

        /* 本题类型: 选择题 或 键盘填空题 (数理化英按用户设置比例;
         * 前 2 次尝试允许填空, 第 3 次强制选择题——填空连续失败时降级,
         * 保证至少能出一题) */
        int want_fill = (subj == 0 || subj == 1 || subj == 2 || subj == 4) &&
                        attempt < 2 &&
                        ((int)(esp_random() % 100) < s_fill_pct);

        /* 最近出过的同科目题清单 (进提示词, 防 AI 重出) */
        static char hist_list[AI_HIST_N * (AI_HIST_LEN + 8)];
        hist_list[0] = 0;
        int used = 0;
        for (int i = 0; i < s_hist_n; i++) {
            int idx = (s_hist_next - 1 - i + AI_HIST_N * 2) % AI_HIST_N;  /* 最新在前 */
            if (s_hist_subj[idx] != subject)
                continue;
            char tmp[AI_HIST_LEN + 16];
            snprintf(tmp, sizeof(tmp), "%d. %s；", ++used, s_hist[idx]);
            if (strlen(hist_list) + strlen(tmp) < sizeof(hist_list) - 1)
                strcat(hist_list, tmp);
        }
        if (!hist_list[0])
            snprintf(hist_list, sizeof(hist_list), "（暂无）");

        static char body[3200];
        if (want_fill) {
            if (subj == 4) {   /* 英语: 专用模板 (纯英语语法/词汇题, 语法填空必须括号给原词) */
                snprintf(body, sizeof(body),
                    "{\"model\":\"deepseek-chat\",\"stream\":true,\"messages\":["
                    "{\"role\":\"system\",\"content\":\"你是一名高中英语出题老师。只输出 JSON，不要输出任何其他文字。\"},"
                    "{\"role\":\"user\",\"content\":\"请为高中%s出一道英语填空题（难度适合%s学生）。%s"
                    "必须围绕知识点「%s」出题；题干必须用纯英文，一至两句话、不超过30个单词；"
                    "严禁用英文出数学、物理、化学等其他学科的题目，必须是英语语法填空、词汇或情景交际题；"
                    "题干留空处用____（连续4个下划线）占位；"
                    "若是语法填空（词形变化），必须在每个空后用括号给出原词，例如：She is ____ (interest) in music.；"
                    "若是词汇题（词义辨析、固定搭配、情景交际），可以只留空不给原词，但语境必须能唯一推出答案；"
                    "答案必须是英文单词或短语的适当形式，只允许英文字母、连字符和空格，总长不超过25个字符；"
                    "严禁中文、严禁数字答案、严禁希腊字母、严禁标点符号；"
                    "设问角度要新颖；解析不超过80字，简明讲清考点和答案即可。"
                    "出题前先在心里完整解答并验证：确保答案与解析一致（解析中禁止出现"
                    "'更正为''实际上应为'等自我否定表述）、每个空都有唯一正确答案、"
                    "语法填空的原词能通过正确的词形变化得出答案，再输出 JSON；"
                    "若发现题目有误或答案不确定，重新设计一道题，绝对禁止输出错误答案。"
                    "随机种子#%lu，不同种子必须出不同的题。"
                    "注意：整个回答里只能输出一个 JSON 对象，绝对禁止输出多个 JSON 或任何多余文字。"
                    "最近已出过的题，禁止重复或高度雷同：%s"
                    "严格按以下 JSON 格式输出（不要 options 字段）："
                    "{\\\"content\\\":\\\"题目内容(含____和原词括号)\\\",\\\"answer\\\":\\\"参考答案\\\","
                    "\\\"explanation\\\":\\\"解析\\\"}\"}"
                    "],\"max_tokens\":2000,\"temperature\":0.8}",
                    grade_names[g], grade_names[g], s_diff_desc[d], topic,
                    (unsigned long)esp_random(), hist_list);
            } else {
                snprintf(body, sizeof(body),
                "{\"model\":\"deepseek-chat\",\"stream\":true,\"messages\":["
                "{\"role\":\"system\",\"content\":\"你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。\"},"
                "{\"role\":\"user\",\"content\":\"请为高中%s出一道%s填空题（难度适合%s学生）。%s"
                "必须围绕知识点「%s」出题；题干留空处用____（连续4个下划线）占位；"
                "尽量只留1个空（最多2个空），答案尽量是单一数字或简单分数，避免复杂的表达式、区间或字母答案；"
                "答案必须能在英文键盘上输入：只允许数字、英文单词/短语和常见符号（+ - × ÷ = < > / ( ) . %% ^ ± √ π ² ³ 等）；"
                "严禁任何中文，包括 能/不能、增大/减小、相反、或、且 等汉字词一律禁用，判断类答案用 yes/no，变化类用 increase/decrease；"
                "严禁中文标点、严禁希腊字母（ρ θ α 等）、严禁上下标字符、严禁带单位；若有多处留空，答案用英文分号分隔；"
                "答案总长不超过25个字符；"
                "设问角度要新颖。题干长度适中：中文题干不超过120字，可适当设置情境但不要冗长铺陈；"
                "英语题干不超过30个单词、一至两句话。解析不超过80字，简明讲清考点和答案即可。"
                "出题前先在心里完整解答，然后用代入法逐一验证：把每个答案代回题干条件检验是否成立（如把λ值代回垂直/平行/范围条件），"
                "确认所有答案都能通过代入检验、各空答案与题干留空一一对应、答案与解析完全一致（解析中禁止出现"
                "'更正为''实际上应为'等自我否定表述），再输出 JSON；"
                "若代入检验不成立或答案不确定，重新设计一道题，绝对禁止输出错误或自相矛盾的答案。"
                "随机种子#%lu，不同种子必须出不同的题。"
                "注意：如果一道题不合适，直接换一道再写，但整个回答里只能输出一个 JSON 对象，绝对禁止输出多个 JSON 或任何多余文字。"
                "最近已出过的题，禁止重复或高度雷同：%s"
                "严格按以下 JSON 格式输出（不要 options 字段）："
                "{\\\"content\\\":\\\"题目内容(含____)\\\",\\\"answer\\\":\\\"参考答案\\\","
                "\\\"explanation\\\":\\\"解析\\\"}\"}"
                "],\"max_tokens\":2000,\"temperature\":0.8}",
                grade_names[g], subject, grade_names[g], s_diff_desc[d], topic,
                (unsigned long)esp_random(), hist_list);
            }
        } else {
            snprintf(body, sizeof(body),
                "{\"model\":\"deepseek-chat\",\"stream\":true,\"messages\":["
                "{\"role\":\"system\",\"content\":\"你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。\"},"
                "{\"role\":\"user\",\"content\":\"请为高中%s出一道%s选择题（难度适合%s学生）。%s"
                "必须围绕知识点「%s」出题；设问角度要新颖，避免“下列关于…的叙述，正确的是”这类俗套问法，"
                "干扰项要有迷惑性。题干长度适中：中文题干不超过150字，可适当设置情境但不要冗长铺陈；"
                "英语题干不超过30个单词、一至两句话（仍只出语法/词汇/情景交际单选，严禁阅读理解式长文）；"
                "每个选项不超过20字（英语不超过8个单词）；解析不超过180字，须讲清考点、错因和关键步骤。"
                "出题前先在心里完整解答并验算：确保答案正确、与解析完全一致（解析中禁止出现"
                "'更正为''实际上应为'等自我否定表述），确认无误再输出 JSON；"
                "若发现题目有误或答案不确定，重新设计一道题，绝对禁止输出错误答案。"
                "随机种子#%lu，不同种子必须出不同的题。"
                "注意：如果一道题不合适，直接换一道再写，但整个回答里只能输出一个 JSON 对象，绝对禁止输出多个 JSON 或任何多余文字。"
                "最近已出过的题，禁止重复或高度雷同：%s"
                "严格按以下 JSON 格式输出："
                "{\\\"content\\\":\\\"题目内容\\\",\\\"options\\\":{\\\"A\\\":\\\"选项A\\\",\\\"B\\\":\\\"选项B\\\","
                "\\\"C\\\":\\\"选项C\\\",\\\"D\\\":\\\"选项D\\\"},\\\"answer\\\":\\\"A\\\",\\\"explanation\\\":\\\"解析\\\"}\"}"
                "],\"max_tokens\":1200,\"temperature\":0.8}",
                grade_names[g], subject, grade_names[g], s_diff_desc[d], topic,
                (unsigned long)esp_random(), hist_list);
        }

        esp_err_t err = ai_post(&resp, body, 20000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "quiz post failed: %s (heap=%lu)", esp_err_to_name(err),
                     (unsigned long)esp_get_free_heap_size());
            /* 网络瞬时故障 (如 TLS 后连接被 RST) 常见: 重试, 不直接判失败 */
            ai_fail(5, "网络连接失败，正在重试");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (resp.len == 0) {
            ESP_LOGW(TAG, "empty response, retry %d", attempt + 1);
            ai_fail(6, "AI 响应为空");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        resp.buf[resp.len] = 0;
        if (resp.overflow)
            ESP_LOGW(TAG, "resp buffer overflow (%d bytes dropped)", resp.len);
        ESP_LOGI(TAG, "resp %d bytes", resp.len);
        ESP_LOGI(TAG, "resp head: %.400s", resp.buf);   /* 诊断: 看响应原文 */

        /* 流式响应: resp.buf 已是累积的 content 文本 */
        if (!resp.buf[0]) {
            ESP_LOGW(TAG, "no content in response, retry %d", attempt + 1);
            ai_fail(6, "AI 响应内容为空");
            continue;
        }
        if (resp.finish_length) {       /* max_tokens 截断: 换知识点重出 */
            ESP_LOGW(TAG, "response truncated by max_tokens");
            ai_fail(11, "AI 输出超长被截断");
            continue;
        }
        const char *cstr = resp.buf;

        /* content 里提取题目 JSON (题干/选项加大后 JSON 上限约 6KB) */
        static char qjson[6144];
        if (!extract_json(cstr, qjson, sizeof(qjson))) {
            ESP_LOGW(TAG, "no json in content, retry %d (%.1200s)", attempt + 1, cstr);
            ai_fail(7, "内容解析失败");
            continue;
        }

        cJSON *q = cJSON_Parse(qjson);
        if (!q) {
            ESP_LOGW(TAG, "question json parse fail: %.200s, retry %d", qjson, attempt + 1);
            ai_fail(8, "题目 JSON 解析失败");
            continue;
        }
        cJSON *jc = cJSON_GetObjectItem(q, "content");
        cJSON *jo = cJSON_GetObjectItem(q, "options");
        cJSON *ja = cJSON_GetObjectItem(q, "answer");
        cJSON *je = cJSON_GetObjectItem(q, "explanation");
        if (!jc || !cJSON_IsString(jc) || !ja || !cJSON_IsString(ja)) {
            cJSON_Delete(q);
            ESP_LOGW(TAG, "question json missing fields, retry %d", attempt + 1);
            ai_fail(8, "题目字段缺失");
            continue;
        }

        static char content_buf[2048];   /* 语文论述文段题干可达 600+ 字, 1024 会截掉最后字 */
        utf8_truncate(cJSON_GetStringValue(jc), content_buf, sizeof(content_buf));

        /* 题型判定: 有非空 options 对象 → 选择题; 否则 → 填空题 */
        int is_fill = !(jo && cJSON_IsObject(jo) && cJSON_GetArraySize(jo) > 0);

        /* 填空题先做完整性校验 (通过才进历史/落盘), 失败换题重出 */
        static char ans_buf[64];
        if (is_fill) {
            if (!strstr(content_buf, "____")) {
                cJSON_Delete(q);
                ESP_LOGW(TAG, "fill no blank in content, retry %d", attempt + 1);
                ai_fail(9, "填空题缺空位");
                continue;
            }
            trim_answer(cJSON_GetStringValue(ja), ans_buf, sizeof(ans_buf));
            if (!fill_ans_ok(ans_buf)) {
                cJSON_Delete(q);
                ESP_LOGW(TAG, "fill bad answer '%.30s', retry %d", ans_buf, attempt + 1);
                ai_fail(9, "填空答案无法输入");
                continue;
            }
        }

        /* 与最近出过的题去重; 重复则换知识点重出 */
        if (is_dup(content_buf)) {
            cJSON_Delete(q);
            ESP_LOGW(TAG, "dup with history, retry %d", attempt + 1);
            ai_fail(10, "与最近题目重复");
            continue;
        }
        hist_push(subject, topic, content_buf);

        memset(&g_ai_q, 0, sizeof(g_ai_q));
        g_ai_q.subject = subject;   /* 指向调用方字符串 (静态) */
        g_ai_q.topic = topic;       /* 考点 (薄弱板块强化选题的依据) */
        g_ai_q.content = content_buf;
        if (je && cJSON_IsString(je)) {
            static char expl_buf[1024];   /* 解析 ≤180 字, 1024B 足够 */
            utf8_truncate(cJSON_GetStringValue(je), expl_buf, sizeof(expl_buf));
            g_ai_q.explanation = expl_buf;
        } else {
            g_ai_q.explanation = "";
        }

        if (!is_fill) {
            /* ---------- 选择题 ---------- */
            static char opts_buf[4][512];   /* 语文/政治长选项可超 85 字, 256 会截掉最后字 */
            for (int i = 0; i < 4; i++) {
                cJSON *o = cJSON_GetObjectItem(jo, (char[]){'A' + i, 0});
                if (o && cJSON_IsString(o)) {
                    utf8_truncate(cJSON_GetStringValue(o), opts_buf[i],
                                  sizeof(opts_buf[i]));
                } else {
                    opts_buf[i][0] = 0;
                }
                g_ai_q.options[i] = opts_buf[i];
            }
            const char *ans = cJSON_GetStringValue(ja);
            g_ai_q.answer_idx = (ans && ans[0] >= 'A' && ans[0] <= 'D') ? ans[0] - 'A' : 0;
            g_ai_q.is_choice = 1;
        } else {
            /* ---------- 填空题 (软键盘输入: 答案须为可打印 ASCII) ---------- */
            g_ai_q.answer_text = ans_buf;
            g_ai_q.is_choice = 2;
        }
        cJSON_Delete(q);
        ESP_LOGI(TAG, "AI CONTENT[%d]: %s", (int)strlen(g_ai_q.content), g_ai_q.content);
        ESP_LOGI(TAG, "AI TYPE=%d ANS=%s", g_ai_q.is_choice,
                 g_ai_q.is_choice == 1 ? (char[]){'A' + g_ai_q.answer_idx, 0}
                                       : (g_ai_q.answer_text ? g_ai_q.answer_text : ""));
        ESP_LOGI(TAG, "aiq done heap=%lu", (unsigned long)esp_get_free_heap_size());  /* 诊断 */
        free(resp.buf); ai_end();;
        return 0;
    }
    ESP_LOGE(TAG, "ai_generate_question: all attempts failed");
    ai_fail(12, "出题失败，请重试");
    free(resp.buf); ai_end();;
    return -1;
}

/* ---------- 失败原因透出 (UI 显示) ---------- */
static char s_ai_last_err[64] = "";
static int s_ai_last_code = 0;

const char *ai_last_error(void)
{
    return s_ai_last_err[0] ? s_ai_last_err : "未知错误";
}

int ai_last_code(void)
{
    return s_ai_last_code;
}

static void ai_set_err(const char *e)
{
    snprintf(s_ai_last_err, sizeof(s_ai_last_err), "%s", e);
}

/* 带错误码的失败 (屏显 "E%d 说明", 用户可直接报码定位):
 *  E1 无APIKey  E2 无WiFi  E3 忙  E4 内存  E5 网络
 *  E6 空响应   E7 内容非JSON  E8 JSON字段缺失  E9 填空校验
 *  E10 重复题  E11 max_tokens截断  E12 全部尝试失败 */
static void ai_fail(int code, const char *e)
{
    s_ai_last_code = code;
    snprintf(s_ai_last_err, sizeof(s_ai_last_err), "E%d %s", code, e);
}

/* ---------- AI 薄弱点分析 ---------- */
static char s_weak_out[8192];   /* 每批 ≤5 题, 8KB 匹配 weak.c AI 区 */

const char *ai_analyze_weakness(const char *subject, const char *topics)
{
    s_weak_out[0] = 0;
    if (!s_api_key[0])
        return s_weak_out;

    /* topics 需 JSON 转义 + 清洗 (旧错题可能含半个汉字/控制字符) */
    static char esc_t[1200];
    esc_for_json(topics, esc_t, sizeof(esc_t));

    char body[2200];
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是高中学习辅导老师。\"},"
        "{\"role\":\"user\",\"content\":\"以下是高中学生的错题（科目：%s；每题已编号，如 1.xxx；2.xxx）：%s。"
        "请逐题简要分析：每道题用『题N』开头单独一段，指出该题考查的知识点、主要错因、一句改进建议，"
        "每题1-2行，不要JSON，不要总结性的废话。\"}"
        "],\"max_tokens\":1500,\"temperature\":0.5}",
        subject, esc_t);

    if (ai_begin() != 0)
        return s_weak_out;
    ai_resp_t resp = { 0 };
    resp.buf = malloc(RESP_SIZE);
    if (!resp.buf) {
        ESP_LOGE(TAG, "weak no mem (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        ai_set_err("内存不足");
        ai_end();
        return s_weak_out;
    }
    resp.cap = RESP_SIZE;
    resp.body_timeout = 90000;

    /* 失败自动重试 (最多 3 次) */
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t err = ai_post(&resp, body, 90000);
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
        }
        resp.buf[resp.len] = 0;
        if (resp.overflow)
            ESP_LOGW(TAG, "weak resp buffer overflow");
        if (!resp.buf[0]) {
            ESP_LOGE(TAG, "weak resp no content");
            ai_set_err("AI 响应内容为空");
            if (attempt < 1) {
                ESP_LOGW(TAG, "weak no content, retry");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }
        {
            const char *cstr = resp.buf;
            /* 去掉可能的前后空白/引号 */
            while (*cstr == ' ' || *cstr == '\n' || *cstr == '"')
                cstr++;
            int len = (int)strlen(cstr);
            while (len > 0 && (cstr[len-1] == ' ' || cstr[len-1] == '\n' || cstr[len-1] == '"'))
                len--;
            if (resp.finish_length) {       /* max_tokens 截断: 保留已生成部分 */
                ESP_LOGW(TAG, "weak analysis truncated (%d bytes)", len);
            }
            if (len >= (int)sizeof(s_weak_out)) {
                len = sizeof(s_weak_out) - 1;
                while (len > 0 && ((uint8_t)cstr[len] & 0xC0) == 0x80)
                    len--;
                if (len > 0 && (uint8_t)cstr[len] >= 0xC0)
                    len--;
            }
            memcpy(s_weak_out, cstr, (size_t)len);
            s_weak_out[len] = 0;
        }
        ESP_LOGI(TAG, "weak analysis: %.60s", s_weak_out);
        free(resp.buf);
        ai_end();
        return s_weak_out;
    }
    free(resp.buf);
    ai_end();
    ESP_LOGE(TAG, "ai_analyze_weakness: all attempts failed");
    return s_weak_out;
}

/* ---------- AI 知识库: 生成某科某主题的核心知识点 ---------- */
static char s_kb_out[KB_OUT_MAX];

const char *ai_get_knowledge(const char *subject, const char *topic)
{
    s_kb_out[0] = 0;
    if (!s_api_key[0]) {
        ai_set_err("未配置 API Key");
        return s_kb_out;
    }

    char body[1500];
    int g = (s_grade >= 0 && s_grade <= 2) ? s_grade : 2;
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是高中辅导老师，知识面广，表达简洁有条理。\"},"
        "{\"role\":\"user\",\"content\":\"请整理高中%s《%s》中「%s」的核心知识点："
        "分点列出：每个知识点必须单独占一行，用数字编号（1. 2. 3. …）开头，"
        "行与行之间必须用换行符换行，严禁把多个知识点挤在同一行或输出成一大段不换行的文字；"
        "每个编号点内不超过2行。内容包括：重要概念定义、关键公式/原理、常见易错点、典型例题提示。"
        "覆盖全部考点，控制在3000字以内（严禁超过3000字，超长输出会被系统截断丢弃）。"
        "直接输出文字，不要JSON。\"}"
        "],\"max_tokens\":6144,\"temperature\":0.4}",
        grade_names[g], subject, topic);

    if (ai_begin() != 0)
        return s_kb_out;
    ai_resp_t resp = { 0 };
    resp.buf = malloc(RESP_SIZE);
    if (!resp.buf) {
        ESP_LOGE(TAG, "kb no mem (heap=%lu)", (unsigned long)esp_get_free_heap_size());
        ai_set_err("内存不足");
        ai_end();
        return s_kb_out;
    }
    resp.cap = RESP_SIZE;
    resp.body_timeout = 300000;   /* 知识库 3000 字生成可达 1-3 分钟 */

    /* 失败自动重试 (最多 3 次): 网络瞬时错误 / 服务端偶发异常 */
    for (int attempt = 0; attempt < 3; attempt++) {
        ESP_LOGI(TAG, "kb start[%d]: resp=%p cap=%d heap=%lu", attempt,
                 (void *)resp.buf, resp.cap,
                 (unsigned long)esp_get_free_heap_size());
        esp_err_t err = ai_post(&resp, body, 300000);
        ESP_LOGI(TAG, "kb perform[%d]: err=%s len=%d overflow=%d",
                 attempt, esp_err_to_name(err), resp.len, resp.overflow);
        if (resp.len > 0) {
            resp.buf[resp.len] = 0;
            ESP_LOGI(TAG, "kb resp head: %.120s", resp.buf);
        }
        if (err != ESP_OK) {
            ai_set_err(esp_err_to_name(err));
            if (attempt < 2) {
                ESP_LOGW(TAG, "kb http fail, retry %d", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }
        resp.buf[resp.len] = 0;
        if (resp.overflow) {
            ESP_LOGW(TAG, "kb resp buffer overflow (%d bytes)", resp.len);
            ai_set_err("响应超过缓冲上限");
        }
        if (!resp.buf[0]) {
            ESP_LOGE(TAG, "kb resp no content");
            ai_set_err("AI 响应内容为空");
            if (attempt < 2) {
                ESP_LOGW(TAG, "kb no content, retry %d", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }
        {
            const char *cstr = resp.buf;
            while (*cstr == ' ' || *cstr == '\n' || *cstr == '"')
                cstr++;
            int len = (int)strlen(cstr);
            while (len > 0 && (cstr[len-1] == ' ' || cstr[len-1] == '\n' || cstr[len-1] == '"'))
                len--;
            if (resp.finish_length) {       /* max_tokens 截断: 保留已生成部分 */
                ESP_LOGW(TAG, "kb truncated by max_tokens (%d bytes)", len);
            }
            if (len >= (int)sizeof(s_kb_out)) {
                len = sizeof(s_kb_out) - 1;
                while (len > 0 && ((uint8_t)cstr[len] & 0xC0) == 0x80)
                    len--;
                if (len > 0 && (uint8_t)cstr[len] >= 0xC0)
                    len--;
            }
            memcpy(s_kb_out, cstr, (size_t)len);
            s_kb_out[len] = 0;
        }
        ESP_LOGI(TAG, "kb generated[%d]: %d bytes", attempt, (int)strlen(s_kb_out));
        free(resp.buf);
        ai_end();
        return s_kb_out;
    }
    free(resp.buf);
    ai_end();
    ESP_LOGE(TAG, "ai_get_knowledge: all attempts failed");
    return s_kb_out;
}

/* ---------- AI 拼音/英文主题名 → 中文主题名 ---------- */
static char s_trans_buf[64];

const char *ai_translate_topic(const char *subject, const char *text)
{
    s_trans_buf[0] = 0;
    if (!s_api_key[0])
        return s_trans_buf;

    char body[800];
    snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是主题名转换器。\"},"
        "{\"role\":\"user\",\"content\":\"以下是高中%s科目的主题名输入（拼音或中英混合），"
        "请结合该科目的学科术语转换成规范的中文主题名"
        "（如生物科 guanghe zuoyong→光合作用，化学科 PCR yuanli→PCR原理，"
        "数学科 hanshu yu daoshu→函数与导数，英语科 shi tai→时态）。"
        "拼音按音节逐字对应正确的汉字，不要用同音错字；学科专有名词（PCR、DNA、"
        "AB等）保留英文大写，不要翻译。只输出转换后的中文主题名，"
        "不要任何解释、标点或多余文字。输入：%s\"}"
        "],\"max_tokens\":30,\"temperature\":0.1}",
        subject ? subject : "", text);

    if (ai_begin() != 0)
        return s_trans_buf;
    ai_resp_t resp = { 0 };
    resp.buf = malloc(RESP_SIZE);
    if (!resp.buf) {
        ai_set_err("内存不足");
        ai_end();
        return s_trans_buf;
    }
    resp.cap = RESP_SIZE;
    resp.body_timeout = 30000;

    /* 失败自动重试 (最多 3 次) */
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t err = ai_post(&resp, body, 30000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "trans http failed: %s", esp_err_to_name(err));
            if (attempt < 1) {
                ESP_LOGW(TAG, "trans http fail, retry");
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            continue;
        }
        resp.buf[resp.len] = 0;
        {
            const char *cstr = resp.buf;
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
        ESP_LOGI(TAG, "trans: '%s' -> '%s'", text, s_trans_buf);
        free(resp.buf);
        ai_end();
        return s_trans_buf;
    }
    free(resp.buf);
    ai_end();
    ESP_LOGE(TAG, "ai_translate_topic: all attempts failed");
    return s_trans_buf;
}
