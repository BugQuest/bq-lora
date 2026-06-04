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

# LVGL (v9.2) — les sources buildables vivent dans $SRC/app (layout du dépôt).
# LVGL et lv_conf.h doivent donc être visibles depuis app/.
if [ ! -d "$H/lvgl" ]; then
    sudo -u "$U" git clone --depth 1 -b release/v9.2 https://github.com/lvgl/lvgl.git "$H/lvgl"
fi
ln -sfn "$H/lvgl" "$SRC/app/lvgl"

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

# lv_conf.h doit être trouvable depuis app/ (LV_CONF_INCLUDE_SIMPLE)
ln -sfn "$SRC/lv_conf.h" "$SRC/app/lv_conf.h"

chown -R "$U:$U" "$H"

# Build (source = app/, sortie = build/ ; le service lance build/meshui)
sudo -u "$U" cmake -S "$SRC/app" -B "$SRC/build"
sudo -u "$U" cmake --build "$SRC/build" --target meshui -j2

# Helper privilégié + sudoers NOPASSWD limités (pour les contrôles dans l'UI)
install -m 755 "$SRC/deploy/meshui-ctl"      /usr/local/sbin/meshui-ctl
install -m 755 "$SRC/deploy/meshui-update.sh" /usr/local/sbin/meshui-update
install -m 440 -o root -g root "$SRC/deploy/meshui-sudoers" /etc/sudoers.d/meshui
visudo -c -f /etc/sudoers.d/meshui

# Init le repo git pour les mises a jour OTA depuis l'UI
cd "$SRC"
if [ ! -d .git ]; then
    sudo -u "$U" git init -q -b master
    sudo -u "$U" git remote add origin https://github.com/BugQuest/bq-lora.git
    sudo -u "$U" git fetch -q origin master || true
    sudo -u "$U" git reset --hard origin/master >/dev/null 2>&1 || true
fi

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
# Profil NM "shared" pour usb0 (DHCP du Pi vers le PC).
# Sous-reseau 10.42.1.0/24 distinct de celui du hotspot WiFi (10.42.0.0/24) :
# sans adresse explicite, NetworkManager attribue 10.42.0.1 aux DEUX interfaces
# shared, ce qui casse l'USB des que le hotspot est actif (collision d'IP/dnsmasq).
nmcli connection show usb0 >/dev/null 2>&1 || \
    nmcli connection add type ethernet ifname usb0 con-name usb0 \
        ipv4.method shared ipv6.method ignore autoconnect yes
# enforce l'adresse (nouveaux profils ET installs existants)
nmcli connection modify usb0 ipv4.method shared ipv4.addresses 10.42.1.1/24 ipv6.method ignore
nmcli connection up usb0 2>/dev/null || true

# === Radio LoRa SX1262 (Waveshare Core1262-868M) : noeud Meshtastic natif ===
# La radio est sur un bus SPI1 dedie (SPI0 etant pris par l'ecran + tactile).
if ! grep -q "spi1-1cs,cs0_pin=26" /boot/firmware/config.txt; then
    cat >> /boot/firmware/config.txt <<'EOF'

# === Radio LoRa SX1262 (Waveshare Core1262-868M) sur bus SPI1 dedie ===
# CE materiel relogé sur GPIO26 (non cable) pour ne pas toucher GPIO18=backlight ni GPIO16=CS
dtoverlay=spi1-1cs,cs0_pin=26
EOF
fi

# Depot meshtasticd (OBS) : pas de canal stable pour trixie -> on prend beta/Debian_13
apt-get install -y gnupg curl ca-certificates
MESHTASTIC_DIST=Debian_13
if [ ! -f "/etc/apt/sources.list.d/network:Meshtastic:beta.list" ]; then
    curl -fsSL "https://download.opensuse.org/repositories/network:Meshtastic:beta/${MESHTASTIC_DIST}/Release.key" \
        | gpg --dearmor | tee /etc/apt/trusted.gpg.d/network_Meshtastic_beta.gpg >/dev/null
    echo "deb http://download.opensuse.org/repositories/network:/Meshtastic:/beta/${MESHTASTIC_DIST}/ /" \
        > "/etc/apt/sources.list.d/network:Meshtastic:beta.list"
    apt-get update -y || true
fi
apt-get install -y meshtasticd

# Config radio (pinout SPI1 + TCXO DIO3 + RF switch RXEN/TXEN)
install -m 644 "$SRC/config/lora.yaml" /etc/meshtasticd/config.d/lora.yaml
systemctl enable meshtasticd
systemctl restart meshtasticd || true

# CLI meshtastic (pipx) pour piloter le noeud via l'API TCP locale 127.0.0.1:4403
apt-get install -y pipx
sudo -u "$U" pipx install meshtastic || true

# Region EU_868 (best-effort : le demon doit etre up ; rejouable sans effet de bord)
sleep 8
sudo -u "$U" "$H/.local/bin/meshtastic" --host 127.0.0.1 --set lora.region EU_868 || true

# Bluetooth : appairage/scan BLE + console serie SPP (RFCOMM)
# bluez fournit bluetoothctl/btmgmt/rfcomm/sdptool ; le mode --compat est requis
# pour publier le profil Serial Port via sdptool.
apt-get install -y bluez
install -d /etc/systemd/system/bluetooth.service.d
install -m 644 "$SRC/deploy/bluetooth-compat.conf" /etc/systemd/system/bluetooth.service.d/bluetooth-compat.conf
install -m 644 "$SRC/deploy/meshui-btserial.service" /etc/systemd/system/meshui-btserial.service
systemctl daemon-reload
systemctl restart bluetooth.service || true

# Services systemd
install -m 644 "$SRC/deploy/meshui.service" /etc/systemd/system/meshui.service
install -m 644 "$SRC/deploy/meshui-splash.service" /etc/systemd/system/meshui-splash.service
systemctl daemon-reload
systemctl enable meshui-splash.service meshui.service
systemctl start meshui-splash.service meshui.service || true

echo "=== provision done $(date) ==="
