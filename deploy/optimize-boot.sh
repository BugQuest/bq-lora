#!/bin/bash
# Optimisations de temps de demarrage pour le Pi Zero 2 W (cyberdeck bq-lora-ui).
# Idempotent : rejouable sans effet de bord. A executer en root.
set -x

# 1) cloud-init : termine son role apres le premier provisionnement.
#    Le desactiver retire ~6 s et debloque toute la chaine sysinit.
#    (WiFi/hostname sont persistes par NetworkManager, pas recrees a chaque boot.)
touch /etc/cloud/cloud-init.disabled

# 2) Reseau : meshtasticd sert du TCP local (4403) + LoRa SPI, il n'a pas
#    besoin d'internet. On retire l'attente reseau du chemin critique (~4,5 s).
systemctl mask NetworkManager-wait-online.service
mkdir -p /etc/systemd/system/meshtasticd.service.d
cat > /etc/systemd/system/meshtasticd.service.d/no-net-wait.conf <<'EOF'
# Retire After=network-online.target : meshtasticd demarre sans attendre le WiFi.
[Unit]
After=
EOF

# 3) UI au plus tot : ordonnee juste apres le retroeclairage, sans attendre
#    meshtasticd ni multi-user.target (l'UI gere deja MESH indisponible).
mkdir -p /etc/systemd/system/bq-lora-ui.service.d
cat > /etc/systemd/system/bq-lora-ui.service.d/early.conf <<'EOF'
[Unit]
After=backlight.service
Wants=backlight.service
EOF

# 4) Bluetooth a la demande : demarre quand on ouvre l'ecran BT (via bq-lora-ui-ctl),
#    pas au boot.
systemctl disable bluetooth.service || true

# 5) Services/timers non necessaires au fonctionnement du cyberdeck.
systemctl disable e2scrub_reap.service systemd-pstore.service rpi-eeprom-update.service || true
systemctl disable apt-daily.timer apt-daily-upgrade.timer man-db.timer \
                  dpkg-db-backup.timer e2scrub_all.timer || true

# 6) cmdline : console serie de debug retiree (UART) + sortie kernel silencieuse.
M=/boot/firmware/cmdline.txt
sed -i 's/console=serial0,115200 //' "$M"
grep -q 'quiet' "$M" || sed -i 's/$/ quiet loglevel=3 logo.nologo/' "$M"

# 7) config firmware : retire les delais/sondes inutiles.
#    NB : on NE touche PAS a dtoverlay=vc4-kms-v3d : meme s'il est inutilise
#    (ecran en fbtft SPI), le retirer fait reapparaitre le framebuffer firmware
#    legacy en fb0 et repousse l'ecran ILI9486 sur fb1 -> ecran noir.
C=/boot/firmware/config.txt
sed -i '/^enable_uart=1/d' "$C"
sed -i 's/^camera_auto_detect=1/camera_auto_detect=0/' "$C"   # pas de camera CSI
sed -i 's/^display_auto_detect=1/display_auto_detect=0/' "$C"  # pas d'ecran DSI
sed -i 's/^dtparam=audio=on/dtparam=audio=off/'        "$C"   # pas de sortie audio
grep -q '^disable_splash=1' "$C" || echo 'disable_splash=1' >> "$C"  # pas d'arc-en-ciel GPU
grep -q '^boot_delay=0'    "$C" || echo 'boot_delay=0'    >> "$C"    # pas de delai firmware

systemctl daemon-reload
echo "=== optimize-boot done ==="
