#!/usr/bin/env python3
# Rasterize printable ASCII (32..126) from a monospace TTF into an 8x16 bitmap
# font and emit a C header. Each glyph = 16 rows x 1 byte; bit (0x80>>x) = pixel.
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
TTF = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
font = ImageFont.truetype(TTF, 15)

def glyph_bits(ch):
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    # DejaVu Mono advance ~ 0.6em; nudge to fit an 8px cell, baseline near row 12.
    d.text((0, -1), ch, fill=255, font=font)
    rows = []
    px = img.load()
    for y in range(H):
        b = 0
        for x in range(W):
            if px[x, y] > 110:
                b |= (0x80 >> x)
        rows.append(b)
    return rows

lines = []
lines.append("#ifndef FONT8X16_H")
lines.append("#define FONT8X16_H")
lines.append("// 8x16 bitmap font for printable ASCII 32..126, generated from")
lines.append("// DejaVu Sans Mono. font8x16[c-32][row], bit (0x80>>col) = pixel on.")
lines.append("#define FONT_W 8")
lines.append("#define FONT_H 16")
lines.append("#define FONT_FIRST 32")
lines.append("#define FONT_LAST 126")
lines.append("static const unsigned char font8x16[95][16] = {")
for code in range(32, 127):
    rows = glyph_bits(chr(code))
    hexrow = ",".join("0x%02x" % b for b in rows)
    disp = chr(code) if code != 92 else "backslash"
    lines.append("  {%s}, // %d '%s'" % (hexrow, code, disp if code not in (32,) else "space"))
lines.append("};")
lines.append("#endif")
open("/home/matt/Project/Graphics/terminal/src2/font8x16.h","w").write("\n".join(lines)+"\n")
print("wrote src2/font8x16.h")

# quick visual sanity: print a few glyphs
for ch in "A/\\-_|():":
    print(ch)
    for r in glyph_bits(ch):
        print("".join("#" if (r & (0x80>>x)) else "." for x in range(8)))
