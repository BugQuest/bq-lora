#!/bin/sh
# Affiche un splash sur l'ecran SPI pendant l'arret/redemarrage.
# Appele par l'ExecStop de meshui-shutdown.service, donc APRES que meshui
# ait libere /dev/fb0. Detecte s'il s'agit d'un reboot ou d'un arret.
PY=/usr/bin/python3
SPLASH=/home/bq-lora/meshui/tools/splash.py

# Empeche la console kernel (fbcon) de redessiner par-dessus le splash.
echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true

if systemctl list-jobs 2>/dev/null | grep -q 'reboot.target'; then
    exec "$PY" "$SPLASH" --status "[ redemarrage... ]" --accent cyan
else
    exec "$PY" "$SPLASH" --status "[ arret en cours... ]" --accent magenta
fi
