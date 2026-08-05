#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扩充 font_cn.c 字库: 为缺失的数学符号生成 16x16 点阵并按升序插入,
然后重新生成 cn_gray.c (调用 gen_font_cn.py)"""
import re, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

MAIN = '/media/user/6AF00DA6F00D7A19/esp32-tester/main'
SRC = MAIN + '/font_cn.c'
FONT_PATH = '/usr/share/fonts/truetype/ms-core-fonts/msyh.ttf'

# 完整上下标字符集 (防止知识库/题目出现其他上下标符号变方框):
# 上标数字+符号 ⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾, 下标数字+符号 ₀-₉ ₊₋₌₍₎,
# 上标小写拉丁 ᵃᵇᶜᵈᵉᶠᵍʰⁱʲᵏˡᵐⁿᵒᵖʳˢᵗᵘᵛʷˣʸᶻ, 下标小写拉丁 ₐₑₒₓₔₕₖₗₘₙₚₛₜ,
# 上标希腊 ᵝᵞᵟᵠᵡᵦᵧᵨᵩᵪ
NEW = sorted(set([
    # 上标数字/符号
    0x2070, 0x00B9, 0x00B2, 0x00B3,
    *range(0x2074, 0x207F),          # ⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾
    # 下标数字/符号
    *range(0x2080, 0x208F),          # ₀-₉ ₊₋₌₍₎
    # 上标小写拉丁
    0x1D43, 0x1D47, 0x1D48, 0x1D49, 0x1D4D, 0x1D4F, 0x1D50, 0x1D52,
    0x1D56, 0x1D57, 0x1D58, 0x1D5B, 0x1D9C, 0x1DA0, 0x1DBB,
    0x02B0, 0x02B2, 0x02B3, 0x02B7, 0x02B8, 0x02E1, 0x02E2, 0x02E3,
    0x2071,
    # 下标小写拉丁
    *range(0x2090, 0x209D),
    # 上标希腊
    0x1D5D, 0x1D5E, 0x1D5F, 0x1D60, 0x1D61,
    0x1D66, 0x1D67, 0x1D68, 0x1D69, 0x1D6A,
    # 拼音长音字母 + 分数 (AI 知识库实测缺字: ā ŷ ȳ ̄ ⅓ ⅘)
    0x0100, 0x0177, 0x0233, 0x0304, 0x2153, 0x2158,
]))

# ---------- 1) 解析现有字库 ----------
src = open(SRC, encoding='utf-8', errors='replace').read()
body = src[src.index('{') + 1: src.rindex('}')]
vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
count = vals[0] | (vals[1] << 8)
entries = []          # (code, 32 bytes)
i = 2
while i + 33 < len(vals):
    code = vals[i] | (vals[i + 1] << 8)
    entries.append((code, vals[i + 2:i + 34]))
    i += 34
assert len(entries) == count, (len(entries), count)
codes = [e[0] for e in entries]
assert codes == sorted(codes), "font_cn.c 未排序"
print(f"现有 {count} 字")

# ---------- 2) 渲染新字符 16x16 点阵 ----------
font = ImageFont.truetype(FONT_PATH, 16)

def render_bits(code):
    ch = chr(code)
    img = Image.new('L', (64, 64), 0)
    d = ImageDraw.Draw(img)
    d.text((8, 8), ch, font=font, fill=255)
    rows = []
    for row in range(16):
        line = 0
        for col in range(16):
            block = max(img.getpixel((col * 4 + x, row * 4 + y))
                        for x in range(4) for y in range(4))
            if block > 96:
                line |= 1 << (15 - col)
        rows.append(line)
    # 检查是否渲染出内容
    if max(rows) == 0:
        return None
    bs = []
    for r in rows:
        bs.append((r >> 8) & 0xFF)
        bs.append(r & 0xFF)
    return bs

new_entries = []
for code in NEW:
    bits = render_bits(code)
    if bits is None:
        print(f"警告: U+{code:04X} {chr(code)} 渲染空白, 字体缺字形, 跳过")
        continue
    new_entries.append((code, bits))
    print(f"U+{code:04X} {chr(code)} 点阵生成")

# ---------- 3) 合并排序 ----------
merged = entries + new_entries
merged.sort(key=lambda e: e[0])
# 去重
dedup = []
seen = set()
for e in merged:
    if e[0] in seen:
        continue
    seen.add(e[0])
    dedup.append(e)
print(f"合并后 {len(dedup)} 字")

# ---------- 4) 重写 font_cn.c ----------
out = ["#include \"font_cn.h\"",
       f"const uint16_t font_cn_count = {len(dedup)};",
       "const uint8_t font_cn_data[] = {"]
# 数组前 2 字节 = count (小端), ui_renderer 从偏移 2 开始读字形
out.append(f"    0x{len(dedup) & 0xFF:02X},0x{(len(dedup) >> 8) & 0xFF:02X},")
for code, bits in dedup:
    out.append(f"    0x{code & 0xFF:02X},0x{(code >> 8) & 0xFF:02X}," +
               ",".join(f"0x{b:02X}" for b in bits) + ",")
out.append("};")
open(SRC, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print(f"font_cn.c 已更新: {len(dedup)} 字")

# ---------- 5) 重新生成 cn_gray.c ----------
r = subprocess.run([sys.executable, MAIN + '/../tools/gen_font_cn.py'],
                   cwd='/media/user/6AF00DA6F00D7A19/esp32-tester/tools',
                   capture_output=True, text=True)
print("gen_font_cn:", r.stdout.strip()[-300:])
if r.returncode != 0:
    print("gen_font_cn stderr:", r.stderr[-500:])
