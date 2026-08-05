#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""模拟 ai_quiz.c 出题全流程: 提示词构造(复刻C字符串转义) -> API调用 -> 解析校验"""
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

def build_body(subject, topic, want_fill, grade=2, diff=1, hist="（暂无）"):
    g = grade_names[grade]
    if want_fill:
        base = (f"请为高中{g}出一道{g}填空题（难度适合{g}学生）。{s_diff_desc[diff]}"
                f"必须围绕知识点「{topic}」出题；题干留空处用____（连续4个下划线）占位，全题1~2处留空；"
                "答案必须是纯数字、英文单词/短语或简短ASCII表达式（化学式下标用普通数字，如 Na2O2；"
                "严禁中文答案、严禁带单位、严禁上下标字符），答案总长不超过20个字符；"
                "设问角度要新颖。题干长度适中：中文题干不超过120字，可适当设置情境但不要冗长铺陈；"
                "英语题干不超过30个单词、一至两句话。解析不超过180字，须讲清考点、错因和关键步骤。"
                "随机种子#12345，不同种子必须出不同的题。"
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
                f"最近已出过的题，禁止重复或高度雷同：{hist}"
                '严格按以下 JSON 格式输出：'
                '{"content":"题目内容","options":{"A":"选项A","B":"选项B","C":"选项C","D":"选项D"},"answer":"A","explanation":"解析"}')
    body = {"model": "deepseek-chat",
            "messages": [
                {"role": "system", "content": "你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。"},
                {"role": "user", "content": base}],
            "max_tokens": 1200, "temperature": 1.0}
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

def extract_json(content):
    s = content.find('{')
    e = content.rfind('}')
    if s < 0 or e <= s:
        return None
    return content[s:e+1]

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
    return all(0x20 <= ord(c) <= 0x7E for c in s)

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
    subj = sys.argv[1] if len(sys.argv) > 1 else "数学"
    mode = sys.argv[2] if len(sys.argv) > 2 else "fill"
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    print(f"=== 模拟: {subj} {mode} x{rounds} ===", flush=True)
    if mode in ("fill", "both"):
        for res in simulate(subj, True, rounds):
            print(res, flush=True)
    if mode in ("choice", "both"):
        for res in simulate(subj, False, rounds):
            print(res, flush=True)
