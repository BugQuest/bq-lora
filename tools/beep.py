#!/usr/bin/env python3
# Bip sur le beeper du MKS TS35 (GPIO17).
# Usage : beep.py [freq_hz] [duree_ms]
import sys, time
try:
    from gpiozero import TonalBuzzer
    from gpiozero.tones import Tone
except ImportError:
    sys.exit("gpiozero manquant (apt install python3-gpiozero)")

freq   = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
dur_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 120

# Plage du TonalBuzzer : 60..2093 Hz par defaut, suffisant pour un piezo.
buz = TonalBuzzer(17, octaves=3, mid_tone=Tone(1500))
try:
    buz.play(Tone(freq))
    time.sleep(dur_ms / 1000.0)
finally:
    buz.stop()
    buz.close()
