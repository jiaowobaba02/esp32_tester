#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""font_cn.c 字符分布统计"""
import re
from collections import Counter

src = open('/media/user/6AF00DA6F00D7A19/esp32-tester/main/font_cn.c', encoding='utf-8', errors='replace').read()
body = src[src.index('{') + 1: src.rindex('}')]
vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
codes = []
i = 0
while i + 33 < len(vals):
    codes.append(vals[i] | (vals[i + 1] << 8))
    i += 34

buckets = Counter()
for c in codes:
    if c < 0x80:
        buckets['ASCII'] += 1
    elif c < 0x2000:
        buckets['标点/符号'] += 1
    elif c < 0x3000:
        buckets['CJK符号'] += 1
    elif c < 0x4E00:
        buckets['日文假名等'] += 1
    elif c < 0x9FFF:
        buckets['汉字'] += 1
    elif c < 0xFF00:
        buckets['希腊/数学符号'] += 1
    else:
        buckets['全角'] += 1
for k, v in buckets.items():
    print(f"{k}: {v}")
han = sorted(c for c in codes if 0x4E00 <= c < 0x9FFF)
print("汉字样本:", ''.join(chr(c) for c in han[:30]))
