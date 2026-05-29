#!/usr/bin/env python3
# Boot splash : dessine un ecran cyberpunk et l'envoie sur /dev/fb0 (RGB565).
# Lance tot au boot par meshui-splash.service, avant que meshui ne prenne la main.
from PIL import Image, ImageDraw, ImageFont
import struct

def rd(p):
    return open(p).read().strip()

w, h = [int(v) for v in rd('/sys/class/graphics/fb0/virtual_size').split(',')]
stride = int(rd('/sys/class/graphics/fb0/stride'))

CY = (0, 229, 255)
MA = (255, 42, 109)
DIM = (82, 103, 122)
BORDER = (27, 42, 61)
BG = (7, 10, 15)

def font(sz):
    try:
        return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', sz)
    except Exception:
        return ImageFont.load_default()

img = Image.new('RGB', (w, h), BG)
d = ImageDraw.Draw(img)
d.rectangle([4, 4, w - 5, h - 5], outline=BORDER)

def centered(text, f, y, fill):
    b = d.textbbox((0, 0), text, font=f)
    d.text(((w - (b[2] - b[0])) / 2 - b[0], y), text, font=f, fill=fill)

centered("MESH//OS", font(34), h / 2 - 78, CY)
centered("node // NODE-7F3A", font(15), h / 2 - 24, MA)
d.line([w / 2 - 70, h / 2 + 16, w / 2 + 70, h / 2 + 16], fill=BORDER)
centered("booting...", font(13), h - 52, DIM)

# Conversion RGB565 + ecriture framebuffer (respecte le stride)
px = img.load()
buf = bytearray()
for y in range(h):
    row = bytearray()
    for x in range(w):
        r, g, b = px[x, y]
        row += struct.pack('<H', ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    row += bytes(stride - len(row))
    buf += row
open('/dev/fb0', 'wb').write(bytes(buf))
