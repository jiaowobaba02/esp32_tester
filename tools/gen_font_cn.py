#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font_cn.py — 预渲染微软雅黑 16px 灰度中文字库 (3bit/像素, 8 级抗锯齿)

在 PC 端将 font_cn.c 点阵字库的全部字符用微软雅黑渲染为灰度位图,
按字形 bbox 裁剪 + 3bit 量化打包, 供 ESP32 端 ui_renderer Alpha 混合绘制。

输出: main/cn_gray.c/.h

格式 (cn_gray_glyph_t, 8 字节/字, 按 code 升序):
  code   : unicode
  bitoff : 数据区 bit 偏移 (24bit)
  w,h    : 位图宽高
  xoff   : 相对 16x16 格左上角的 x 偏移
  yoff   : 相对 16x16 格左上角的 y 偏移
数据区: 每像素 3bit (0-7, 低位优先), 值*255/7 = alpha
未收录字符 (字体缺字形/渲染空白) 自动跳过, 设备端回退 1-bit 点阵。
"""
import os, re
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/ms-core-fonts/msyh.ttf"
FONT_SIZE = 16
SRC = os.path.join(os.path.dirname(__file__), "..", "main", "font_cn.c")
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "main")

# ---------- 1) 提取 font_cn.c 的字符集 (已按 unicode 升序) ----------
src = open(SRC, encoding="utf-8", errors="replace").read()
body = src[src.index("{") + 1: src.rindex("}")]
vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
codes = []
i = 2
while i + 33 < len(vals):
    codes.append(vals[i] | (vals[i + 1] << 8))
    i += 34
assert codes == sorted(codes), "font_cn.c 未按 unicode 排序"
print("charset:", len(codes), "chars")

# ---------- 2) 渲染 + 打包 ----------
font = ImageFont.truetype(FONT_PATH, FONT_SIZE)


def render_char(code):
    ch = chr(code)
    img = Image.new("L", (64, 64), 0)
    d = ImageDraw.Draw(img)
    d.text((8, 8), ch, font=font, fill=255)
    bb = img.getbbox()
    if bb is None:
        return None
    l, t, r, b = bb
    crop = img.crop(bb)
    data = list(crop.getdata())
    if max(data) < 32:                     # 渲染空白 = 字体缺字形
        return None
    # 2bit 量化 (4 级灰度, 值*85=alpha)
    px = [((v * 3 + 127) // 255) if v >= 32 else 0 for v in data]
    return (r - l, b - t, l - 8, t - 8, px)


meta = []        # (code, w, h, xoff, yoff, px)
skipped = []
for code in codes:
    r = render_char(code)
    if r is None:
        skipped.append(code)
        continue
    w, h, xoff, yoff, px = r
    meta.append((code, w, h, xoff, yoff, px))
print("rendered:", len(meta), "skipped:", len(skipped),
      "".join(chr(c) for c in skipped[:20]))

# ---------- 3) 打包数据 (3bit/像素, 低位优先) ----------
data_bits = []
for code, w, h, xoff, yoff, px in meta:
    for v in px:
        data_bits.append(v & 7)

data_bytes = bytearray()
i = 0
while i < len(data_bits):                  # 4 像素 = 8bit = 1 字节
    b = 0
    for j in range(4):
        if i + j < len(data_bits):
            b |= data_bits[i + j] << (j * 2)
    data_bytes.append(b)
    i += 4
data_bytes += b"\x00"                      # 读取越界保护

# 像素偏移表 (2bit/像素)
bitoff = []
bo = 0
for code, w, h, xoff, yoff, px in meta:
    bitoff.append(bo)
    bo += len(px)
assert bo == len(data_bits), (bo, len(data_bits))

# ---------- 4) 输出 C ----------
name = "cn_gray"
hdr = [
    "#ifndef CN_GRAY_H",
    "#define CN_GRAY_H",
    "#include <stdint.h>",
    "",
    "/* 16px 微软雅黑灰度字形 (2bit/像素, 4 级抗锯齿, 按 bbox 裁剪) */",
    "typedef struct {",
    "    uint16_t code;",
    "    uint32_t pixoff : 24;   /* 数据区像素偏移 (2bit/像素) */",
    "    uint8_t w, h;",
    "    int8_t xoff, yoff;      /* 相对 16x16 格左上角 */",
    "} cn_gray_glyph_t;",
    "extern const uint16_t cn_gray_count;",
    "extern const cn_gray_glyph_t cn_gray[];",
    "extern const uint8_t cn_gray_data[];",
    "#endif",
]
open(os.path.join(OUT_DIR, name + ".h"), "w").write("\n".join(hdr) + "\n")

c = []
c.append('#include "%s.h"' % name)
c.append("")
c.append("const uint16_t cn_gray_count = %d;" % len(meta))
c.append("")
c.append("/* 2bit 打包数据 (低位优先, 值*85=alpha) */")
c.append("const uint8_t cn_gray_data[] = {")
for i in range(0, len(data_bytes), 16):
    chunk = data_bytes[i:i + 16]
    c.append("    " + ",".join("0x%02X" % x for x in chunk) + ",")
c.append("};")
c.append("")
c.append("const cn_gray_glyph_t cn_gray[] = {")
for (code, w, h, xoff, yoff, px), bo in zip(meta, bitoff):
    c.append("    { 0x%04X, %d, %d, %d, %d, %d }," % (code, bo, w, h, xoff, yoff))
c.append("};")
open(os.path.join(OUT_DIR, name + ".c"), "w").write("\n".join(c) + "\n")

kb = len(data_bytes) / 1024
kb2 = len(meta) * 8 / 1024
print("data: %.0f KB, table: %.0f KB, total: %.0f KB" % (kb, kb2, kb + kb2))
