#!/bin/bash
# Monte un gadget USB CDC NCM via configfs (compatible Windows 11 nativement).
# Idempotent. A executer comme root.
set -e

NAME=bqlora
GADGET=/sys/kernel/config/usb_gadget/$NAME

# Pre-requis : decharger g_ether s'il est present, mettre libcomposite a sa place
if lsmod | grep -q '^g_ether '; then
    modprobe -r g_ether || true
fi
modprobe libcomposite
mount -t configfs none /sys/kernel/config 2>/dev/null || true

# Si deja monte, ne rien refaire
if [ -d "$GADGET" ]; then
    echo "gadget deja configure"
    exit 0
fi

mkdir -p "$GADGET"
cd "$GADGET"

# Identifiants USB - composite multifonction Linux Foundation
echo 0x1d6b > idVendor
echo 0x0104 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

# Strings (descriptifs utilisateur)
mkdir -p strings/0x409
SN=$(tr -d '\0' < /sys/firmware/devicetree/base/serial-number 2>/dev/null | head -c 16)
echo "${SN:-0123456789abcdef}" > strings/0x409/serialnumber
echo "BugQuest"            > strings/0x409/manufacturer
echo "BugQuest Lora"       > strings/0x409/product

# Microsoft OS descriptors (aident Windows a charger le bon driver)
echo 1       > os_desc/use
echo 0xcd    > os_desc/b_vendor_code
echo MSFT100 > os_desc/qw_sign

# Configuration unique avec la fonction NCM
mkdir -p configs/c.1/strings/0x409
echo "NCM"   > configs/c.1/strings/0x409/configuration
echo 250     > configs/c.1/MaxPower

mkdir -p functions/ncm.usb0
# MACs stables pour eviter qu'Windows ne cree une nouvelle interface a chaque boot
echo "5e:36:6f:8f:e9:74" > functions/ncm.usb0/dev_addr
echo "5e:36:6f:8f:e9:75" > functions/ncm.usb0/host_addr

ln -sf functions/ncm.usb0 configs/c.1/
ln -sf configs/c.1 os_desc/

# Bind au UDC dwc2 (peut etre lent au boot froid, on attend un peu)
UDC=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
    [ -n "$UDC" ] && break
    sleep 0.5
done
[ -z "$UDC" ] && { echo "aucun UDC trouve"; exit 1; }
echo "$UDC" > UDC

echo "gadget NCM monte sur $UDC"
