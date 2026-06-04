#!/bin/sh
# Dispatcher NetworkManager : garde le lien USB "usb0" (gadget NCM, mode shared
# 10.42.1.1) toujours pret a servir.
#
# Contexte : NM n'expose pas d'evenement "carrier" au dispatcher. On reagit donc
# au passage en "down"/"pre-down" de usb0 (debranchement du PC, perte de lien,
# course au boot) en re-activant le profil. Ainsi, des que le PC se rebranche,
# le Pi est deja en 10.42.1.1 avec son DHCP/NAT actif -> SSH direct, sans avoir
# a toggler quoi que ce soit cote Windows.
#
# Installe par provision.sh / meshui-update vers
#   /etc/NetworkManager/dispatcher.d/90-meshui-usb0
# (doit etre root:root, 755, non inscriptible par group/other, sinon NM l'ignore)
IFACE="$1"
ACTION="$2"

# Ne concerne que l'interface du gadget USB.
[ "$IFACE" = "usb0" ] || exit 0

case "$ACTION" in
    down|pre-down)
        # Re-up en arriere-plan + detache : le dispatcher doit rendre la main
        # vite (NM le tue au-dela de son timeout). Le court delai laisse NM
        # finir son teardown avant qu'on relance le profil.
        setsid sh -c '
            sleep 2
            nmcli -t -f NAME,STATE connection show --active 2>/dev/null \
                | grep -q "^usb0:activated" || nmcli connection up usb0
        ' >/dev/null 2>&1 &
        ;;
esac
exit 0
