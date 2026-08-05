#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 weakness 分区 dump: 错题/AI 分析/知识库槽位, 检测截断迹象"""
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/weakness_dump.bin', 'rb').read()
subjects = ["数学", "物理", "化学", "生物", "英语", "语文", "历史", "政治", "地理"]
SLOT = 65536
KB_OFF, KB_SLOT, KB_NAME = 8192, 16384, 64
AI_OFF, AI_SIZE = 4096, 4096


def utf8_ok_end(b):
    """返回 (末尾 UTF-8 是否完整, 需回退字节数)"""
    n = 0
    for i in range(len(b) - 1, max(-1, len(b) - 4), -1):
        c = b[i]
        if c < 0x80:
            break
        if c & 0xC0 == 0x80:
            n += 1
            continue
        if c >= 0xF0:
            need = 3
        elif c >= 0xE0:
            need = 2
        elif c >= 0xC0:
            need = 1
        else:
            need = 99
        return (n == need, n)
    return (True, 0)


def end_char_ok(s):
    if not s:
        return True
    if s[-1] in '。！？…；：:""\n ':
        return True
    if s[-1].isalnum() or s[-1] in '）】》%':
        return True
    return False


print("=== 各科薄弱点区 ===")
for si in range(9):
    base = si * SLOT
    wrongs = []
    for i in range(20):
        pos = base + 4 + i * (4 + 124)
        ln = int.from_bytes(data[pos:pos + 4], 'little')
        if ln == 0 or ln == 0xFFFFFFFF or ln > 124:
            break
        wrongs.append(data[pos + 4:pos + 4 + ln].decode('utf-8', errors='replace'))
    ai = data[base + AI_OFF:base + AI_OFF + AI_SIZE]
    ai_end = ai.find(b'\0')
    if ai_end < 0:
        ai_end = AI_SIZE
    ai_txt = ai[:ai_end].decode('utf-8', errors='replace')
    ok, back = utf8_ok_end(ai[:ai_end])
    print(f"[{subjects[si]}] 错题={len(wrongs)} AI区={len(ai_txt)}B "
          f"utf8完整={ok} 末字符={ai_txt[-1:]!r} 长度异常={'!' if len(ai_txt) > 4000 else ''}")
    kbs = []
    for k in range(3):
        p = base + KB_OFF + k * KB_SLOT
        nm = data[p:p + KB_NAME]
        ne = nm.find(b'\0')
        if ne < 0:
            ne = 0
        name = nm[:ne].decode('utf-8', errors='replace')
        if not name:
            continue
        content = data[p + KB_NAME:p + KB_SLOT]
        ce = content.find(b'\0')
        if ce < 0:
            ce = len(content)
        body = content[:ce]
        ok2, back2 = utf8_ok_end(body)
        txt = body.decode('utf-8', errors='replace')
        kbs.append((name, len(body), body[-1:] if body else b'', ok2, back2, end_char_ok(txt)))
    for name, ln, last, ok2, back2, eok in kbs:
        flag = []
        if ln >= 16300:
            flag.append('⚠接近槽上限')
        if not ok2:
            flag.append(f'⚠UTF-8截断(回退{back2})')
        if not eok:
            flag.append(f'⚠结尾异常末字符={last!r}')
        print(f"   KB '{name}' len={ln}B 末字节={last} utf8={ok2} 结尾正常={eok} {' '.join(flag)}")
    for w in wrongs[:3]:
        print(f"     错题样例: {w[:50]}")
