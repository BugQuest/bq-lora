#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Runner Ducky Script qui ecrit des rapports HID sur /dev/hidg0.
#
# Layouts geres :
#   LAYOUT US   (defaut) - clavier QWERTY US
#   LAYOUT FR             - AZERTY francais (avec AltGr pour @ # [ ] | \ etc.)
#
# Syntaxe (un cmd par ligne ; # ou REM en debut = commentaire) :
#   LAYOUT US|FR
#   STRING <texte>       -> tape le texte
#   STRINGLN <texte>     -> tape le texte puis ENTER
#   ENTER / TAB / ESC / BACKSPACE / SPACE / UP/DOWN/LEFT/RIGHT
#   F1..F12 / HOME END INSERT DELETE PAGEUP PAGEDOWN / CAPSLOCK
#   DELAY <ms>
#   CTRL <k>  ALT <k>  SHIFT <k>  GUI <k>   (combos, ex: GUI r, CTRL ALT t)
import sys, time

# ---- HID usage codes (clavier) ----
SHIFT = 0x02
ALTGR = 0x40

# Layout US : char -> (modifier, code)
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

# Layout FR (AZERTY) : char -> (modifier, US HID code) interprete par Windows en mode FR
# Les codes correspondent aux scancodes US qui produisent ces caracteres sur AZERTY.
HID_FR = {
    # lettres
    'a':(0,20),'A':(SHIFT,20),
    'b':(0,5), 'B':(SHIFT,5),
    'c':(0,6), 'C':(SHIFT,6),
    'd':(0,7), 'D':(SHIFT,7),
    'e':(0,8), 'E':(SHIFT,8),
    'f':(0,9), 'F':(SHIFT,9),
    'g':(0,10),'G':(SHIFT,10),
    'h':(0,11),'H':(SHIFT,11),
    'i':(0,12),'I':(SHIFT,12),
    'j':(0,13),'J':(SHIFT,13),
    'k':(0,14),'K':(SHIFT,14),
    'l':(0,15),'L':(SHIFT,15),
    'm':(0,51),'M':(SHIFT,51),    # US ;
    'n':(0,17),'N':(SHIFT,17),
    'o':(0,18),'O':(SHIFT,18),
    'p':(0,19),'P':(SHIFT,19),
    'q':(0,4), 'Q':(SHIFT,4),     # US a
    'r':(0,21),'R':(SHIFT,21),
    's':(0,22),'S':(SHIFT,22),
    't':(0,23),'T':(SHIFT,23),
    'u':(0,24),'U':(SHIFT,24),
    'v':(0,25),'V':(SHIFT,25),
    'w':(0,29),'W':(SHIFT,29),    # US z
    'x':(0,27),'X':(SHIFT,27),
    'y':(0,28),'Y':(SHIFT,28),
    'z':(0,26),'Z':(SHIFT,26),    # US w

    # chiffres : sur AZERTY ils sont au shift
    '1':(SHIFT,30),'2':(SHIFT,31),'3':(SHIFT,32),'4':(SHIFT,33),'5':(SHIFT,34),
    '6':(SHIFT,35),'7':(SHIFT,36),'8':(SHIFT,37),'9':(SHIFT,38),'0':(SHIFT,39),

    # ponctuation
    ' ':(0,44),'\t':(0,43),'\n':(0,40),
    '&':(0,30),
    '"':(0,32),
    '\'':(0,33),
    '(':(0,34),
    '-':(0,35),
    '_':(0,37),
    ')':(0,45),
    '=':(0,46),
    '+':(SHIFT,46),
    ',':(0,16),    # US m
    '?':(SHIFT,16),
    ';':(0,54),    # US ,
    '.':(SHIFT,54),
    ':':(0,55),    # US .
    '/':(SHIFT,55),
    '!':(0,56),    # US /
    '*':(0,49),    # US \\
    '$':(0,48),    # US ]
    '%':(SHIFT,52),

    # symboles AltGr (right alt)
    '@':(ALTGR,39),
    '#':(ALTGR,32),
    '{':(ALTGR,33),
    '[':(ALTGR,34),
    '|':(ALTGR,35),
    '`':(ALTGR,36),
    '\\':(ALTGR,37),
    '^':(ALTGR,38),
    '~':(ALTGR,31),
    ']':(ALTGR,45),
    '}':(ALTGR,46),
    '<':(0,100),   # ISO key (non-US backslash) : scancode 100, AZERTY donne <
    '>':(SHIFT,100),
}

KEYS = {
    'ENTER':40,'ESC':41,'BACKSPACE':42,'TAB':43,'SPACE':44,
    'CAPSLOCK':57,
    'F1':58,'F2':59,'F3':60,'F4':61,'F5':62,'F6':63,
    'F7':64,'F8':65,'F9':66,'F10':67,'F11':68,'F12':69,
    'PRINTSCREEN':70,'SCROLLLOCK':71,'PAUSE':72,
    'INSERT':73,'HOME':74,'PAGEUP':75,'DELETE':76,'END':77,'PAGEDOWN':78,
    'RIGHT':79,'LEFT':80,'DOWN':81,'UP':82,
}
MOD = {'CTRL':0x01,'SHIFT':SHIFT,'ALT':0x04,'GUI':0x08,'WIN':0x08,'CMD':0x08,'ALTGR':ALTGR}

layout = 'us'

def send(fd, mod, code):
    fd.write(bytes([mod, 0, code, 0, 0, 0, 0, 0])); fd.flush()
    fd.write(bytes(8)); fd.flush()

def type_char(fd, c):
    if layout == 'fr':
        if c in HID_FR:
            mod, code = HID_FR[c]; send(fd, mod, code)
        return
    # US layout
    if c in SHIFTED_US:
        base = SHIFTED_US[c]
        if base in HID_US: send(fd, SHIFT, HID_US[base])
        return
    if c.isalpha() and c.isupper():
        l = c.lower()
        if l in HID_US: send(fd, SHIFT, HID_US[l])
        return
    if c in HID_US:
        send(fd, 0, HID_US[c])

def resolve_combo(tokens):
    mod = 0; code = 0
    for t in tokens:
        if t in MOD:    mod |= MOD[t]
        elif t in KEYS: code = KEYS[t]
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
    global layout
    with open('/dev/hidg0', 'wb', buffering=0) as fd:
        for raw in open(path, encoding='utf-8'):
            line = raw.rstrip('\n').rstrip('\r')
            if not line or line.startswith('#') or line.startswith('REM ') or line.startswith('REM'):
                continue
            up = line.upper()
            if up.startswith('LAYOUT '):
                lo = line.split(None, 1)[1].strip().lower()
                if lo in ('fr', 'us'): layout = lo
            elif up.startswith('DELAY '):
                time.sleep(int(line.split(None, 1)[1].strip()) / 1000.0)
            elif up.startswith('STRINGLN '):
                for c in line[9:]:
                    type_char(fd, c); time.sleep(0.008)
                send(fd, 0, KEYS['ENTER'])
            elif up.startswith('STRING '):
                for c in line[7:]:
                    type_char(fd, c); time.sleep(0.008)
            elif up in KEYS:
                send(fd, 0, KEYS[up])
            elif any(line.startswith(m + ' ') for m in MOD):
                mod, code = resolve_combo(line.split())
                if code: send(fd, mod, code)
            else:
                # ligne brute -> tape tel quel
                for c in line:
                    type_char(fd, c); time.sleep(0.008)
            time.sleep(0.02)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("usage: badusb.py <script.txt>")
    run(sys.argv[1])
