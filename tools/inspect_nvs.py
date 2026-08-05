#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 NVS dump 中的 kb_ver / settings 命名空间"""
import re
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/nvs_dump.bin', 'rb').read()
for m in re.finditer(rb'kb_ver', data):
    print('found "kb_ver" at', hex(m.start()))
for m in re.finditer(rb'settings', data):
    print('found "settings" at', hex(m.start()))
# NVS 条目格式: [ns(1) type(1) span(1) chunk(1) data... key...] 粗略解析
# 在 kb_ver 附近找 int32 值 (data 区在 key 前)
for m in re.finditer(rb'kb_ver', data):
    p = m.start()
    # key 字符串前面是 data (最多 8 字节对齐), 往前找 ns/type
    ctx = data[p - 16:p + 8]
    print(f'  ctx before: {ctx.hex()}')
