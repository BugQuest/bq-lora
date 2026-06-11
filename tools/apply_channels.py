#!/usr/bin/env python3
"""Applique un ChannelSet (URL meshtastic.org/e/#...) en REMPLACANT TOTALEMENT
les canaux du noeud local, puis sa LoRaConfig embarquee.

Pourquoi ce helper plutot que `meshtastic --seturl` seul :
  setURL(addOnly=False) n'ecrit que les canaux PRESENTS dans l'URL (ex : 1 seul
  primaire) et NE DESACTIVE PAS les index 1..7 restants -> les secondaires d'une
  config precedente (Fr_Balise/Fr_EMCOM/... de Gaulix) survivaient et polluaient
  le chat apres un passage sur "LongFast public".

Methode (validee sur firmware 2.7.x) :
  PHASE 1 - vider les secondaires : tant qu'il reste un canal SECONDARY, on
    OUVRE une connexion neuve, on relit l'etat REEL du device, on supprime LE
    PREMIER secondaire trouve via deleteChannel() (compaction pop+decalage), puis
    on FERME. Une connexion = une suppression.
  PHASE 2 - ecrire la cible : une derniere connexion appelle setURL(url). Le
    device n'ayant plus que le primaire, setURL ecrit proprement le primaire +
    les secondaires de l'URL + la LoRaConfig embarquee (region/preset/hop/tx).

  POURQUOI une connexion par suppression (et pas une boucle dans une session) :
    Le cache node.channels local diverge de l'etat device des qu'on mute (la
    compaction cote firmware ne suit pas exactement le pop local). Enchainer
    plusieurs deleteChannel()/writeChannel() dans LA MEME session laisse alors
    des secondaires ressuscites - de facon non deterministe, et meme avec des
    sleeps (ce n'est pas une course mais un cache divergent). Rouvrir relit l'etat
    vrai a chaque fois et persiste a la fermeture : c'est ce qui rend l'operation
    fiable (c'est exactement ce que fait la CLI `--ch-del`, un process par del).
  On evite aussi beginSettingsTransaction() : en transaction le firmware
  bufferise les set_channel et la compaction de deleteChannel() ne s'applique pas.

Usage : apply_channels.py "<url>"
Sortie : "ok" + exit 0 si succes ; message d'erreur + exit !=0 sinon.
"""
import sys
import time
import base64


def main():
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        print("usage: apply_channels.py <url>", file=sys.stderr)
        return 2
    url = sys.argv[1].strip()

    from meshtastic.tcp_interface import TCPInterface
    from meshtastic.protobuf import apponly_pb2, channel_pb2
    SECONDARY = channel_pb2.Channel.Role.SECONDARY

    # --- valide l'URL tot (avant toute connexion) ---
    b64 = url.split("/#")[-1]
    b64 += "=" * ((4 - len(b64) % 4) % 4)   # re-padde (les URLs sont strippees)
    cs = apponly_pb2.ChannelSet()
    cs.ParseFromString(base64.urlsafe_b64decode(b64))
    if len(cs.settings) == 0:
        print("erreur: URL sans canaux", file=sys.stderr)
        return 3

    # --- PHASE 1 : vide les SECONDARY, une connexion neuve par suppression ---
    for _ in range(8):                       # borne dure (max 7 secondaires)
        iface = TCPInterface(hostname="127.0.0.1")
        try:
            node = iface.localNode
            victim = next((i for i in range(len(node.channels))
                           if node.channels[i].role == SECONDARY), None)
            if victim is None:
                break
            node.deleteChannel(victim)
        finally:
            try:
                iface.close()
            except Exception:
                pass
        time.sleep(0.5)                      # laisse le device persister/respirer

    # --- PHASE 2 : setURL sur etat propre (primaire + secondaires + lora) ---
    iface = TCPInterface(hostname="127.0.0.1")
    try:
        iface.localNode.setURL(url)          # addOnly=False par defaut
    finally:
        try:
            iface.close()
        except Exception:
            pass

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
