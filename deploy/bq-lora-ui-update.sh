#!/bin/bash
# Pull du depot + rebuild + restart de bq-lora-ui.
# Lance via sudo (NOPASSWD sur bq-lora-ui-ctl). Les operations user (git, cmake)
# sont relayees via "sudo -u" pour conserver les bons droits.
set -e

U=bq-lora
SRC=/home/$U/bq-lora-ui
LOG=/var/log/bq-lora-ui-update.log
PROGRESS=/tmp/bq-lora-ui-update.progress

# Publie un jalon de progression (0..100) lu par l'UI pour la barre de chargement.
progress() { echo "$1" > "$PROGRESS" 2>/dev/null; chmod 644 "$PROGRESS" 2>/dev/null || true; }
# En cas d'echec (set -e), marque -1 pour que l'UI affiche l'erreur.
trap 'progress -1' ERR

exec >>"$LOG" 2>&1
echo "=== update $(date) ==="

progress 5
cd "$SRC"

# Fast-forward depuis origin/master (descarte les modifs locales)
sudo -u "$U" git fetch -q origin master
progress 15
sudo -u "$U" git reset --hard origin/master
progress 25

# Reconfigure (le GLOB CMake doit voir les eventuels nouveaux .c de app/)
sudo -u "$U" cmake -S app -B build
progress 40
# Rebuild incremental (LVGL deja compile, seuls les fichiers modifies recompilent)
sudo -u "$U" cmake --build build --target bq-lora-ui -j2
progress 85

# Reinstalle les artefacts deploy/ s'ils ont change
install -m 755 deploy/bq-lora-ui-ctl                /usr/local/sbin/bq-lora-ui-ctl
install -m 755 deploy/usb-ncm-setup.sh          /usr/local/sbin/bq-lora-ui-usb-gadget
install -m 755 deploy/usb-hid-setup.sh          /usr/local/sbin/bq-lora-ui-usb-hid
install -m 755 deploy/usb-storage-setup.sh      /usr/local/sbin/bq-lora-ui-usb-storage
install -m 755 deploy/backlight-init.sh         /usr/local/sbin/bq-lora-ui-backlight-init
install -m 755 deploy/bq-lora-ui-update.sh          /usr/local/sbin/bq-lora-ui-update 2>/dev/null || true
install -m 755 deploy/shutdown-splash.sh        /usr/local/sbin/bq-lora-ui-shutdown-splash 2>/dev/null || true
install -m 755 deploy/usb-net-up.sh             /usr/local/sbin/bq-lora-ui-usb-net-up 2>/dev/null || true

# Dispatcher NM qui maintient usb0 (gadget) actif (root:root 755 obligatoire)
install -d /etc/NetworkManager/dispatcher.d
install -m 755 -o root -g root deploy/usb0-keepup.sh \
    /etc/NetworkManager/dispatcher.d/90-bq-lora-ui-usb0 2>/dev/null || true
# ignore-carrier sur usb0 : NM le monte des le boot sans attendre de carrier
# (sinon deadlock -> PC voit "cable debranche")
install -m 644 deploy/usb0-ignore-carrier.conf \
    /etc/NetworkManager/conf.d/10-usb0-ignore-carrier.conf 2>/dev/null || true
nmcli general reload 2>/dev/null || true

# Reinstalle les unites systemd + drop-ins s'ils ont change, puis recharge.
for u in bq-lora-ui.service bq-lora-ui-splash.service bq-lora-ui-shutdown.service \
         bq-lora-ui-btserial.service backlight.service usb-gadget.service \
         usb-net-up.service; do
    [ -f "deploy/$u" ] && install -m 644 "deploy/$u" "/etc/systemd/system/$u"
done
if [ -f deploy/bluetooth-compat.conf ]; then
    install -d /etc/systemd/system/bluetooth.service.d
    install -m 644 deploy/bluetooth-compat.conf \
        /etc/systemd/system/bluetooth.service.d/bluetooth-compat.conf
fi
systemctl daemon-reload
systemctl enable bq-lora-ui-shutdown.service 2>/dev/null || true
systemctl enable usb-net-up.service 2>/dev/null || true
progress 99

systemctl restart bq-lora-ui
echo "=== update done ==="
