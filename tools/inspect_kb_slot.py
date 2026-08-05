#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查某科知识库槽位内容 (主题名/长度/结尾 UTF-8 完整性/截断迹象)"""
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/weak_check.bin', 'rb').read()
subj = int(sys.argv[2]) if len(sys.argv) > 2 else 3
subjects = ["数学", "物理", "化学", "生物", "英语", "语文", "历史", "政治", "地理"]
base = subj * 131072
print(f"=== {subjects[subj]}科 知识库槽位 ===")
for k in range(7):
    p = base + 16384 + k * 16384
    nm = data[p:p + 64]
    ne = nm.find(b'\0')
    name = nm[:ne].decode('utf-8', 'ignore') if ne > 0 else ''
    if not name:
        continue
    body = data[p + 64:p + 16384]
    ce = body.find(b'\0')
    ln = ce if ce >= 0 else len(body)
    content = body[:ln]
    b = content[-1] if content else 0
    ok = True
    if b >= 0x80 and (b & 0xC0 == 0x80 or b >= 0xC0):
        ok = False
    print(f"槽{k}: 主题={name!r} 长度={ln}B 末字节=0x{b:02X} utf8完整={ok}")
    print(f"  开头: {content[:70].decode('utf-8', 'replace')!r}")
    print(f"  结尾: {content[-70:].decode('utf-8', 'replace')!r}")
