#!/bin/bash
# (Re)monte un gadget USB CDC NCM via configfs (compatible Windows 11).
# Idempotent : tear down de tout gadget existant puis reconstruction NCM.
set -e

NAME=bqlora
GADGET=/sys/kernel/config/usb_gadget/$NAME

# Pre-requis : decharger g_ether s'il est present, mettre libcomposite
if lsmod | grep -q '^g_ether '; then modprobe -r g_ether || true; fi
modprobe libcomposite
mount -t configfs none /sys/kernel/config 2>/dev/null || true

# Teardown : unbind + supprime symlinks/fonctions/strings/config pour repartir propre
if [ -d "$GADGET" ]; then
    echo "" > "$GADGET/UDC" 2>/dev/null || true
    for link in "$GADGET"/configs/*/*; do [ -L "$link" ] && rm "$link"; done
    [ -L "$GADGET/os_desc/c.1" ] && rm "$GADGET/os_desc/c.1"
    for fn in "$GADGET"/functions/*; do [ -d "$fn" ] && rmdir "$fn"; done
    for sdir in "$GADGET"/configs/c.1/strings/*; do [ -d "$sdir" ] && rmdir "$sdir"; done
    rmdir "$GADGET/configs/c.1" 2>/dev/null || true
    for sdir in "$GADGET"/strings/*; do [ -d "$sdir" ] && rmdir "$sdir"; done
    rmdir "$GADGET" 2>/dev/null || true
fi

mkdir -p "$GADGET"; cd "$GADGET"
echo 0x1d6b > idVendor
echo 0x0104 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
SN=$(tr -d '\0' < /sys/firmware/devicetree/base/serial-number 2>/dev/null | head -c 16)
echo "${SN:-0123456789abcdef}" > strings/0x409/serialnumber
echo "BugQuest"            > strings/0x409/manufacturer
echo "BugQuest Lora"       > strings/0x409/product

echo 1       > os_desc/use
echo 0xcd    > os_desc/b_vendor_code
echo MSFT100 > os_desc/qw_sign

mkdir -p configs/c.1/strings/0x409
echo "NCM"   > configs/c.1/strings/0x409/configuration
echo 250     > configs/c.1/MaxPower

mkdir -p functions/ncm.usb0
echo "5e:36:6f:8f:e9:74" > functions/ncm.usb0/dev_addr
echo "5e:36:6f:8f:e9:75" > functions/ncm.usb0/host_addr

ln -sf functions/ncm.usb0 configs/c.1/
ln -sf configs/c.1 os_desc/

UDC=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
    [ -n "$UDC" ] && break
    sleep 0.5
done
[ -z "$UDC" ] && { echo "aucun UDC trouve"; exit 1; }
echo "$UDC" > UDC

# Remonte l'image badusb sur le point de montage (l'app y lira ses scripts)
IMG=/home/bq-lora/bq-lora-ui/badusb.img
MNT=/home/bq-lora/bq-lora-ui/badusb
if [ -f "$IMG" ] && ! mountpoint -q "$MNT" 2>/dev/null; then
    mkdir -p "$MNT"
    mount -o loop,umask=000 "$IMG" "$MNT" 2>/dev/null || true
fi

echo "gadget NCM monte sur $UDC"
