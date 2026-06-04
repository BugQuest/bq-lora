#!/bin/sh
# Force l'activation du profil usb0 (gadget NCM, mode shared 10.42.1.1) apres le
# boot. NM ne l'auto-active pas : c'est une interface "serveur" sans carrier au
# demarrage (le carrier n'arrive que lorsque le lien est monte) -> sans ce coup
# de pouce, NM laisse usb0 inactif et le PC voit "cable reseau debranche".
#
# On retente le temps que NM soit pret et que l'interface existe.
for _ in $(seq 1 20); do
    if nmcli -t -f NAME,STATE connection show --active 2>/dev/null \
         | grep -q '^usb0:activated'; then
        exit 0
    fi
    nmcli connection up usb0 >/dev/null 2>&1 && exit 0
    sleep 2
done
exit 0
