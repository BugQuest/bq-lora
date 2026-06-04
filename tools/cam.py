#!/usr/bin/env python3
# Convertit une photo (JPEG) en buffer brut RGB565 pour un canvas LVGL.
#
# L'UI (app/sys.c -> sys_cam_capture_async) appelle :
#     cam.py <photo.jpg> <sortie.rgb565> <largeur> <hauteur>
# La sortie fait exactement largeur*hauteur*2 octets (RGB565 little-endian,
# rouge sur les bits de poids fort), directement copiable dans le buffer d'un
# lv_canvas (LV_COLOR_FORMAT_RGB565). L'image est redimensionnee en gardant le
# ratio et centree sur fond noir.
import sys
from PIL import Image

src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

im = Image.open(src).convert("RGB")
im.thumbnail((w, h))                       # downscale en gardant le ratio
canvas = Image.new("RGB", (w, h), (0, 0, 0))
canvas.paste(im, ((w - im.width) // 2, (h - im.height) // 2))

# Packe en RGB565 : R[15:11] G[10:5] B[4:0], stocke en little-endian.
try:
    import numpy as np
    a = np.asarray(canvas, dtype=np.uint16)            # (h, w, 3)
    rgb565 = (((a[:, :, 0] & 0xF8) << 8) |
              ((a[:, :, 1] & 0xFC) << 3) |
              ( a[:, :, 2] >> 3)).astype("<u2")
    data = rgb565.tobytes()
except ImportError:
    out = bytearray(w * h * 2)
    i = 0
    for r, g, b in canvas.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i] = v & 0xFF
        out[i + 1] = (v >> 8) & 0xFF
        i += 2
    data = bytes(out)

with open(dst, "wb") as f:
    f.write(data)
