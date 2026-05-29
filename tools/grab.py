#!/usr/bin/env python3
# Capture le framebuffer /dev/fb0 en PNG (/tmp/screen.png).
# Lit la geometrie reelle via /sys (gere le stride et l'orientation portrait 320x480).
# Astuce anti-tearing : geler le process LVGL (kill -STOP) avant de lancer ce script.
from PIL import Image
import struct

def rd(p):
    return open(p).read().strip()

w, h = [int(v) for v in rd('/sys/class/graphics/fb0/virtual_size').split(',')]
stride = int(rd('/sys/class/graphics/fb0/stride'))
data = open('/dev/fb0', 'rb').read(stride * h)
out = Image.new('RGB', (w, h))
op = out.load()
for yy in range(h):
    row = struct.unpack('<%dH' % w, data[yy*stride:yy*stride + w*2])
    for xx in range(w):
        v = row[xx]
        op[xx, yy] = ((v >> 11 & 0x1F) << 3, (v >> 5 & 0x3F) << 2, (v & 0x1F) << 3)
out.save('/tmp/screen.png')
print('saved', (w, h))
