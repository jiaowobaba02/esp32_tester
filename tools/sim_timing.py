#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""计时实验: 主机发慢生成请求(填空,max_tokens 2000), 看耗时是否 >15s 且成功"""
import json, sys, time, urllib.request, urllib.error
sys.path.insert(0, '/media/user/6AF00DA6F00D7A19/esp32-tester/tools')
from sim_quiz2 import build_body, call_api, topic_pool

for i in range(3):
    topic = topic_pool["数学"][(i * 7) % 22]
    body = build_body("数学", topic, True)
    t0 = time.time()
    raw = call_api(body)
    dt = time.time() - t0
    status = "OK" if not raw.startswith(("HTTP_ERROR", "NET_ERROR")) else raw[:60]
    print(f"请求 {i+1} [{topic}] 耗时 {dt:.1f}s -> {status}", flush=True)
