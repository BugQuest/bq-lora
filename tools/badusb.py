#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Runner BadUSB compatible avec la grammaire Flipper Zero / Rubber Ducky.

Commandes supportees (un par ligne) :
    REM <commentaire>            (ou # ...)
    LAYOUT US | FR               (alias : LOCALE)
    DEFAULT_DELAY <ms>           (alias : DEFAULTDELAY) - pause apres chaque cmd
    STRING_DELAY <ms>            (delai entre chaque char tape)
    DELAY <ms>                   - pause ponctuelle
    STRING <texte>               - tape le texte
    STRINGLN <texte>             - tape le texte + ENTER
    ALTSTRING <texte>            (alias : ALTCHAR, ALT-STRING) - tape via ALT+numpad
    REPEAT <n>                   - repete la commande precedente n fois
    HOLD <key>                   - maintient une touche (modifier ou normale)
    RELEASE [<key>]              - relache (RELEASE seul = tout relacher)
    WAIT_FOR_BUTTON_PRESS        (no-op cote Pi : on simule par 2s de pause)

Combos :
    GUI r          CTRL ALT DEL          SHIFT TAB
    GUI-r          CTRL-ALT-DEL          SHIFT-TAB      (style Flipper)

Touches : ENTER/RETURN, ESC/ESCAPE, BACKSPACE/BKSP, TAB, SPACE, CAPSLOCK,
F1..F12, INSERT/INS, DELETE/DEL, HOME, END, PAGEUP/PUP, PAGEDOWN/PDOWN/PGDN,
UP, DOWN, LEFT, RIGHT (et les *ARROW), NUMLOCK, SCROLLLOCK, PRINTSCREEN/PRTSC,
PAUSE/BREAK, MENU/APP/CONTEXT, KP_0..KP_9, KP_PLUS/MINUS/SLASH/ASTERISK/DOT/ENTER.

Modifiers : CTRL/CONTROL, SHIFT, ALT, ALTGR/RALT/RIGHTALT, GUI/WIN/WINDOWS/CMD/COMMAND/META.
"""

import sys, time

SHIFT = 0x02
ALT   = 0x04
# AltGr : on simule par Ctrl+Alt (0x05). RightAlt seul (0x40) est
# inconsistant cote Windows ; Ctrl+Alt est interprete comme AltGr par
# le subsystem clavier Windows FR. Plus fiable cross-version.
ALTGR = 0x05

# -------- LAYOUTS --------
HID_US = {
    'a':4,'b':5,'c':6,'d':7,'e':8,'f':9,'g':10,'h':11,'i':12,'j':13,
    'k':14,'l':15,'m':16,'n':17,'o':18,'p':19,'q':20,'r':21,'s':22,'t':23,
    'u':24,'v':25,'w':26,'x':27,'y':28,'z':29,
    '1':30,'2':31,'3':32,'4':33,'5':34,'6':35,'7':36,'8':37,'9':38,'0':39,
    ' ':44,'\t':43,
    '-':45,'=':46,'[':47,']':48,'\\':49,
    ';':51,'\'':52,'`':53,',':54,'.':55,'/':56,
}
SHIFTED_US = {
    '!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8','(':'9',')':'0',
    '_':'-','+':'=','{':'[','}':']','|':'\\',':':';','"':'\'','~':'`','<':',','>':'.','?':'/',
}

HID_FR = {
    # lettres
    'a':(0,20),'A':(SHIFT,20), 'b':(0,5),'B':(SHIFT,5), 'c':(0,6),'C':(SHIFT,6),
    'd':(0,7),'D':(SHIFT,7),   'e':(0,8),'E':(SHIFT,8), 'f':(0,9),'F':(SHIFT,9),
    'g':(0,10),'G':(SHIFT,10), 'h':(0,11),'H':(SHIFT,11),'i':(0,12),'I':(SHIFT,12),
    'j':(0,13),'J':(SHIFT,13), 'k':(0,14),'K':(SHIFT,14),'l':(0,15),'L':(SHIFT,15),
    'm':(0,51),'M':(SHIFT,51), 'n':(0,17),'N':(SHIFT,17),'o':(0,18),'O':(SHIFT,18),
    'p':(0,19),'P':(SHIFT,19), 'q':(0,4), 'Q':(SHIFT,4), 'r':(0,21),'R':(SHIFT,21),
    's':(0,22),'S':(SHIFT,22), 't':(0,23),'T':(SHIFT,23),'u':(0,24),'U':(SHIFT,24),
    'v':(0,25),'V':(SHIFT,25), 'w':(0,29),'W':(SHIFT,29),'x':(0,27),'X':(SHIFT,27),
    'y':(0,28),'Y':(SHIFT,28), 'z':(0,26),'Z':(SHIFT,26),
    # chiffres (AZERTY = shift)
    '1':(SHIFT,30),'2':(SHIFT,31),'3':(SHIFT,32),'4':(SHIFT,33),'5':(SHIFT,34),
    '6':(SHIFT,35),'7':(SHIFT,36),'8':(SHIFT,37),'9':(SHIFT,38),'0':(SHIFT,39),
    # ponctuation
    ' ':(0,44),'\t':(0,43),'\n':(0,40),
    '&':(0,30),'"':(0,32),'\'':(0,33),'(':(0,34),'-':(0,35),
    '_':(0,37),')':(0,45),'=':(0,46),'+':(SHIFT,46),
    ',':(0,16),'?':(SHIFT,16),';':(0,54),'.':(SHIFT,54),
    ':':(0,55),'/':(SHIFT,55),'!':(0,56),
    '*':(0,49),'$':(0,48),'%':(SHIFT,52),
    # AltGr
    '@':(ALTGR,39),'#':(ALTGR,32),'{':(ALTGR,33),'[':(ALTGR,34),
    '|':(ALTGR,35),'`':(ALTGR,36),'\\':(ALTGR,37),'^':(ALTGR,38),
    '~':(ALTGR,31),']':(ALTGR,45),'}':(ALTGR,46),
    '<':(0,100),'>':(SHIFT,100),
}

KEYS = {
    'ENTER':40,'RETURN':40,
    'ESC':41,'ESCAPE':41,
    'BACKSPACE':42,'BKSP':42,
    'TAB':43,
    'SPACE':44,
    'CAPSLOCK':57,'CAPS':57,
    'F1':58,'F2':59,'F3':60,'F4':61,'F5':62,'F6':63,
    'F7':64,'F8':65,'F9':66,'F10':67,'F11':68,'F12':69,
    'PRINTSCREEN':70,'PRTSC':70,
    'SCROLLLOCK':71,
    'PAUSE':72,'BREAK':72,
    'INSERT':73,'INS':73,
    'HOME':74,
    'PAGEUP':75,'PUP':75,
    'DELETE':76,'DEL':76,
    'END':77,
    'PAGEDOWN':78,'PDOWN':78,'PGDN':78,
    'RIGHT':79,'RIGHTARROW':79,
    'LEFT':80,'LEFTARROW':80,
    'DOWN':81,'DOWNARROW':81,
    'UP':82,'UPARROW':82,
    'NUMLOCK':83,
    'KP_SLASH':84,'KP_ASTERISK':85,'KP_MINUS':86,'KP_PLUS':87,
    'KP_ENTER':88,
    'KP_1':89,'KP_2':90,'KP_3':91,'KP_4':92,'KP_5':93,
    'KP_6':94,'KP_7':95,'KP_8':96,'KP_9':97,'KP_0':98,
    'KP_DOT':99,
    'APP':101,'MENU':101,'CONTEXT':101,
}

MOD = {
    'CTRL':0x01,'CONTROL':0x01,
    'SHIFT':SHIFT,
    'ALT':ALT,
    'ALTGR':ALTGR,'RIGHTALT':ALTGR,'RALT':ALTGR,
    'GUI':0x08,'WIN':0x08,'WINDOWS':0x08,'CMD':0x08,'COMMAND':0x08,'META':0x08,
}

# -------- etat global --------
layout = 'us'
default_delay_ms = 0
str_delay_ms     = 8
held_mod         = 0
held_keys        = set()
last_cmd         = None   # ('STRING'|'STRINGLN'|'COMBO'|..., args)

# -------- HID writer --------
def send_report(fd, mod, codes):
    rpt = bytearray([mod | held_mod, 0])
    keys = list(codes)[:6]
    for k in held_keys:
        if k not in keys and len(keys) < 6:
            keys.append(k)
    rpt += bytearray(keys)
    rpt += bytes(8 - len(rpt))
    fd.write(bytes(rpt)); fd.flush()

def press(fd, mod, code):
    # AltGr (Ctrl+Alt) : appuyer le modificateur AVANT la touche, sinon
    # Windows n'a pas le temps d'entrer dans la couche AltGr et tape
    # la touche sans, donnant un mauvais caractere.
    if (mod & 0x05) == 0x05 and code:
        send_report(fd, mod, [])
    send_report(fd, mod, [code] if code else [])
    send_report(fd, 0, [])

def type_char(fd, c):
    if layout == 'fr':
        if c in HID_FR:
            m, k = HID_FR[c]; press(fd, m, k)
        return
    if c in SHIFTED_US:
        b = SHIFTED_US[c]
        if b in HID_US: press(fd, SHIFT, HID_US[b])
        return
    if c.isalpha() and c.isupper():
        l = c.lower()
        if l in HID_US: press(fd, SHIFT, HID_US[l])
        return
    if c in HID_US:
        press(fd, 0, HID_US[c])

def type_string(fd, s):
    for c in s:
        type_char(fd, c)
        time.sleep(str_delay_ms / 1000.0)

def type_alt(fd, s):
    """ALT+numpad : tape la valeur decimale ASCII de chaque char."""
    for c in s:
        code = ord(c) % 1000
        send_report(fd, ALT, [])
        for d in str(code):
            n = int(d)
            kp = 98 if n == 0 else (89 + (n - 1))
            send_report(fd, ALT, [kp])
            send_report(fd, ALT, [])
            time.sleep(0.012)
        send_report(fd, 0, [])
        time.sleep(0.03)

def resolve_combo(tokens):
    mod = 0; code = 0
    for t in tokens:
        u = t.upper()
        if u in MOD:      mod |= MOD[u]
        elif u in KEYS:   code = KEYS[u]
        elif len(t) == 1:
            c = t.lower()
            if layout == 'fr':
                if c in HID_FR:
                    m, k = HID_FR[c]; mod |= m; code = k
            else:
                if c in HID_US: code = HID_US[c]
                if t.isupper(): mod |= SHIFT
    return mod, code

def run(path):
    global layout, default_delay_ms, str_delay_ms, held_mod, held_keys, last_cmd
    with open('/dev/hidg0', 'wb', buffering=0) as fd:
        for raw in open(path, encoding='utf-8'):
            line = raw.rstrip('\r\n')
            stripped = line.lstrip()
            if not stripped or stripped.startswith('#') or stripped.startswith('REM'):
                continue
            up = line.upper()
            cmd_done = None

            if up.startswith('LAYOUT ') or up.startswith('LOCALE '):
                lo = line.split(None, 1)[1].strip().lower()
                if lo.startswith('us'): layout = 'us'
                elif lo.startswith('fr'): layout = 'fr'
            elif up.startswith('DEFAULT_DELAY ') or up.startswith('DEFAULTDELAY '):
                default_delay_ms = int(line.split(None, 1)[1].strip())
            elif up.startswith('STRING_DELAY ') or up.startswith('STRINGDELAY '):
                str_delay_ms = int(line.split(None, 1)[1].strip())
            elif up.startswith('DELAY '):
                time.sleep(int(line.split(None, 1)[1].strip()) / 1000.0)
            elif up.startswith('STRINGLN '):
                txt = line[9:]
                type_string(fd, txt)
                press(fd, 0, KEYS['ENTER'])
                cmd_done = ('STRINGLN', txt)
            elif up.startswith('STRING '):
                txt = line[7:]
                type_string(fd, txt)
                cmd_done = ('STRING', txt)
            elif up.startswith('ALTSTRING ') or up.startswith('ALTCHAR ') or up.startswith('ALT-STRING '):
                txt = line.split(None, 1)[1] if ' ' in line else ''
                type_alt(fd, txt)
                cmd_done = ('ALTSTRING', txt)
            elif up.startswith('HOLD '):
                k = line.split(None, 1)[1].strip().upper()
                if   k in MOD:  held_mod |= MOD[k]
                elif k in KEYS: held_keys.add(KEYS[k])
                send_report(fd, 0, [])
            elif up.startswith('RELEASE'):
                rest = line[7:].strip().upper()
                if not rest:
                    held_mod = 0; held_keys.clear()
                elif rest in MOD:  held_mod &= ~MOD[rest]
                elif rest in KEYS: held_keys.discard(KEYS[rest])
                send_report(fd, 0, [])
            elif up.startswith('REPEAT '):
                n = int(line.split(None, 1)[1].strip())
                if last_cmd:
                    typ, arg = last_cmd
                    for _ in range(n):
                        if   typ == 'STRING':   type_string(fd, arg)
                        elif typ == 'STRINGLN': type_string(fd, arg); press(fd, 0, KEYS['ENTER'])
                        elif typ == 'ALTSTRING': type_alt(fd, arg)
                        elif typ == 'COMBO':    press(fd, arg[0], arg[1])
                        if default_delay_ms > 0: time.sleep(default_delay_ms / 1000.0)
                        time.sleep(0.02)
            elif up.startswith('WAIT_FOR_BUTTON_PRESS') or up.startswith('WAITFORBUTTON'):
                time.sleep(2)
            else:
                # combos / touches : autorise '-' a la place d'espace (style Flipper)
                tokens = line.replace('-', ' ').split()
                first = tokens[0].upper() if tokens else ''
                if first in MOD or first in KEYS:
                    mod, code = resolve_combo(tokens)
                    if code:
                        press(fd, mod, code)
                        cmd_done = ('COMBO', (mod, code))
                else:
                    type_string(fd, line)
                    cmd_done = ('STRING', line)

            if cmd_done: last_cmd = cmd_done
            if default_delay_ms > 0: time.sleep(default_delay_ms / 1000.0)
            time.sleep(0.02)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("usage: badusb.py <script.txt>")
    run(sys.argv[1])
