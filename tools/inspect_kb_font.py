#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 weak 分区 dump 提取数学科知识库文本, 对照 font_cn.c 字库找缺字"""
import re, sys

# 1) 字库字符集
src = open('/media/user/6AF00DA6F00D7A19/esp32-tester/main/font_cn.c', encoding='utf-8', errors='replace').read()
body = src[src.index('{') + 1: src.rindex('}')]
vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
codes = []
i = 2
while i + 33 < len(vals):
    codes.append(vals[i] | (vals[i + 1] << 8))
    i += 34
charset = set(codes)

# 2) 解析 dump: 知识库区 [16384..131072), 7 槽 × 16384
dump = open('/tmp/weak_subj0.bin', 'rb').read()
WEAK_KB_OFF = 16384
WEAK_KB_SLOT = 16384
WEAK_KB_NAME_SZ = 64

texts = []
for slot in range(7):
    pos = WEAK_KB_OFF + slot * WEAK_KB_SLOT
    name = dump[pos:pos + WEAK_KB_NAME_SZ].split(b'\x00')[0]
    content = dump[pos + WEAK_KB_NAME_SZ:pos + WEAK_KB_SLOT].split(b'\x00')[0]
    if name:
        try:
            texts.append((name.decode('utf-8', 'replace'), content.decode('utf-8', 'replace')))
        except Exception:
            pass

print(f"知识库槽: {len(texts)} 个")
all_missing = {}
for name, content in texts:
    missing = sorted({ord(ch) for ch in content if ord(ch) >= 0x80 and ord(ch) not in charset and ord(ch) != 0xFFFD})
    all_missing[name] = missing
    print(f"--- [{name}] 内容 {len(content)} 字, 缺 {len(missing)} 字符 ---")
    if missing:
        shown = ''.join(chr(m) for m in missing)
        print(f"  缺字: {shown}")
        print(f"  码位: {[hex(m) for m in missing]}")

# 汇总所有缺字
merged = sorted(set(m for v in all_missing.values() for m in v))
print(f"\n全部缺字 {len(merged)} 个:")
print(''.join(chr(m) for m in merged))
print([hex(m) for m in merged])
