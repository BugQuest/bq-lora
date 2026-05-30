#!/bin/bash
# (Re)monte un gadget USB HID (clavier) via configfs. Bascule du mode reseau
# vers le mode BadUSB. Idempotent : teardown puis reconstruction.
set -e

NAME=bqlora
GADGET=/sys/kernel/config/usb_gadget/$NAME

modprobe libcomposite
mount -t configfs none /sys/kernel/config 2>/dev/null || true

# Teardown
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
echo "BugQuest"               > strings/0x409/manufacturer
echo "BugQuest Lora KB"       > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo "HID Keyboard" > configs/c.1/strings/0x409/configuration
echo 250            > configs/c.1/MaxPower

mkdir -p functions/hid.usb0
echo 1 > functions/hid.usb0/protocol
echo 1 > functions/hid.usb0/subclass
echo 8 > functions/hid.usb0/report_length
# Report descriptor : clavier standard 8 octets (modifiers + 6 keycodes)
printf '\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' > functions/hid.usb0/report_desc

ln -sf functions/hid.usb0 configs/c.1/

UDC=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
    [ -n "$UDC" ] && break
    sleep 0.5
done
[ -z "$UDC" ] && { echo "aucun UDC trouve"; exit 1; }
echo "$UDC" > UDC

# /dev/hidg0 -> rendre accessible au groupe video (bq-lora en est membre)
for _ in 1 2 3 4 5; do
    [ -c /dev/hidg0 ] && break
    sleep 0.2
done
chgrp video /dev/hidg0 2>/dev/null || true
chmod g+w   /dev/hidg0 2>/dev/null || true

echo "gadget HID monte sur $UDC (/dev/hidg0)"
