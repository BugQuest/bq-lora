#!/usr/bin/env python3
# Boot splash : ecran cyberpunk "BugQuest // LORA", blit RGB565 sur /dev/fb0.
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

def font(sz, bold=True):
    p = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf' if bold else \
        '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
    try:
        return ImageFont.truetype(p, sz)
    except Exception:
        return ImageFont.load_default()

def text_centered(d, text, f, y, fill):
    b = d.textbbox((0, 0), text, font=f)
    d.text(((w - (b[2] - b[0])) / 2 - b[0], y), text, font=f, fill=fill)

def text_at(d, text, f, x, y, fill):
    d.text((x, y), text, font=f, fill=fill)

img = Image.new('RGB', (w, h), BG)
d = ImageDraw.Draw(img)

# crochets aux 4 coins
L = 22; T = 2
def bracket(x, y, dx, dy):
    d.line([(x, y), (x + dx * L, y)], fill=CY, width=T)
    d.line([(x, y), (x, y + dy * L)], fill=CY, width=T)
bracket(6,   6,    1,  1)
bracket(w-7, 6,   -1,  1)
bracket(6,   h-7,  1, -1)
bracket(w-7, h-7, -1, -1)

# titre
text_centered(d, "BugQuest", font(36), h/2 - 84, CY)

# sous-titre lettré : "// L O R A"
sub_f = font(20)
sub = "/ / L O R A"
text_centered(d, sub, sub_f, h/2 - 36, MA)

# divider
d.line([(w/2 - 80, h/2 + 10), (w/2 + 80, h/2 + 10)], fill=BORDER, width=1)

# noeud
text_centered(d, "node // NODE-7F3A", font(13, False), h/2 + 22, DIM)

# bandeau bas
text_centered(d, "[ initialisation ]", font(13), h - 56, CY)
text_centered(d, "v0.1", font(11, False), h - 34, DIM)

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
