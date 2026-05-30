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
apt-get install -y build-essential cmake git python3-pil python3-gpiozero dosfstools

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
sed -i -E 's/(#define LV_USE_QRCODE) +0/\1 1/' "$SRC/lv_conf.h"

chown -R "$U:$U" "$H"

# Build
sudo -u "$U" cmake -S "$SRC" -B "$SRC/build"
sudo -u "$U" cmake --build "$SRC/build" --target meshui -j2

# Helper privilégié + sudoers NOPASSWD limités (pour les contrôles dans l'UI)
install -m 755 "$SRC/deploy/meshui-ctl"      /usr/local/sbin/meshui-ctl
install -m 440 -o root -g root "$SRC/deploy/meshui-sudoers" /etc/sudoers.d/meshui
visudo -c -f /etc/sudoers.d/meshui

# Backlight PWM (rétroéclairage écran)
install -m 755 "$SRC/deploy/backlight-init.sh"   /usr/local/sbin/meshui-backlight-init
install -m 644 "$SRC/deploy/backlight.service"   /etc/systemd/system/backlight.service
systemctl enable backlight.service || true
systemctl start  backlight.service || true

# Gadget USB CDC NCM (compatible Windows 11) via configfs au boot
install -m 755 "$SRC/deploy/usb-ncm-setup.sh"     /usr/local/sbin/meshui-usb-gadget
install -m 755 "$SRC/deploy/usb-hid-setup.sh"     /usr/local/sbin/meshui-usb-hid
install -m 755 "$SRC/deploy/usb-storage-setup.sh" /usr/local/sbin/meshui-usb-storage
install -m 644 "$SRC/deploy/usb-gadget.service"   /etc/systemd/system/usb-gadget.service

# Image FAT pour le mode STORAGE (depot de scripts BadUSB depuis le PC)
IMG="$H/meshui/badusb.img"
MNT="$H/meshui/badusb"
TMP="/tmp/meshui-badusb-seed"
mkdir -p "$TMP"
if [ -d "$MNT" ] && ! mountpoint -q "$MNT"; then
    cp -r "$MNT"/* "$TMP/" 2>/dev/null || true
fi
if [ ! -f "$IMG" ]; then
    dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
    mkfs.vfat -F 16 -n BADUSB "$IMG" >/dev/null
    chown $U:$U "$IMG"
fi
mkdir -p "$MNT"
chown $U:$U "$MNT"
mountpoint -q "$MNT" || mount -o loop,umask=000 "$IMG" "$MNT"
# Seed avec les exemples si l'image est vide
if [ -z "$(ls -A "$MNT" 2>/dev/null)" ] && [ -d "$TMP" ]; then
    cp -r "$TMP"/* "$MNT/" 2>/dev/null || true
fi
rm -rf "$TMP"
# Retirer g_ether de cmdline.txt (legacy, en conflit avec le gadget configfs)
sed -i 's/modules-load=dwc2,g_ether/modules-load=dwc2/' /boot/firmware/cmdline.txt || true
# Bascule directe sur NCM des le premier boot (sinon il faudrait un reboot)
modprobe -r g_ether 2>/dev/null || true
systemctl enable usb-gadget.service || true
systemctl start  usb-gadget.service || true
# Profil NM "shared" pour usb0 (DHCP du Pi vers le PC)
nmcli connection show usb0 >/dev/null 2>&1 || \
    nmcli connection add type ethernet ifname usb0 con-name usb0 \
        ipv4.method shared ipv6.method ignore autoconnect yes
nmcli connection up usb0 2>/dev/null || true

# Services systemd
install -m 644 "$SRC/deploy/meshui.service" /etc/systemd/system/meshui.service
install -m 644 "$SRC/deploy/meshui-splash.service" /etc/systemd/system/meshui-splash.service
systemctl daemon-reload
systemctl enable meshui-splash.service meshui.service
systemctl start meshui-splash.service meshui.service || true

echo "=== provision done $(date) ==="
