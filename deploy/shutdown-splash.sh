#!/bin/sh
# Affiche un splash sur l'ecran SPI pendant l'arret/redemarrage.
# Appele par l'ExecStop de meshui-shutdown.service, donc APRES que meshui
# ait libere /dev/fb0. Detecte s'il s'agit d'un reboot ou d'un arret.
PY=/usr/bin/python3
SPLASH=/home/bq-lora/meshui/tools/splash.py

# Empeche la console kernel (fbcon) de redessiner par-dessus le splash.
echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true

# Re-affirme le retroeclairage au max (au cas ou il aurait ete baisse) :
# la PWM tourne a 1 kHz (periode 1 ms = 1000000 ns).
echo 1000000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle 2>/dev/null || true

# Trace de verification (le journald est volatile, on ecrit sur disque).
if systemctl list-jobs 2>/dev/null | grep -q 'reboot.target'; then
    MODE=reboot; STATUS="[ redemarrage... ]"; ACCENT=cyan
else
    MODE=poweroff; STATUS="[ arret en cours... ]"; ACCENT=magenta
fi
echo "$(date '+%Y-%m-%d %H:%M:%S') splash $MODE" >> /var/log/meshui-shutdown.log 2>/dev/null || true

exec "$PY" "$SPLASH" --status "$STATUS" --accent "$ACCENT"
