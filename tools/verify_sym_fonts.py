#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证生成的符号字库: 码点升序/无重复/关键码点覆盖/偏移一致性"""
import re
import sys

ok = True
for sz in (16, 24, 32):
    txt = open(f'main/sym_gray{sz}.c', encoding='utf-8').read()
    m = re.search(r'sym_gray%d_codes\[\d+\] = \{(.*?)\};' % sz, txt, re.S)
    codes = [int(x, 16) for x in re.findall(r'0x[0-9A-F]{4}', m.group(1))]
    if codes != sorted(codes):
        print(f"sym_gray{sz}: 未排序!"); ok = False
    if len(codes) != len(set(codes)):
        print(f"sym_gray{sz}: 有重复!"); ok = False
    need = [0x2082, 0x207A, 0x207B, 0x21CC, 0x00B7, 0x2103, 0x2074, 0xFF10, 0x03B8, 0x2460]
    missing = [hex(c) for c in need if c not in codes]
    gs = re.findall(r'\{ \d+, \d+, -?\d+, -?\d+, \d+, &sym_gray%d_data\[(\d+)\] \}' % sz, txt)
    offs = [int(g) for g in gs]
    mono = all(offs[i] <= offs[i + 1] for i in range(len(offs) - 1))
    print(f"sym_gray{sz}: {len(codes)} glyphs 排序={'ok' if codes==sorted(codes) else 'BAD'} "
          f"唯一={'ok' if len(codes)==len(set(codes)) else 'BAD'} 偏移单调={mono} "
          f"缺失关键码点: {missing if missing else '无'}")
    if missing or not mono:
        ok = False
print("ALL OK" if ok else "HAS PROBLEMS")
sys.exit(0 if ok else 1)
