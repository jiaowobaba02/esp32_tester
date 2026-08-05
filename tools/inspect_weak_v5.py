#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 v5 布局 weakness dump: 错题迁移结果 + 生物错题原始字节"""
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/weak_v5_dump.bin', 'rb').read()
subjects = ["数学", "物理", "化学", "生物", "英语", "语文", "历史", "政治", "地理"]
SLOT = 131072

# 1) 旧区域 (0..0x90000) 是否已擦除 (迁移后应全 0xFF)
old = data[:0x90000]
ff = sum(1 for b in old if b == 0xFF)
print(f"旧区域 0..0x90000: 0xFF 占比 {ff * 100 // len(old)}%")

# 2) 各科错题
for si in range(9):
    base = si * SLOT
    wrongs = []
    for i in range(20):
        pos = base + 4 + i * 128
        ln = int.from_bytes(data[pos:pos + 4], 'little')
        if ln == 0 or ln == 0xFFFFFFFF or ln > 124:
            break
        wrongs.append(data[pos + 4:pos + 4 + ln])
    print(f"[{subjects[si]}] 错题={len(wrongs)}")

# 3) 生物科错题原始字节 (检查 UTF-8 完整性)
if len(wrongs) or True:
    base = 3 * SLOT
    for i in range(2):
        pos = base + 4 + i * 128
        ln = int.from_bytes(data[pos:pos + 4], 'little')
        if ln == 0 or ln == 0xFFFFFFFF:
            break
        body = data[pos + 4:pos + 4 + ln]
        # 检查末尾字节是否完整 UTF-8
        b = body[-1]
        ok_end = True
        back = 0
        if b & 0xC0 == 0x80:      # 以续字节结尾 = 被截断
            ok_end = False
            for j in range(len(body) - 1, max(-1, len(body) - 4), -1):
                if data[pos + 4 + j] & 0xC0 == 0x80:
                    back += 1
                else:
                    break
        elif b >= 0xC0:            # 以首字节结尾 = 被截断
            ok_end = False
        print(f"  生物错题{i}: len={ln} 末字节=0x{b:02X} utf8完整结尾={ok_end} "
              f"含换行={'\\n' in body.decode('utf-8','ignore')} 末尾12字节={body[-12:].hex()}")

# 4) KB 槽: 应全空 (旧乱码数据已擦)
for si in range(9):
    base = si * SLOT
    names = []
    for k in range(7):
        nm = data[base + 16384 + k * 16384: base + 16384 + k * 16384 + 64]
        ne = nm.find(b'\0')
        name = nm[:ne].decode('utf-8', 'ignore') if ne > 0 else ''
        if name:
            names.append(name)
    if names:
        print(f"[{subjects[si]}] KB 残留: {names}")

# 5) AI 区
for si in range(9):
    ai = data[si * SLOT + 8192: si * SLOT + 16384]
    ff = sum(1 for b in ai if b == 0xFF)
    if ff < len(ai):
        print(f"[{subjects[si]}] AI 区有数据 0xFF占比 {ff * 100 // len(ai)}%")
