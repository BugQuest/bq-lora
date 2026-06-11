#!/bin/sh
# Garantit /dev/fb_spi -> panneau SPI (driver fbtft fb_ili9486), quel que soit le
# numero fbN. Filet de securite si la regle udev 99-bq-lora-ui-fb.rules n'a pas
# (encore) cree le symlink au moment ou splash/UI demarrent. Idempotent ; attend
# l'apparition du panneau jusqu'a ~10 s puis abandonne (non bloquant pour le boot).
i=0
while [ "$i" -lt 100 ]; do
    [ -e /dev/fb_spi ] && exit 0
    for d in /sys/class/graphics/fb[0-9]*; do
        [ -r "$d/name" ] || continue
        if [ "$(cat "$d/name" 2>/dev/null)" = fb_ili9486 ]; then
            ln -sf "/dev/$(basename "$d")" /dev/fb_spi
            exit 0
        fi
    done
    i=$((i + 1))
    sleep 0.1
done
echo "fb-spi-link: panneau SPI (fb_ili9486) introuvable" >&2
exit 1
