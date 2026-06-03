#!/bin/bash
# Pull du depot + rebuild + restart de meshui.
# Lance via sudo (NOPASSWD sur meshui-ctl). Les operations user (git, cmake)
# sont relayees via "sudo -u" pour conserver les bons droits.
set -e

U=bq-lora
SRC=/home/$U/meshui
LOG=/var/log/meshui-update.log

exec >>"$LOG" 2>&1
echo "=== update $(date) ==="

cd "$SRC"

# Fast-forward depuis origin/master (descarte les modifs locales)
sudo -u "$U" git fetch -q origin master
sudo -u "$U" git reset --hard origin/master

# Rebuild incremental (LVGL deja compile, seuls les fichiers modifies recompilent)
sudo -u "$U" cmake --build build --target meshui -j2

# Reinstalle les artefacts deploy/ s'ils ont change
install -m 755 deploy/meshui-ctl                /usr/local/sbin/meshui-ctl
install -m 755 deploy/usb-ncm-setup.sh          /usr/local/sbin/meshui-usb-gadget
install -m 755 deploy/usb-hid-setup.sh          /usr/local/sbin/meshui-usb-hid
install -m 755 deploy/usb-storage-setup.sh      /usr/local/sbin/meshui-usb-storage
install -m 755 deploy/backlight-init.sh         /usr/local/sbin/meshui-backlight-init
install -m 755 deploy/meshui-update.sh          /usr/local/sbin/meshui-update 2>/dev/null || true

systemctl restart meshui
echo "=== update done ==="
