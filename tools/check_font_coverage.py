#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统计 font_cn.c 字库字符集 + 与知识库文本对比找缺字"""
import re, sys

# 1) 读 font_cn.c 字符集
src = open('/media/user/6AF00DA6F00D7A19/esp32-tester/main/font_cn.c', encoding='utf-8', errors='replace').read()
body = src[src.index('{') + 1: src.rindex('}')]
vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
# 结构: [count(2)] + 每字 34 字节 = code(2, 小端) + 32 点阵
codes = []
i = 2
while i + 33 < len(vals):
    code = vals[i] | (vals[i + 1] << 8)
    codes.append(code)
    i += 34
n_count = int(re.search(r'font_cn_count\s*=\s*(\d+)', src).group(1))
print(f"font_cn.c: 声明 count={n_count}, 实际解析 {len(codes)} 字")
print(f"覆盖范围: U+{min(codes):04X} .. U+{max(codes):04X}")
cn = sorted(c for c in codes if c >= 0x4E00)
print(f"汉字数: {len(cn)}")

# 2) 对比文本找缺字
if len(sys.argv) > 1:
    text = open(sys.argv[1], encoding='utf-8').read()
    charset = set(codes)
    missing = sorted({ord(ch) for ch in text if ord(ch) >= 0x80 and ord(ch) not in charset and ord(ch) != 0xFFFD})
    print(f"文本共 {len(text)} 字符, 缺失 {len(missing)} 个:")
    for m in missing:
        print(f"  U+{m:04X} {chr(m)}", end='')
        print()
