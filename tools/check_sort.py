#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""调试: 解析 font_cn.c 并打印关键信息"""
import re

src = open('/media/user/6AF00DA6F00D7A19/esp32-tester/main/font_cn.c', encoding='utf-8').read()
start = src.index('{')
end = src.rindex('}')
body = src[start + 1:end]
print("body 长度:", len(body))
print("body 前 60 字符:", body[:60])
vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
print("vals 数量:", len(vals))
print("vals 前 8:", vals[:8])
codes = []
i = 2
while i + 33 < len(vals):
    codes.append(vals[i] | (vals[i + 1] << 8))
    i += 34
print("解析字数:", len(codes))
print("codes 前 8:", codes[:8])
bad = [(codes[k], codes[k + 1]) for k in range(len(codes) - 1) if codes[k] >= codes[k + 1]]
print("无序对数:", len(bad), "样本:", bad[:3])
