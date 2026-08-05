#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证 DeepSeek SSE 流式响应格式 + 复刻 sse_extract_content 逻辑"""
import json, urllib.request, urllib.error, sys

KEY = open('/media/user/6AF00DA6F00D7A19/esp32-tester/deepseek_api_key.txt').read().strip()
URL = "https://api.deepseek.com/chat/completions"

body = {
    "model": "deepseek-chat",
    "messages": [
        {"role": "system", "content": "你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。"},
        {"role": "user", "content": "请为高中数学出一道基础选择题，围绕知识点「集合与函数」。严格按 JSON 输出：{\"content\":\"题目\",\"options\":{\"A\":\"a\",\"B\":\"b\",\"C\":\"c\",\"D\":\"d\"},\"answer\":\"A\",\"explanation\":\"解析\"}"}],
    "max_tokens": 200, "temperature": 1.0, "stream": True,
}

req = urllib.request.Request(URL, data=json.dumps(body, ensure_ascii=False).encode(),
                             headers={"Content-Type": "application/json",
                                      "Accept-Encoding": "identity",
                                      "Authorization": "Bearer " + KEY})
raw = b""
with urllib.request.urlopen(req, timeout=60) as r:
    while True:
        chunk = r.read(1024)
        if not chunk:
            break
        raw += chunk
txt = raw.decode('utf-8', 'replace')
print("=== 原始 SSE (前 800 字符) ===")
print(txt[:800])
print("...")
print("=== 尾部 200 字符 ===")
print(txt[-200:])

# ---- 复刻设备端 sse_extract_content ----
out = []
found = 0
for line in txt.split("\n"):
    if line.startswith("data:"):
        found = 1
        js = line[5:].strip()
        if js.startswith("[DONE]"):
            break
        try:
            o = json.loads(js)
            ch = o["choices"][0]
            dc = ch.get("delta", {}).get("content")
            if dc:
                out.append(dc)
        except Exception as e:
            print("chunk parse fail:", e, js[:100])
print("=== 拼接结果 ===")
print("".join(out))
print("=== 提取 JSON 后的 content ===")
import re
s = "".join(out)
m = re.search(r'\{.*\}', s, re.S)
if m:
    q = json.loads(m.group(0))
    print("content:", q.get("content"))
    print("answer:", q.get("answer"))
    print("options:", q.get("options"))
