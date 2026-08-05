#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 weakness dump 的 AI 区是否已使用 (全 0xFF = 未使用)"""
data = open('/tmp/weakness_dump.bin', 'rb').read()
for si in range(9):
    ai = data[si * 65536 + 4096: si * 65536 + 8192]
    ff = sum(1 for b in ai if b == 0xFF)
    print(f'科{si}: AI区 0xFF占比 {ff * 100 // len(ai)}% 首16字节={ai[:16].hex()}')
