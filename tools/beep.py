#!/usr/bin/env python3
# Bip sur le beeper du MKS TS35 (GPIO17).
#
# Usages :
#   beep.py [freq_hz] [duree_ms]              # 1 bip simple (defaut 1500 Hz, 120 ms)
#   beep.py morse "TEXTE"                     # joue le texte en Morse (700 Hz)
#   beep.py morse "TEXTE" <wpm>               # idem avec un rythme custom (defaut 18 wpm)
import sys, time
try:
    from gpiozero import TonalBuzzer
    from gpiozero.tones import Tone
except ImportError:
    sys.exit("gpiozero manquant (apt install python3-gpiozero)")

MORSE = {
    'A': '.-',    'B': '-...',  'C': '-.-.',  'D': '-..',   'E': '.',
    'F': '..-.',  'G': '--.',   'H': '....',  'I': '..',    'J': '.---',
    'K': '-.-',   'L': '.-..',  'M': '--',    'N': '-.',    'O': '---',
    'P': '.--.',  'Q': '--.-',  'R': '.-.',   'S': '...',   'T': '-',
    'U': '..-',   'V': '...-',  'W': '.--',   'X': '-..-',  'Y': '-.--',
    'Z': '--..',
    '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-',
    '5': '.....', '6': '-....', '7': '--...', '8': '---..', '9': '----.',
    '.': '.-.-.-', ',': '--..--', '?': '..--..', '/': '-..-.', '-': '-....-',
    '=': '-...-',  '+': '.-.-.', '@': '.--.-.',
}

def beep_simple(freq, dur_ms):
    buz = TonalBuzzer(17, octaves=3, mid_tone=Tone(1500))
    try:
        buz.play(Tone(freq))
        time.sleep(dur_ms / 1000.0)
    finally:
        buz.stop(); buz.close()

def beep_morse(text, wpm=18, freq=700):
    # Duree d'une "unite" Morse selon le standard PARIS : 1200 ms / wpm.
    unit = 1.2 / wpm
    buz = TonalBuzzer(17, octaves=3, mid_tone=Tone(freq))
    try:
        for c in text.upper():
            if c == ' ':
                time.sleep(unit * 7)        # inter-mot
                continue
            seq = MORSE.get(c, '')
            if not seq:
                continue
            for i, sym in enumerate(seq):
                buz.play(Tone(freq))
                time.sleep(unit * (1 if sym == '.' else 3))
                buz.stop()
                if i < len(seq) - 1:
                    time.sleep(unit)         # intra-char
            time.sleep(unit * 3)             # inter-char
    finally:
        buz.stop(); buz.close()

if len(sys.argv) >= 2 and sys.argv[1] == "morse":
    text = sys.argv[2] if len(sys.argv) > 2 else "SOS"
    wpm  = int(sys.argv[3]) if len(sys.argv) > 3 else 18
    beep_morse(text, wpm)
else:
    freq   = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
    dur_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    beep_simple(freq, dur_ms)
