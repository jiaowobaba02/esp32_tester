#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""模拟 v2: 修复版提示词 + 顶层对象配对扫描 + 键盘符号表校验"""
import json, random, sys, urllib.request, urllib.error

KEY = open('/media/user/6AF00DA6F00D7A19/esp32-tester/deepseek_api_key.txt').read().strip()
URL = "https://api.deepseek.com/chat/completions"

grade_names = ["高一", "高二", "高三"]
s_diff_desc = [
    "难度：基础。重点考查课本核心概念、定义与基本计算，直接运用所学即可作答，选项区分度高。",
    "难度：中等。接近高考常规题，需要一定的分析推理和简单综合。",
    "难度：较难。接近高考压轴题，可综合多个知识点、设置易错陷阱，计算与推理量较大。",
]
topic_pool = {
    "数学": ["集合与函数","函数性质与图像","基本初等函数","函数零点","三角函数","三角恒等变换","解三角形","数列通项与求和","不等式","线性规划","平面向量","立体几何初步","空间向量与坐标","直线与圆","椭圆与双曲线","抛物线","参数方程与极坐标","导数与单调性","导数与极值最值","概率","统计与分布列","排列组合与二项式"],
    "英语": ["时态与语态","主谓一致","非谓语动词","定语从句","名词性从句","状语从句","虚拟语气","倒装句与强调句","情态动词","介词与连词","冠词与代词","名词与数词","形容词与副词","动词与动词短语","词义辨析","固定搭配","情景交际","完形填空语境","阅读理解","七选五","短文改错","语法综合"],
    "生物": ["细胞中的元素与化合物","细胞结构与功能","物质跨膜运输","酶与ATP","细胞呼吸","光合作用","细胞增殖","细胞分化与衰老","遗传的分子基础","基因的表达","遗传规律","伴性遗传","变异与育种","基因工程","内环境与稳态","神经调节","体液调节","免疫调节","植物激素调节","种群与群落","生态系统","生物技术实践"],
    "物理": ["运动的描述","匀变速直线运动","受力分析与平衡","牛顿运动定律","曲线运动与平抛","圆周运动","万有引力与航天","功和功率","机械能守恒定律","动量定理与守恒","机械振动与机械波","分子动理论","气体实验定律","静电场","电容器与带电粒子","恒定电流","磁场与安培力","带电粒子在磁场中运动","电磁感应","交变电流与变压器","光学与全反射","原子物理"],
    "化学": ["化学计量","离子反应","氧化还原反应","原子结构与化学键","元素周期律","金属及其化合物","非金属及其化合物","化学反应速率","化学平衡移动","热化学与反应热","电化学","电解质溶液","弱电解质的电离","盐类水解","沉淀溶解平衡","烃及其性质","烃的衍生物","有机合成与推断","同分异构体","化学实验基本操作","物质的分离与提纯","综合推断"],
}

# 键盘符号页允许的 UTF-8 符号 (ui_keyboard.c 符号页2/3)
KB_SYMBOLS = ["×","÷","±","√","π","∞","°","²","³","¹","₂","₃","↑","↓","α","β","γ","Δ","Ω","µ","∑","∫","≤","≥","≠","≈","→","←","½","¼","₀","₁","₄","₅","₆","₇","₈","₉","₊","₋","₌","₍","₎","ₐ","ₑ","ₒ","ₓ","ₔ"]

def build_body(subject, topic, want_fill, grade=2, diff=1, hist="（暂无）"):
    g = grade_names[grade]
    if want_fill:
        base = (f"请为高中{g}出一道{g}填空题（难度适合{g}学生）。{s_diff_desc[diff]}"
                f"必须围绕知识点「{topic}」出题；题干留空处用____（连续4个下划线）占位；"
                "尽量只留1个空（最多2个空），答案尽量是单一数字或简单分数，避免复杂的表达式、区间或字母答案；"
                "答案必须能在英文键盘上输入：只允许数字、英文单词/短语和常见符号（+ - × ÷ = < > / ( ) . % ^ ± √ π ² ³ 等）；"
                "严禁任何中文，包括 能/不能、增大/减小、相反、或、且 等汉字词一律禁用，判断类答案用 yes/no，变化类用 increase/decrease；"
                "严禁中文标点、严禁希腊字母（ρ θ α 等）、严禁上下标字符、严禁带单位；若有多处留空，答案用英文分号分隔；"
                "答案总长不超过25个字符；"
                "设问角度要新颖。题干长度适中：中文题干不超过120字；英语题干不超过30个单词。"
                "解析不超过80字，简明讲清考点和答案即可。"
                "出题前先在心里完整解答，然后用代入法逐一验证：把每个答案代回题干条件检验是否成立（如把λ值代回垂直/平行/范围条件），"
                "确认所有答案都能通过代入检验、各空答案与题干留空一一对应、解析能推导出答案，再输出 JSON；"
                "若代入检验不成立或答案不确定，重新设计一道题，绝对禁止输出错误或自相矛盾的答案。"
                "随机种子#12345，不同种子必须出不同的题。"
                "注意：如果一道题不合适，直接换一道再写，但整个回答里只能输出一个 JSON 对象，绝对禁止输出多个 JSON 或任何多余文字。"
                f"最近已出过的题，禁止重复或高度雷同：{hist}"
                '严格按以下 JSON 格式输出（不要 options 字段）：'
                '{"content":"题目内容(含____)","answer":"参考答案","explanation":"解析"}')
    else:
        base = (f"请为高中{g}出一道{g}选择题（难度适合{g}学生）。{s_diff_desc[diff]}"
                f"必须围绕知识点「{topic}」出题；设问角度要新颖，避免“下列关于…的叙述，正确的是”这类俗套问法，"
                "干扰项要有迷惑性。题干长度适中：中文题干不超过150字，可适当设置情境但不要冗长铺陈；"
                "英语题干不超过30个单词、一至两句话（仍只出语法/词汇/情景交际单选，严禁阅读理解式长文）；"
                "每个选项不超过20字（英语不超过8个单词）；解析不超过180字，须讲清考点、错因和关键步骤。"
                "随机种子#12345，不同种子必须出不同的题。"
                "注意：如果一道题不合适，直接换一道再写，但整个回答里只能输出一个 JSON 对象，绝对禁止输出多个 JSON 或任何多余文字。"
                f"最近已出过的题，禁止重复或高度雷同：{hist}"
                '严格按以下 JSON 格式输出：'
                '{"content":"题目内容","options":{"A":"选项A","B":"选项B","C":"选项C","D":"选项D"},"answer":"A","explanation":"解析"}')
    body = {"model": "deepseek-chat",
            "messages": [
                {"role": "system", "content": "你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。"},
                {"role": "user", "content": base}],
            "max_tokens": (2000 if want_fill else 1200), "temperature": 0.8}
    return json.dumps(body, ensure_ascii=False)

def call_api(body):
    req = urllib.request.Request(URL, data=body.encode('utf-8'),
                                 headers={"Content-Type": "application/json",
                                          "Accept-Encoding": "identity",
                                          "Authorization": "Bearer " + KEY})
    try:
        with urllib.request.urlopen(req, timeout=90) as r:
            return r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return f"HTTP_ERROR {e.code}: {e.read().decode('utf-8','replace')[:300]}"
    except Exception as e:
        return f"NET_ERROR {e}"

def top_level_objects(s):
    """配对扫描: 返回所有顶层 {...} 区间 (字符串内的括号不识别, 由调用方解析兜底)"""
    objs = []
    depth = 0
    start = -1
    for i, ch in enumerate(s):
        if ch == '{':
            if depth == 0:
                start = i
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0 and start >= 0:
                objs.append((start, i + 1))
                start = -1
    return objs

def extract_json(content):
    """优先返回含 content+answer 字段的顶层对象; 否则第一个能解析的"""
    for (a, b) in top_level_objects(content):
        try:
            q = json.loads(content[a:b])
        except Exception:
            continue
        if isinstance(q, dict) and isinstance(q.get("content"), str) and isinstance(q.get("answer"), str):
            return content[a:b]
    for (a, b) in top_level_objects(content):
        try:
            json.loads(content[a:b])
            return content[a:b]
        except Exception:
            continue
    return None

def trim_answer(s):
    s = s.strip()
    while s and s[-1] in '.,':
        s = s[:-1]
    return s.strip()

def fill_ans_ok(s):
    if not s:
        return False
    if len(s) > 40:
        return False
    for bad in ("json", "content", "answer", "错误", "不对", "无法", "不确定"):
        if bad in s:
            return False
    i = 0
    while i < len(s):
        c = s[i]
        if 0x20 <= ord(c) <= 0x7E:
            i += 1
            continue
        ok = False
        for sym in KB_SYMBOLS:
            if s.startswith(sym, i):
                ok = True
                i += len(sym)
                break
        if not ok:
            return False
    return True

def simulate(subject, want_fill, rounds=5):
    results = []
    for r in range(rounds):
        topic = random.choice(topic_pool[subject])
        body = build_body(subject, topic, want_fill)
        raw = call_api(body)
        if raw.startswith(("HTTP_ERROR", "NET_ERROR")):
            results.append(("API_FAIL", topic, raw[:200]))
            continue
        try:
            root = json.loads(raw)
            cstr = root["choices"][0]["message"]["content"]
            fr = root["choices"][0].get("finish_reason")
        except Exception:
            results.append(("PARSE_ROOT_FAIL", topic, raw[:300]))
            continue
        qj = extract_json(cstr)
        if not qj:
            results.append(("NO_JSON", topic, cstr[:200]))
            continue
        try:
            q = json.loads(qj)
        except Exception:
            results.append(("QJSON_FAIL", topic, qj[:200]))
            continue
        jc = q.get("content"); ja = q.get("answer"); jo = q.get("options")
        if not isinstance(jc, str) or not isinstance(ja, str):
            results.append(("MISSING_FIELDS", topic, qj[:200]))
            continue
        is_fill = not (isinstance(jo, dict) and len(jo) > 0)
        if is_fill:
            if "____" not in jc:
                results.append(("FILL_NO_BLANK", topic, f"fr={fr} jc={jc[:120]}"))
                continue
            ans = trim_answer(ja)
            if not fill_ans_ok(ans):
                results.append(("FILL_BAD_ANS", topic, f"fr={fr} ans={ja!r} jc={jc[:60]}"))
                continue
            results.append(("FILL_OK", topic, f"fr={fr} ans={ans!r} 题干={jc[:50]}"))
        else:
            if not all(isinstance(jo.get(k), str) for k in "ABCD"):
                results.append(("CHOICE_BAD_OPTS", topic, qj[:200]))
                continue
            if ja not in "ABCD":
                results.append(("CHOICE_BAD_ANS", topic, f"ans={ja!r}"))
                continue
            results.append(("CHOICE_OK", topic, f"fr={fr} ans={ja} 题干={jc[:50]}"))
    return results

if __name__ == "__main__":
    subjects = sys.argv[1].split(",") if len(sys.argv) > 1 else ["数学"]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    for subj in subjects:
        print(f"=== {subj} 填空 x{rounds} ===", flush=True)
        res = simulate(subj, True, rounds)
        ok = sum(1 for r in res if r[0] == "FILL_OK")
        for r in res:
            print(r, flush=True)
        print(f"--- 成功率 {ok}/{len(res)} ---", flush=True)
