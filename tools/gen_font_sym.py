#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font_sym.py — 预渲染 Arial 特殊符号灰度字库 (16/24/32px) 为 C 数组

数学/物理/化学符号 (×÷±√π∞°²³ 等), 与 gen_font.py 同字形格式:
  输出 main/sym_gray16.c/.h, sym_gray24.c/.h, sym_gray32.c/.h
code 数组按码点升序 (C 端二分查找); 缺字形自动跳过并告警。
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"  # Arial 无下标字形, DejaVu 全覆
SIZES = [16, 24, 32]
SYMBOLS = ("×÷±√π∞°²³¹₂₃↑↓αβγΔΩµ∑∫≤≥≠≈→←½¼"   # 数学/化学/物理
           "₀₁₄₅₆₇₈₉₊₋₌₍₎"                        # 下标数字与符号
           "ₐₑₒₓₔₕₖₗₘₙₚₛₜ"                        # 下标字母
           "⁰⁴⁵⁶⁷⁸⁹⁺⁻⁽⁾"                          # 上标数字/离子电荷 (Fe³⁺ SO₄²⁻ H⁺)
           "·⇌℃"                                  # 结晶水 / 可逆反应 / 摄氏度
           "∠⊥∥∂"                                  # 几何/微积分
           "¾⅓⅔"                                  # 分数
           "０１２３４５６７８９"                    # 全角数字 (AI 输出兜底)
           "θλφψωεηστν"                            # 希腊字母补充 (物理/数学)
           "★☆※"                                  # 装饰
           "①②③④⑤⑥⑦⑧⑨⑩")                      # 圈数字 (AI 列表)
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "main")


def render_glyph(font, ch):
    """与 gen_font.py 完全一致的渲染参数, 返回 (w,h,xoff,yoff,adv,px2bit)
    px2bit: 每像素 2bit (0-3, 4 级灰度, 值*85=alpha), 与 cn_gray 同格式"""
    ascent, _ = font.getmetrics()
    tmp = Image.new("L", (512, 512), 0)
    d = ImageDraw.Draw(tmp)
    d.text((8, 8), ch, fill=255, font=font)
    bbox = tmp.getbbox()
    adv = int(round(font.getlength(ch)))
    if bbox is None:
        return None                       # 缺字形
    l, t, r, b = bbox
    img = tmp.crop(bbox)
    data = list(img.getdata())
    if max(data) < 32:
        return None                       # 渲染空白 = 缺字形
    px = [((v * 3 + 127) // 255) if v >= 32 else 0 for v in data]
    return (r - l, b - t, l - 8, t - 8 - ascent, adv, px)


def emit_c(name, size, items):
    """items: [(code, glyph)] 已按 code 升序"""
    codes = [c for c, _ in items]
    n = len(items)
    hdr = [
        "#ifndef %s_H" % name.upper(),
        "#define %s_H" % name.upper(),
        "#include <stdint.h>",
        "",
        "/* %dpx Arial 特殊符号灰度字形 (码点升序, C 端二分查找) */" % size,
        "#ifndef GRAY_GLYPH_T_DEFINED",
        "#define GRAY_GLYPH_T_DEFINED",
        "typedef struct {",
        "    uint8_t w, h;",
        "    int8_t xoff;",
        "    int8_t yoff;",
        "    uint8_t adv;",
        "    const uint8_t *px;",
        "} gray_glyph_t;\n#endif",
        "#define %s_count %d" % (name, n),
        "extern const uint16_t %s_codes[%d];" % (name, n),
        "extern const gray_glyph_t %s[%d];" % (name, n),
        "#endif",
    ]
    open(os.path.join(OUT_DIR, name + ".h"), "w").write("\n".join(hdr) + "\n")

    c = ['#include "%s.h"' % name, "",
         "const uint16_t %s_codes[%d] = {" % (name, n)]
    for i in range(0, n, 8):
        c.append("    " + ",".join("0x%04X" % x for x in codes[i:i + 8]) + ",")
    c.append("};")
    c.append("")
    c.append("/* 2bit 打包数据 (低位优先, 4 像素/字节, 值*85=alpha; 与 cn_gray 同格式) */")
    c.append("static const uint8_t %s_data[] = {" % name)
    for code, (w, h, xoff, yoff, adv, data) in items:
        c.append("/* U+%04X %s  %dx%d  adv=%d */" % (code, chr(code), w, h, adv))
        for j in range(0, len(data), 4):
            v = 0
            for k in range(4):
                v |= (data[j + k] & 3) << (k * 2) if j + k < len(data) else 0
            c.append("    0x%02X," % v)
    c.append("};")
    c.append("")
    c.append("const gray_glyph_t %s[%d] = {" % (name, n))
    off = 0
    for code, (w, h, xoff, yoff, adv, data) in items:
        c.append("    { %d, %d, %d, %d, %d, &%s_data[%d] }," % (
            w, h, xoff, yoff, adv, name, off))
        off += (len(data) + 3) // 4
    c.append("};")
    open(os.path.join(OUT_DIR, name + ".c"), "w").write("\n".join(c) + "\n")
    return off


total = 0
for size in SIZES:
    font = ImageFont.truetype(FONT_PATH, size)
    items = []
    for ch in SYMBOLS:
        g = render_glyph(font, ch)
        if g is None:
            print("WARN: U+%04X %s 无字形 (size %d)" % (ord(ch), ch, size))
            continue
        items.append((ord(ch), g))
    items.sort(key=lambda x: x[0])        # 升序, C 端二分查找要求
    name = "sym_gray%d" % size
    nbytes = emit_c(name, size, items)
    total += nbytes
    print("%s: %d glyphs, %d bytes" % (name, len(items), nbytes))
print("total: %d bytes (~%.1f KB)" % (total, total / 1024.0))
