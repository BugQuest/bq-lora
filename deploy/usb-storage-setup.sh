#!/bin/bash
# (Re)monte un gadget USB Mass Storage : expose ~/bq-lora-ui/badusb.img au PC
# pour qu'il apparaisse comme cle USB et permette de deposer des scripts BadUSB.
set -e

NAME=bqlora
GADGET=/sys/kernel/config/usb_gadget/$NAME
IMG=/home/bq-lora/bq-lora-ui/badusb.img
MNT=/home/bq-lora/bq-lora-ui/badusb

modprobe libcomposite
mount -t configfs none /sys/kernel/config 2>/dev/null || true

# Cree l'image si absente (au cas ou provision aurait saute)
if [ ! -f "$IMG" ]; then
    dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
    mkfs.vfat -F 16 -n BADUSB "$IMG" >/dev/null
    chown bq-lora:bq-lora "$IMG"
fi

# IMPORTANT : on doit demonter cote Pi avant d'exposer l'image (sinon corruption)
mkdir -p "$MNT"
umount "$MNT" 2>/dev/null || true

# Teardown existant
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
echo "BugQuest"              > strings/0x409/manufacturer
echo "BugQuest Lora Storage" > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo "Mass Storage" > configs/c.1/strings/0x409/configuration
echo 250            > configs/c.1/MaxPower

mkdir -p functions/mass_storage.usb0
echo 1 > functions/mass_storage.usb0/stall
echo 1 > functions/mass_storage.usb0/lun.0/removable
echo 0 > functions/mass_storage.usb0/lun.0/cdrom
echo 0 > functions/mass_storage.usb0/lun.0/ro
echo "$IMG" > functions/mass_storage.usb0/lun.0/file

ln -sf functions/mass_storage.usb0 configs/c.1/

UDC=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
    [ -n "$UDC" ] && break
    sleep 0.5
done
[ -z "$UDC" ] && { echo "aucun UDC trouve"; exit 1; }
echo "$UDC" > UDC
echo "gadget mass-storage monte sur $UDC ($IMG)"
