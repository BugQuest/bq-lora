#!/bin/bash
# Provisionnement premier démarrage : LVGL, lv_conf.h, build, services.
# Appelé par cloud-init (runcmd) après installation des paquets et extraction
# du bundle dans /home/bq-lora/meshui. Idempotent.
set -euo pipefail

U=bq-lora
H=/home/$U
SRC=$H/meshui

exec >>/var/log/meshui-provision.log 2>&1
echo "=== provision $(date) ==="

# Dépendances (filet : cloud-init peut planter sur des 404 de mirroir)
export DEBIAN_FRONTEND=noninteractive
apt-get update -y || true
apt-get install -y build-essential cmake git python3-pil

# LVGL (v9.2)
if [ ! -d "$H/lvgl" ]; then
    sudo -u "$U" git clone --depth 1 -b release/v9.2 https://github.com/lvgl/lvgl.git "$H/lvgl"
fi
ln -sfn "$H/lvgl" "$SRC/lvgl"

# lv_conf.h depuis le template + réglages validés
cp "$H/lvgl/lv_conf_template.h" "$SRC/lv_conf.h"
sed -i '0,/#if 0/s//#if 1/' "$SRC/lv_conf.h"
sed -i 's/#define LV_COLOR_DEPTH 32/#define LV_COLOR_DEPTH 16/' "$SRC/lv_conf.h"
sed -i -E 's/#define LV_MEM_SIZE \(64 \* 1024U\)/#define LV_MEM_SIZE (1024 * 1024U)/' "$SRC/lv_conf.h"
sed -i -E 's/(#define LV_USE_LINUX_FBDEV) +0/\1 1/' "$SRC/lv_conf.h"
for f in 12 16 20 28; do
    sed -i -E "s/(#define LV_FONT_MONTSERRAT_$f) +0/\\1 1/" "$SRC/lv_conf.h"
done
sed -i -E 's/(#define LV_FONT_UNSCII_16) +0/\1 1/' "$SRC/lv_conf.h"

chown -R "$U:$U" "$H"

# Build
sudo -u "$U" cmake -S "$SRC" -B "$SRC/build"
sudo -u "$U" cmake --build "$SRC/build" --target meshui -j2

# Helper privilégié + sudoers NOPASSWD limités (pour les contrôles dans l'UI)
install -m 755 "$SRC/deploy/meshui-ctl"      /usr/local/sbin/meshui-ctl
install -m 440 -o root -g root "$SRC/deploy/meshui-sudoers" /etc/sudoers.d/meshui
visudo -c -f /etc/sudoers.d/meshui

# Services systemd
install -m 644 "$SRC/deploy/meshui.service" /etc/systemd/system/meshui.service
install -m 644 "$SRC/deploy/meshui-splash.service" /etc/systemd/system/meshui-splash.service
systemctl daemon-reload
systemctl enable meshui-splash.service meshui.service
systemctl start meshui-splash.service meshui.service || true

echo "=== provision done $(date) ==="
