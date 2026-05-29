#!/usr/bin/env python3
# Diagnostic + releve tactile ADS7846/XPT2046 (resistif : APPUYER FERMEMENT).
# Affiche chaque evenement brut et le min/max des coordonnees ADC, pour calibrer.
#
# Usage : python3 touchcal.py   (puis toucher les 4 coins, Ctrl-C pour finir)
import struct

FMT = 'llHHi'                 # input_event 64-bit : sec,usec,type,code,value
SZ = struct.calcsize(FMT)
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_X, ABS_Y = 0, 1

f = open('/dev/input/event0', 'rb')
x = y = None
minx = miny = 10**9
maxx = maxy = -10**9
taps = 0

print("APPUIE FERMEMENT (ongle/stylet). Chaque evenement s'affiche.")
print("Touche les 4 coins, puis Ctrl-C.\n")
try:
    while True:
        s, us, typ, code, val = struct.unpack(FMT, f.read(SZ))
        if typ == EV_ABS:
            if code == ABS_X: x = val
            elif code == ABS_Y: y = val
            print("  ABS code=%d val=%d" % (code, val))
        elif typ == EV_KEY:
            print("  KEY code=%d val=%d  (touch %s)" % (code, val, "DOWN" if val else "UP"))
        elif typ == EV_SYN and x is not None and y is not None:
            minx, maxx = min(minx, x), max(maxx, x)
            miny, maxy = min(miny, y), max(maxy, y)
            taps += 1
            print(">> POINT x=%4d y=%4d" % (x, y))
except KeyboardInterrupt:
    print("\n==> %d points | minX=%d maxX=%d  minY=%d maxY=%d"
          % (taps, minx, maxx, miny, maxy))
