#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""主机复现实验: TLS 1.2 + 设备同款请求头, 看 DeepSeek 是否拒绝"""
import json, ssl, sys, time, urllib.request, urllib.error

KEY = open('/media/user/6AF00DA6F00D7A19/esp32-tester/deepseek_api_key.txt').read().strip()
URL = "https://api.deepseek.com/chat/completions"

body = json.dumps({
    "model": "deepseek-chat",
    "messages": [
        {"role": "system", "content": "你是一名高中出题老师。只输出 JSON，不要输出任何其他文字。"},
        {"role": "user", "content": "请为高中数学出一道中等选择题，围绕知识点「函数零点」。严格按 JSON 输出：{\"content\":\"题目\",\"options\":{\"A\":\"a\",\"B\":\"b\",\"C\":\"c\",\"D\":\"d\"},\"answer\":\"A\",\"explanation\":\"解析\"}"}],
    "max_tokens": 1200, "temperature": 0.8,
}, ensure_ascii=False).encode()

def run(tag, ctx, headers):
    req = urllib.request.Request(URL, data=body, headers=headers)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=60, context=ctx) as r:
            d = r.read()
            dt = time.time() - t0
            print(f"[{tag}] OK {dt:.1f}s {len(d)} bytes", flush=True)
    except Exception as e:
        print(f"[{tag}] FAIL {time.time()-t0:.1f}s: {e}", flush=True)

# 1) TLS 1.2 (设备同款版本)
ctx12 = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx12.minimum_version = ssl.TLSVersion.TLSv1_2
ctx12.maximum_version = ssl.TLSVersion.TLSv1_2
ctx12.check_hostname = True
ctx12.load_default_certs()

hdr_esp = {
    "Content-Type": "application/json",
    "Accept-Encoding": "identity",
    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36",
    "Authorization": "Bearer " + KEY,
}
hdr_plain = {
    "Content-Type": "application/json",
    "Authorization": "Bearer " + KEY,
}

run("TLS1.2+设备头", ctx12, hdr_esp)
run("TLS1.2+默认头", ctx12, hdr_plain)
# 3) 默认 TLS (1.3) + 设备头
ctx13 = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx13.check_hostname = True
ctx13.load_default_certs()
run("TLS默认+设备头", ctx13, hdr_esp)
