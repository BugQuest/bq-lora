#!/bin/sh
# Initialise la PWM materielle sur GPIO18 pour piloter le retroeclairage.
# Idempotent. Lance par backlight.service au boot, avant fbtft/meshui.
set -e

CHIP=/sys/class/pwm/pwmchip0
PWM=$CHIP/pwm0

# Si la PWM n'est pas encore visible, attendre un peu (overlay pas applique ?)
for _ in 1 2 3 4 5; do
    [ -d "$CHIP" ] && break
    sleep 0.3
done
if [ ! -d "$CHIP" ]; then
    # Fallback : driver PWM absent -> rester allume en GPIO digital high
    echo "pwmchip0 absent, fallback GPIO18 = HIGH"
    /usr/bin/pinctrl set 18 op dh || true
    exit 0
fi

# Export du channel 0 si pas deja fait
[ -d "$PWM" ] || echo 0 > "$CHIP/export"

# Periode 1 ms (1 kHz) - assez haute pour ne pas etre vue, basse pour etre fiable
echo 1000000 > "$PWM/period"
# Duty initial 100 %
echo 1000000 > "$PWM/duty_cycle"
echo 1       > "$PWM/enable"

# Permettre au groupe video d'ecrire le duty (UI controle la luminosite)
chgrp video  "$PWM/duty_cycle" 2>/dev/null || true
chmod g+w    "$PWM/duty_cycle" 2>/dev/null || true
