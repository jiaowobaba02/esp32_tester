#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""诊断: 打印填空响应的 finish_reason + 完整 content, 定位 JSON 损坏原因"""
import json, sys, urllib.request, urllib.error
sys.path.insert(0, '/media/user/6AF00DA6F00D7A19/esp32-tester/tools')
from sim_quiz import build_body, call_api, topic_pool

subject = sys.argv[1] if len(sys.argv) > 1 else "数学"
topic = sys.argv[2] if len(sys.argv) > 2 else "三角恒等变换"
body = build_body(subject, topic, True)
raw = call_api(body)
try:
    root = json.loads(raw)
    cstr = root["choices"][0]["message"]["content"]
    fr = root["choices"][0].get("finish_reason")
    usage = root.get("usage")
    print(f"finish_reason = {fr}")
    print(f"usage = {usage}")
    print(f"content len = {len(cstr)}")
    print("---- content (前1200字符) ----")
    print(cstr[:1200])
    print("---- content (尾部120字符) ----")
    print(cstr[-120:])
except Exception as e:
    print("root parse fail:", e)
    print(raw[:500])
