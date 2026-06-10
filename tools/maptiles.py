#!/usr/bin/env python3
"""
Telecharge des tuiles OSM et les convertit en RAW RGB565 (little-endian) pour
l'app carte LVGL du bq-lora-ui. Aucune dependance LVGL (FS/PNG) cote C : chaque
tuile est un .bin de 256x256 px = 131072 octets, charge directement en memoire.

Sortie : <out>/<z>/<x>/<y>.bin   (resumable : saute les fichiers deja presents)

Usage :
    python3 maptiles.py                 # plan Grand Lyon par defaut
    python3 maptiles.py --out ~/maps    # repertoire de sortie
"""
import argparse, hashlib, math, os, sys, time, urllib.request
from io import BytesIO
from PIL import Image
import numpy as np

# Plan par defaut : Grand Lyon metropole.
#   (zmin, zmax, lat_sud, lat_nord, lon_ouest, lon_est)
JOBS = [
    (11, 15, 45.60, 45.90, 4.70, 5.10),   # toute la metropole
    (16, 16, 45.71, 45.81, 4.78, 4.92),   # centre de Lyon, detail rue
]

# tile.openstreetmap.org INTERDIT le bulk-download : il renvoie une image
# "access blocked" (HTTP 200) qui empoisonne le cache. On utilise les basemaps
# CARTO (fond sombre "dark_all" : bien plus lisible sur le petit ecran que le
# clair), qui tolerent un usage perso modere et servent de vraies tuiles.
TILE_URL = "https://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png"
UA = "bq-lora-ui/1.0 (offline cyberdeck map cache; personal use; realitynauts@gmail.com)"

# md5 hex de tuiles connues comme "image de blocage / erreur" a rejeter.
# (rempli dynamiquement : si une meme image revient trop souvent -> abandon)


def deg2num(lat, lon, z):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(lat)
    y = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return x, y


def to_rgb565(png_bytes):
    img = Image.open(BytesIO(png_bytes)).convert("RGB")
    if img.size != (256, 256):
        img = img.resize((256, 256))
    a = np.asarray(img, dtype=np.uint16)
    r = (a[:, :, 0] >> 3) & 0x1F
    g = (a[:, :, 1] >> 2) & 0x3F
    b = (a[:, :, 2] >> 3) & 0x1F
    return ((r << 11) | (g << 5) | b).astype("<u2").tobytes()


def fetch(z, x, y):
    req = urllib.request.Request(TILE_URL.format(z=z, x=x, y=y),
                                 headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.expanduser("~/bq-lora-ui/maps"))
    ap.add_argument("--delay", type=float, default=0.08)
    args = ap.parse_args()

    # liste complete des tuiles a traiter (dedupliquee par zoom)
    todo = {}
    for zmin, zmax, las, lan, low, loe in JOBS:
        for z in range(zmin, zmax + 1):
            x0, y0 = deg2num(lan, low, z)   # nord-ouest
            x1, y1 = deg2num(las, loe, z)   # sud-est
            xs = range(min(x0, x1), max(x0, x1) + 1)
            ys = range(min(y0, y1), max(y0, y1) + 1)
            todo.setdefault(z, set())
            for x in xs:
                for y in ys:
                    todo[z].add((x, y))

    total = sum(len(v) for v in todo.values())
    print(f"Plan : {total} tuiles sur zooms {sorted(todo)}", flush=True)

    done = skipped = failed = 0
    hash_count = {}   # md5(png) -> nb d'occurrences (detection image de blocage)
    t0 = time.time()
    for z in sorted(todo):
        for (x, y) in sorted(todo[z]):
            path = os.path.join(args.out, str(z), str(x), f"{y}.bin")
            if os.path.exists(path) and os.path.getsize(path) == 256 * 256 * 2:
                skipped += 1
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            try:
                png = fetch(z, x, y)
                h = hashlib.md5(png).hexdigest()
                hash_count[h] = hash_count.get(h, 0) + 1
                # Une meme image qui revient en boucle = tuile "access blocked".
                if hash_count[h] > 12:
                    print(f"\nARRET : la tuile z{z} {x}/{y} (md5 {h[:8]}) est "
                          f"servie {hash_count[h]}x a l'identique -> le serveur "
                          f"renvoie une image de blocage, pas la carte.\n"
                          f"Change de source (TILE_URL) ou augmente --delay.",
                          flush=True)
                    sys.exit(2)
                raw = to_rgb565(png)
                with open(path, "wb") as f:
                    f.write(raw)
                done += 1
                time.sleep(args.delay)
            except SystemExit:
                raise
            except Exception as e:
                failed += 1
                print(f"  ECHEC z{z} {x}/{y}: {e}", flush=True)
            if (done + skipped) % 100 == 0:
                el = time.time() - t0
                print(f"  {done+skipped}/{total}  (dl={done} skip={skipped} "
                      f"fail={failed}) {el:.0f}s", flush=True)

    print(f"Termine : dl={done} skip={skipped} fail={failed} "
          f"en {time.time()-t0:.0f}s -> {args.out}", flush=True)


if __name__ == "__main__":
    main()
