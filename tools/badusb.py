#!/usr/bin/env python3
# Mini-runner Ducky Script qui ecrit des rapports HID sur /dev/hidg0.
# Layout cible : US keyboard (caracteres FR a regler cote victime ou via STRING ASCII).
#
# Syntaxe supportee (un cmd par ligne, # ou REM = commentaire) :
#   STRING <texte>     -> tape le texte
#   ENTER / TAB / ESC / BACKSPACE / SPACE / UP / DOWN / LEFT / RIGHT
#   F1..F12  HOME END INSERT DELETE PAGEUP PAGEDOWN  CAPSLOCK
#   DELAY <ms>         -> attente
#   CTRL <key>  ALT <key>  SHIFT <key>  GUI <key>  (combinaisons)
#   GUI r, CTRL ALT t, CTRL c, etc.
import sys, time

HID = {
    'a':4,'b':5,'c':6,'d':7,'e':8,'f':9,'g':10,'h':11,'i':12,'j':13,
    'k':14,'l':15,'m':16,'n':17,'o':18,'p':19,'q':20,'r':21,'s':22,'t':23,
    'u':24,'v':25,'w':26,'x':27,'y':28,'z':29,
    '1':30,'2':31,'3':32,'4':33,'5':34,'6':35,'7':36,'8':37,'9':38,'0':39,
    ' ':44,'\t':43,
    '-':45,'=':46,'[':47,']':48,'\\':49,
    ';':51,'\'':52,'`':53,',':54,'.':55,'/':56,
}
SHIFTED = {
    '!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8','(':'9',')':'0',
    '_':'-','+':'=','{':'[','}':']','|':'\\',':':';','"':'\'','~':'`','<':',','>':'.','?':'/',
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
MOD = {'CTRL':0x01,'SHIFT':0x02,'ALT':0x04,'GUI':0x08,'WIN':0x08,'CMD':0x08}

def send(fd, mod, code):
    fd.write(bytes([mod, 0, code, 0, 0, 0, 0, 0]))
    fd.flush()
    fd.write(bytes(8))
    fd.flush()

def type_char(fd, c):
    if c in SHIFTED:
        base = SHIFTED[c]
        if base in HID:
            send(fd, 0x02, HID[base])
        return
    if c.isalpha() and c.isupper():
        l = c.lower()
        if l in HID:
            send(fd, 0x02, HID[l])
        return
    if c in HID:
        send(fd, 0, HID[c])

def resolve_combo(tokens):
    mod = 0; code = 0
    for t in tokens:
        if t in MOD:        mod |= MOD[t]
        elif t in KEYS:     code = KEYS[t]
        elif len(t) == 1:
            c = t.lower()
            if c in HID:     code = HID[c]
            if t.isupper():  mod |= 0x02
    return mod, code

def run(path):
    with open('/dev/hidg0', 'wb', buffering=0) as fd:
        for line in open(path):
            line = line.rstrip()
            if not line or line.startswith('#') or line.startswith('REM '):
                continue
            up = line.upper()
            if up.startswith('DELAY '):
                time.sleep(int(line.split(None, 1)[1].strip()) / 1000.0)
            elif up.startswith('STRING '):
                for c in line[7:]:
                    type_char(fd, c)
                    time.sleep(0.008)
            elif up in KEYS:
                send(fd, 0, KEYS[up])
            elif any(line.startswith(m + ' ') for m in MOD):
                mod, code = resolve_combo(line.split())
                if code: send(fd, mod, code)
            else:
                # ligne brute -> tape tel quel
                for c in line:
                    type_char(fd, c)
                    time.sleep(0.008)
            time.sleep(0.02)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("usage: badusb.py <script.txt>")
    run(sys.argv[1])
