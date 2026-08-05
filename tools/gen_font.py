#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py — 预渲染 Arial 灰度字库 (16/24/32px) 为 C 数组

在 PC 端用 PIL 将 Arial 渲染为带 Alpha (抗锯齿) 的位图,
供 ESP32 端 ui_renderer 做 Alpha 混合绘制 (RGB565)。

输出: main/ascii_gray16.c/.h, ascii_gray24.c/.h, ascii_gray32.c/.h

字形格式 (gray_glyph_t):
  w,h     : 位图宽高 (像素)
  xoff    : 字形左上角相对笔位 x 的偏移
  yoff    : 字形左上角相对基线的偏移 (负=基线上方)
  adv     : 前进量 (下一字符笔位增量)
  px      : w*h 字节 Alpha (0=全透明 255=不透明)
基线约定: 调用者传文本顶 y, 基线 = y + ascent
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/msttcorefonts/Arial.TTF"
SIZES = [16, 24, 32]
CHARS = [chr(c) for c in range(0x20, 0x7F)]   # 0x20 ' ' .. 0x7E '~'
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "main")


def render_glyph(font, ch):
    """返回 (w, h, xoff, yoff, adv, px2bit)
    px2bit: 每像素 2bit (0-3, 4 级灰度, 值*85=alpha), 与 cn_gray 同格式"""
    ascent, descent = font.getmetrics()
    tmp = Image.new("L", (512, 512), 0)
    d = ImageDraw.Draw(tmp)
    d.text((8, 8), ch, fill=255, font=font)
    bbox = tmp.getbbox()
    adv = int(round(font.getlength(ch)))
    if bbox is None:                       # 空白字形 (空格)
        return (1, 1, 0, 0, adv, [0])
    l, t, r, b = bbox
    img = tmp.crop(bbox)
    data = list(img.getdata())
    if max(data) < 32:                     # 渲染空白 = 空格类字形
        return (1, 1, 0, 0, adv, [0])
    px = [((v * 3 + 127) // 255) if v >= 32 else 0 for v in data]
    return (r - l, b - t, l - 8, t - 8 - ascent, adv, px)


def emit_c(name, size, glyphs):
    hdr = []
    hdr.append("#ifndef %s_H" % name.upper())
    hdr.append("#define %s_H" % name.upper())
    hdr.append("#include <stdint.h>")
    hdr.append("")
    hdr.append("/* %dpx Arial 灰度字形: w/h 位图尺寸, xoff/yoff 偏移, adv 前进量 */" % size)
    hdr.append("#ifndef GRAY_GLYPH_T_DEFINED")
    hdr.append("#define GRAY_GLYPH_T_DEFINED")
    hdr.append("typedef struct {")
    hdr.append("    uint8_t w, h;")
    hdr.append("    int8_t xoff;")
    hdr.append("    int8_t yoff;")
    hdr.append("    uint8_t adv;")
    hdr.append("    const uint8_t *px;")
    hdr.append("} gray_glyph_t;\n#endif")
    hdr.append("#define %s_ascent %d" % (name, ascent))
    hdr.append("extern const gray_glyph_t %s[95];" % name)
    hdr.append("#endif")
    open(os.path.join(OUT_DIR, name + ".h"), "w").write("\n".join(hdr) + "\n")

    c = []
    c.append('#include "%s.h"' % name)
    c.append("")
    c.append("/* 2bit 打包数据 (低位优先, 4 像素/字节, 值*85=alpha; 与 cn_gray 同格式) */")
    c.append("static const uint8_t %s_data[] = {" % name)
    for i, (w, h, xoff, yoff, adv, data) in enumerate(glyphs):
        c.append("/* 0x%02X '%s'  %dx%d  adv=%d */" % (
            CHARS[i].encode("ascii", "replace")[0], CHARS[i],
            w, h, adv) if CHARS[i] != " " else
            "/* 0x20 ' '  %dx%d  adv=%d */" % (w, h, adv))
        for j in range(0, len(data), 4):
            v = 0
            for k in range(4):
                v |= (data[j + k] & 3) << (k * 2) if j + k < len(data) else 0
            c.append("    0x%02X," % v)
    c.append("};")
    c.append("")
    # ascent 用宏定义 (extern const 非编译期常量, 不能用于静态数组初始化)
    c.append("const gray_glyph_t %s[95] = {" % name)
    off = 0
    for i, (w, h, xoff, yoff, adv, data) in enumerate(glyphs):
        c.append("    { %d, %d, %d, %d, %d, &%s_data[%d] }," % (
            w, h, xoff, yoff, adv, name, off))
        off += (len(data) + 3) // 4
    c.append("};")
    open(os.path.join(OUT_DIR, name + ".c"), "w").write("\n".join(c) + "\n")
    return off


total = 0
for size in SIZES:
    font = ImageFont.truetype(FONT_PATH, size)
    ascent, descent = font.getmetrics()
    glyphs = [render_glyph(font, ch) for ch in CHARS]
    name = "ascii_gray%d" % size
    nbytes = emit_c(name, size, glyphs)
    total += nbytes
    # 抗锯齿自检: 存在中间 alpha 值
    mid = sum(1 for g in glyphs for b in g[5] if 0 < b < 255)
    print("%s: %d glyphs, %d bytes alpha, mid-alpha px=%d, ascent=%d descent=%d" %
          (name, len(glyphs), nbytes, mid, ascent, descent))
print("total alpha bytes: %d (~%.1f KB)" % (total, total / 1024.0))
