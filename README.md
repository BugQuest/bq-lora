# meshtastic-screen

**BugQuest // LORA** — nœud **Meshtastic LoRa** autonome à écran tactile, bâti
sur un **Raspberry Pi Zero 2 W** et un écran **MKS TS35-R V2.0** (3.5" 480×320).

L'appareil est un nœud Meshtastic complet (radio **SX1262** 868 MHz pilotée par
`meshtasticd`) doublé d'une interface de commande **C / LVGL** rendue directement
sur le framebuffer (`/dev/fb0`), sans serveur graphique (OS *Lite*). Tout se pilote
au doigt sur l'écran, sans CLI ni application tierce.

### Fonctionnalités

- **Messagerie mesh** — canaux public + chiffrés (PSK AES256), envoi/réception de
  texte avec ACK, badge de messages non lus, clavier virtuel.
- **Nœuds** — liste temps réel des nœuds vus (nom, SNR/RSSI, batterie, sauts,
  dernier contact).
- **Gestion des canaux** — créer / renommer / supprimer / partager (QR + URL
  `meshtastic.org/e/#…`) / importer, intégralement depuis l'écran.
- **Réseau** — client WiFi (scan + connexion), hotspot WiFi (avec QR d'appairage),
  accès SSH filaire par **gadget USB CDC NCM** (compatible Windows 11).
- **Bluetooth** — scan/appairage BLE et **console série** (SPP/RFCOMM) pour piloter
  le Pi depuis un terminal Bluetooth (cf. [docs/bluetooth.md](docs/bluetooth.md)).
- **Bad USB** — bascule du gadget en clavier **HID** (exécution de scripts type
  *DuckyScript*) ou en **stockage de masse** (dépôt de scripts depuis le PC), avec
  explorateur de fichiers intégré.
- **Caméra** — viseur **temps réel** (caméra CSI IMX219) directement dans l'UI,
  capture de photos en pleine résolution, et **galerie** de consultation
  (navigation, suppression) — le tout au doigt sur l'écran.
- **Système** — infos (CPU/RAM/disque/temp/alim/IP), alimentation, SSH, écran
  (luminosité PWM, calibration tactile 5 points), logs `journalctl`.
- **Réglages** — nom du nœud, SSID/passphrase du hotspot, fuseau horaire,
  activation du mesh — sans recompiler.
- **Mise à jour OTA** — `git pull` + rebuild + redémarrage depuis l'UI, avec barre
  de progression.

> **État : opérationnel.** Matériel (écran + rétroéclairage + tactile + radio)
> validé, radio LoRa EU_868 active @ 869.525 MHz, UI branchée sur l'API du nœud
> (`127.0.0.1:4403`) via un client protobuf natif en C. L'appareil démarre en
> *appliance* (services systemd, splash de boot/arrêt, console détachée).

---

## Matériel

| Élément        | Détail |
|----------------|--------|
| Calculateur    | Raspberry Pi Zero 2 W (hostname `bq-lora`) |
| OS             | Raspberry Pi OS **Lite 64-bit, trixie** (noyau 6.12) |
| Écran          | MKS TS35-R V2.0 — 480×320, contrôleur **ILI9488** (SPI) |
| Tactile        | **XPT2046** résistif (SPI) — driver Linux `ads7846` |
| Radio LoRa     | Waveshare **Core1262-868M** (puce **SX1262**, 868 MHz) sur SPI1 |
| Caméra         | Module CSI **Freenove IMX219** (8 MP, 3280×2464, objectif 120°) |
| Extras         | Beeper (GPIO17), rétroéclairage BLK (GPIO18) |

### Câblage (bus matériel SPI0)

Les labels « SPI1 » sérigraphiés sur l'écran correspondent en réalité au
**SPI0** du Pi (GPIO 7/8/9/10/11). Branchement groupé par connecteur de l'écran.

![Brochage et câblage écran ↔ Raspberry Pi Zero 2 W](pinout.png)

Le schéma ci-dessus reprend le sens de branchement EXP1/EXP2 et les numéros
de broche physique du GPIO Pi correspondants ; les tableaux qui suivent
listent chaque signal avec sa GPIO logique et son rôle.

**EXP1 :**

| Signal écran | GPIO Pi | Broche physique | Rôle |
|--------------|---------|-----------------|------|
| spi1_cs   | GPIO8  | 24 | CE0 → écran (`spi0.0`) |
| touch_cs  | GPIO7  | 26 | CE1 → tactile (`spi0.1`) |
| spi1_rs   | GPIO24 | 18 | DC (data/command) |
| tft_reset | GPIO25 | 22 | Reset écran |
| touch_int | GPIO5  | 29 | PENIRQ tactile |
| tft_blk   | GPIO18 | 12 | Rétroéclairage (BLK) |
| beeper    | GPIO17 | 11 | Buzzer |
| board_5v  | 5V     | 2 (ou 4) | Alimentation écran |
| gnd       | GND    | 6 (ou 9/14/20/25/30/34/39) | Masse |

**EXP2 :**

| Signal écran | GPIO Pi | Broche physique | Rôle |
|--------------|---------|-----------------|------|
| spi1_sck  | GPIO11 | 23 | SCLK |
| spi1_mosi | GPIO10 | 19 | MOSI |
| spi1_miso | GPIO9  | 21 | MISO |

> ⚠️ **Alimentation 5V mais logique 3.3V.** Vérifier que les lignes qui
> reviennent vers le Pi (`spi1_miso`/GPIO9 et `touch_int`/GPIO5) ne dépassent
> pas 3.3V.
>
> ⚠️ **Fiabilité des contacts tactiles.** `touch_int` (GPIO5) et `spi1_miso`
> (GPIO9) sont les deux lignes critiques du tactile — l'écran (write-only) ne se
> sert jamais du MISO, donc un MISO mal connecté donne « IRQ OK mais aucun
> point ». Sur fils dupont, ces contacts lâchent facilement à la manipulation :
> les fiabiliser (soudure / nappe / colle chaude) évite les rechutes.

### Accès SSH par USB (gadget réseau)

En plus du WiFi, le Pi expose un **gadget USB réseau CDC NCM** (`dwc2` + configfs,
descripteurs MS OS → reconnu nativement par **Windows 11**) : relier le **port USB
*data*** du Pi (micro-USB du milieu, marqué `USB`, pas `PWR IN`) à un port USB du PC
crée une interface `usb0` côté Pi en **10.42.1.1/24** (mode `shared` : le Pi sert le
DHCP + NAT). Le PC obtient un bail automatiquement et `ssh bq-lora@10.42.1.1`
fonctionne dès le boot, indépendamment du WiFi.

`usb0` est forcé actif au démarrage (`usb-net-up.service` + dispatcher NM
`90-meshui-usb0` + `ignore-carrier`) pour éviter le *deadlock* de carrier qui
laisserait Windows voir « câble réseau débranché ». Le sous-réseau 10.42.1.0/24 est
distinct de celui du hotspot WiFi (10.42.0.0/24) pour permettre la coexistence des
deux. Alimenter le Pi par le port `PWR IN` en parallèle évite tout manque de courant.

> Le gadget peut aussi être basculé en **clavier HID** ou **stockage de masse**
> depuis l'écran (menu BAD USB), au prix de la perte temporaire du lien réseau USB.

---

## Configuration système (Pi)

L'ILI9488 n'est pas supporté nativement par `fbtft`, mais le driver **`ili9486`
le pilote correctement**. Le rétroéclairage (`led_pin=18`) est **indispensable** :
sans lui l'écran reste noir alors que le rendu fonctionne.

Ajouter le contenu de [`config/config.txt.append`](config/config.txt.append) à la
fin de `/boot/firmware/config.txt`, puis redémarrer.

Vérification après reboot :

```bash
ls /dev/fb*                                   # /dev/fb0 attendu
dmesg | grep -iE 'fb_ili9486|ads7846'         # init driver + tactile
# test couleur (vert plein écran), le rétroéclairage doit être allumé :
python3 -c "open('/dev/fb0','wb').write(bytes([0xE0,0x07])*480*320)"
```

---

## Radio LoRa (SX1262)

Le nœud Meshtastic est constitué d'un module radio **Waveshare Core1262-868M**
(puce **SX1262** nue, 868 MHz) piloté directement par le Pi.

### Architecture

Le Core1262 n'est pas un nœud autonome : c'est un modem radio. Sur le Pi, on
fait donc tourner **`meshtasticd`** (le portage Linux natif de Meshtastic,
*portduino*), qui pilote le SX1262 en SPI et constitue un vrai nœud Meshtastic
local. L'interface LVGL se connecte à ce nœud via son **API TCP locale
(`127.0.0.1:4403`, protobuf)**, implémentée dans `app/mesh.c` (client natif C,
codec protobuf maison `app/pb.c`). (Cette approche remplace l'ancienne idée de
pont série : inutile puisque la radio est directement sur le Pi.)

Le client est entièrement piloté par la boucle LVGL (socket non bloquant,
`mesh_poll()`, aucun thread) : *handshake* `want_config_id`, réception des
`FromRadio` (my_info / node_info / channel / config / paquets texte), envoi des
`ToRadio` (paquets texte + heartbeat), ACK via paquets ROUTING, reconnexion
automatique. Le codec `pb.c` ne dépend d'aucune lib (pas de nanopb) : empreinte
minimale, juste les champs utiles du protocole Meshtastic.

### Câblage — bus SPI1 dédié

Le bus **SPI0 est entièrement occupé** par l'écran (CE0) et le tactile (CE1).
La radio est donc placée sur le **SPI1 auxiliaire**, dont les lignes de données
(GPIO 19/20/21) sont libres. Aucune contention avec le trafic écran à 24 MHz.

> ⚠️ **Alimentation 3.3V UNIQUEMENT.** Le SX1262 est en logique 3.3V et grille
> au-delà — contrairement à l'écran qui est alimenté en 5V. Brancher le VCC du
> module sur une broche **3V3**, jamais sur 5V.

Brochage relevé sur le module (deux rangées) : `ANT, GND, CS, CLK, MOSI, MISO,
RESET, BUSY` d'un côté ; `GND, GND, RXEN, TXEN, DIO2, DIO1, GND, 3V3` de l'autre.
Le module expose **RXEN/TXEN/DIO2 séparément** (commutateur RF piloté par l'hôte)
et n'a **pas de DIO3** (TCXO interne).

| Signal module | GPIO Pi (BCM) | Broche physique | Rôle / clé meshtasticd |
|---------------|---------------|-----------------|------------------------|
| 3V3   | 3V3    | 17 | Alimentation (**3.3V**) |
| GND   | GND    | 39 | Masse (plusieurs GND sur le module, un seul suffit) |
| CLK   | GPIO21 | 40 | SCLK (via `spidev1.0`) |
| MOSI  | GPIO20 | 38 | MOSI (via `spidev1.0`) |
| MISO  | GPIO19 | 35 | MISO (via `spidev1.0`) |
| CS    | GPIO16 | 36 | Chip select → `CS: 16` |
| DIO1  | GPIO13 | 33 | IRQ radio → `IRQ: 13` |
| BUSY  | GPIO12 | 32 | Busy → `Busy: 12` |
| RESET | GPIO6  | 31 | Reset → `Reset: 6` |
| RXEN  | GPIO22 | 15 | RF RX enable → `RXen: 22` |
| TXEN  | GPIO23 | 16 | RF TX enable → `TXen: 23` |
| DIO2  | *non câblé* | — | laissé NC (commutateur RF géré par RXEN/TXEN) |
| ANT   | *antenne* | — | **ne jamais émettre sans antenne 868 MHz** |

> **Commutateur RF.** RXEN/TXEN/DIO2 étant sortis séparément et non pontés sur ce
> module, on pilote **RXEN et TXEN depuis deux GPIO du Pi** (config `RXen`/`TXen`,
> comme l'E22-900M30S) plutôt que d'utiliser `DIO2_AS_RF_SWITCH` (qui exigerait de
> souder un pont DIO2↔TXEN). DIO2 reste non câblé.
>
> Dans meshtasticd, `CS`/`IRQ`/`Busy`/`Reset`/`RXen`/`TXen` sont des **GPIO pilotés
> en libgpiod**, indépendants du chip-select matériel du spidev (qui ne fournit que
> CLK/MOSI/MISO). C'est pourquoi le CE matériel de `spidev1.0` est relogé sur
> **GPIO26 (non câblé)** dans l'overlay, pour ne pas entrer en conflit avec GPIO18
> (backlight) ni GPIO16 (CS).
>
> Le Core1262 embarque un **TCXO** alimenté par DIO3 (confirmé par la spec
> Waveshare) → `DIO3_TCXO_VOLTAGE: true` est requis ; sans ce flag la radio
> n'initialise pas.

### Mise en service

Sur une installation neuve, [`deploy/provision.sh`](deploy/provision.sh) automatise
tout ce qui suit (overlay, dépôt, install, config, région). Procédure manuelle /
de référence (validée sur Pi Zero 2 W, RPi OS trixie arm64) :

1. Ajouter l'overlay SPI1 à `/boot/firmware/config.txt` (déjà inclus dans
   [`config/config.txt.append`](config/config.txt.append)) puis redémarrer :
   ```
   dtoverlay=spi1-1cs,cs0_pin=26
   ```
   Vérifier : `ls /dev/spidev1*` → `/dev/spidev1.0` attendu.

2. Installer **meshtasticd** depuis le dépôt OBS. Il n'existe **pas** de canal
   *stable* pour Debian 13 (trixie) → on utilise **beta/Debian_13** :
   ```bash
   curl -fsSL 'https://download.opensuse.org/repositories/network:Meshtastic:beta/Debian_13/Release.key' \
     | gpg --dearmor | sudo tee /etc/apt/trusted.gpg.d/network_Meshtastic_beta.gpg >/dev/null
   echo 'deb http://download.opensuse.org/repositories/network:/Meshtastic:/beta/Debian_13/ /' \
     | sudo tee /etc/apt/sources.list.d/network:Meshtastic:beta.list
   sudo apt update && sudo apt install -y meshtasticd
   ```

3. Déposer la config radio [`config/lora.yaml`](config/lora.yaml) dans
   `/etc/meshtasticd/config.d/`, puis (re)démarrer et vérifier la détection :
   ```bash
   sudo cp config/lora.yaml /etc/meshtasticd/config.d/lora.yaml
   sudo systemctl enable --now meshtasticd
   journalctl -u meshtasticd -f      # attendu : « sx1262 init success »
   ```

4. Installer le CLI **meshtastic** (Python) et régler la région (bande 868) :
   ```bash
   sudo apt install -y pipx && pipx install meshtastic
   ~/.local/bin/meshtastic --host 127.0.0.1 --set lora.region EU_868
   ```
   Vérifier : la fréquence passe à **869.525 MHz** (LongFast, EU_868).

5. L'API TCP `127.0.0.1:4403` est alors consommée par l'UI (client protobuf natif
   `app/mesh.c`), qui affiche nœuds, canaux et messages réels.

---

## Caméra CSI (IMX219)

Le module caméra (Freenove IMX219) est branché sur le **port CSI** du Pi et piloté
par **`libcamera` / `rpicam-apps`**. L'auto-détection (`camera_auto_detect`) échouant
sur ce clone, on force l'overlay explicite **`dtoverlay=imx219`** (déjà inclus dans
[`config/config.txt.append`](config/config.txt.append) et posé par `provision.sh`).

Vérification après reboot :

```bash
rpicam-hello --list-cameras            # le capteur imx219 doit apparaître
```

Côté UI :

- **Viseur live** — `rpicam-vid` diffuse un flux **YUV420 256×192 ~12 fps** que
  `app/sys.c` convertit en RGB565 et pousse dans un `lv_canvas` (largeur multiple de
  64 → frame compacte, sans *padding* de stride). La caméra étant mono-accès, le flux
  est démarré à l'ouverture de la page et arrêté proprement au changement d'onglet.
- **Capture HD** — le bouton CAPTURE coupe brièvement le flux, prend une photo en
  pleine résolution via `rpicam-still` (enregistrée dans `~/meshui/photos/`), puis le
  live reprend.
- **Galerie** — parcours des photos (navigation cyclique, suppression confirmée) ;
  chaque vignette est convertie en RGB565 par [`tools/cam.py`](tools/cam.py) (JPEG →
  buffer brut pour le canvas, vectorisé NumPy avec repli Python pur).

> Le dossier `~/meshui/photos/` est un répertoire **runtime** (jamais versionné).
> Si l'image reste noire, vérifier d'abord la **nappe CSI** (un défaut de contact
> donne `-EREMOTEIO` côté i2c : capteur non détecté malgré le bon driver).

---

## Application LVGL

### Dépendances (sur le Pi)

```bash
sudo apt update
sudo apt install -y build-essential cmake git rpicam-apps python3-pil
```

> `rpicam-apps` (viseur live + capture) et `python3-pil` (conversion des previews
> caméra) sont requis par l'app **Caméra/Galerie** ; `provision.sh` les installe
> automatiquement.

### Récupération de LVGL

LVGL n'est pas inclus dans ce dépôt. Le cloner et le placer dans `app/lvgl` :

```bash
cd ~
git clone https://github.com/lvgl/lvgl.git
cd lvgl && git checkout release/v9.2 && cd ~
# dans le dépôt de ce projet :
ln -s ~/lvgl app/lvgl       # ou copier le dossier dans app/lvgl
```

### Configuration LVGL

Créer `app/lv_conf.h` depuis le template et appliquer les modifications
documentées dans [`docs/lv_conf.md`](docs/lv_conf.md).

### Build & run

```bash
cd app
cmake -B build && cmake --build build -j2
sudo ./build/meshui
```

> Le premier build sur Pi Zero 2 W prend quelques minutes (compilation de LVGL).
> Le warning `ioctl(FBIOBLANK): Invalid argument` est bénin (fbtft ne supporte
> pas le blanking).

---

## Arborescence

```
meshtastic-screen/
├── README.md
├── .gitignore
├── app/                       # appli LVGL en C (compilée en ~/meshui/build/meshui)
│   ├── CMakeLists.txt         # GLOB sur *.c
│   ├── main.c                 # tick + display fbdev + touch + ui_init
│   ├── ui.c / ui.h            # toute l'UI : hub, chat, nodes, sys, badusb, bt, modales
│   ├── theme.h                # palette cyberpunk + polices
│   ├── mesh.c / mesh.h        # backend Meshtastic : client API TCP 4403 (protobuf natif)
│   ├── pb.c / pb.h            # codec protobuf minimal (wire format, sans nanopb)
│   ├── sys.c / sys.h          # infos système, actions privilégiées, WiFi/BT/USB/OTA async
│   ├── settings.c / .h        # réglages persistants (nom nœud, hotspot, fuseau, mesh)
│   ├── touch.c / touch.h      # pilote tactile maison (evdev + affine + lissage)
│   └── calib.c / calib.h      # calibrage 5 points moindres carrés
├── config/
│   ├── config.txt.append      # overlays fbtft + ads7846 + pwm + dwc2 + spi1 (LoRa) + imx219 (caméra)
│   └── lora.yaml              # config meshtasticd SX1262 (-> /etc/meshtasticd/config.d/)
├── deploy/                    # à installer sur le Pi
│   ├── provision.sh           # premier-boot : build + services + radio + sudoers
│   ├── meshui-update.sh       # mise à jour OTA : git reset + rebuild + réinstall + restart
│   ├── optimize-boot.sh       # optimisations du temps de boot (désactive cloud-init)
│   ├── meshui.service         # autostart de l'app
│   ├── meshui-splash.service  # boot splash + détache la console
│   ├── meshui-shutdown.service# splash d'arrêt/redémarrage
│   ├── shutdown-splash.sh      # rendu du splash d'arrêt (après libération de fb0)
│   ├── meshui-btserial.service# console série Bluetooth (SPP/RFCOMM)
│   ├── bluetooth-compat.conf  # bluetoothd --compat (requis pour sdptool/SPP)
│   ├── meshui-ctl             # helper privilégié (NOPASSWD limité)
│   ├── meshui-sudoers         # règle sudoers correspondante
│   ├── backlight-init.sh      # PWM GPIO18 init
│   ├── backlight.service      # systemd oneshot pour le PWM
│   ├── usb-gadget.service     # systemd pour le gadget USB (au boot)
│   ├── usb-ncm-setup.sh       # gadget USB CDC NCM — réseau (Win11 OK)
│   ├── usb-hid-setup.sh       # gadget USB HID — clavier (BadUSB)
│   ├── usb-storage-setup.sh   # gadget USB mass storage — dépôt de scripts
│   ├── usb-net-up.service     # force l'activation de usb0 après NetworkManager
│   ├── usb-net-up.sh          # script associé (retries nmcli connection up usb0)
│   ├── usb0-keepup.sh         # dispatcher NM : ré-active usb0 s'il tombe
│   └── usb0-ignore-carrier.conf# NM : monte usb0 sans attendre de carrier
├── tools/                     # utilitaires Python (pas dans le binaire C)
│   ├── splash.py              # boot splash PIL → fb0
│   ├── cam.py                 # JPEG → buffer brut RGB565 pour le canvas (preview/galerie)
│   ├── badusb.py              # interpréteur DuckyScript → frappes HID
│   ├── grab.py                # capture fb0 → PNG (dev)
│   ├── touchcal.py            # relevé tactile brut (dev)
│   └── beep.py                # tonalité piezo GPIO17 via gpiozero
└── docs/
    ├── lv_conf.md             # modifs lv_conf.h (gérées par provision.sh)
    └── bluetooth.md           # utilisation de la console série Bluetooth
```

---

## État des fonctionnalités

### Matériel & système

- [x] Écran ILI9488 (driver `ili9486`, `/dev/fb0`) en portrait 320×480
- [x] Rétroéclairage PWM matériel sur GPIO18 (overlay `pwm`)
- [x] Tactile XPT2046 (`ads7846`) — pilote maison evdev + lissage doigt
- [x] Calibrage tactile 5 points (affine moindres carrés, persistant)
- [x] Beeper GPIO17 piloté via `gpiozero`
- [x] Caméra CSI **IMX219** (overlay `dtoverlay=imx219`, `libcamera`/`rpicam-apps`) :
      viseur live + capture HD + galerie
- [x] Gadget USB multi-mode (configfs) : **CDC NCM** (réseau, Win11 OK), **HID**
      (clavier BadUSB), **mass storage** (dépôt de scripts) — commutable depuis l'UI
- [x] `usb0` forcé actif au boot (anti-deadlock carrier) → SSH USB fiable au reboot
- [x] Bluetooth : scan/appairage BLE + console série SPP/RFCOMM (`bluetoothd --compat`)
- [x] Hotspot WiFi (NetworkManager `shared`) + client WiFi (scan/connexion)
- [x] Mise à jour OTA depuis l'UI (`git` + rebuild + restart, barre de progression)
- [x] Démarrage automatique appliance (services systemd, splash boot/arrêt, console détachée)

### Interface (cyberpunk minimaliste)

- [x] Identité **BugQuest // LORA** (boot splash PIL + splash app LVGL animé)
- [x] Topbar : nom du nœud + horloge ; **barre d'état verticale** (icônes système +
      indicateurs LoRa, voir légende ci-dessous)
- [x] **Hub d'accueil** : grille de cartes (MESSAGES, NODES, WIFI, BLUETOOTH, HOTSPOT,
      BAD USB, CAMERA, GALERIE, SYSTÈME, À PROPOS) avec badge de messages non lus
- [x] **MESSAGES** : canaux public + chiffrés, fil de messages avec ACK, clavier virtuel,
      bouton **⚙ gestion des canaux** → modal complet (voir ci-dessous)
- [x] **NODES** : liste des nœuds **réels** (nom, SNR/RSSI, batterie, sauts, dernier contact)
- [x] **WIFI** : SSID/signal + modal scan + connexion avec saisie passphrase
- [x] **BLUETOOTH** : scan/appairage des périphériques + bascule de la console série
- [x] **HOTSPOT** : état + toggle + QR code WiFi pour scan téléphone
- [x] **BAD USB** : explorateur de scripts, bascule HID/STORAGE, exécution *DuckyScript*
- [x] **CAMERA** : viseur live (`rpicam-vid` YUV420 → RGB565 dans un canvas), bouton
      CAPTURE (photo pleine résolution `rpicam-still` → `~/meshui/photos/`), accès galerie
- [x] **GALERIE** : consultation des photos (canvas RGB565 via `tools/cam.py`),
      navigation précédent/suivant cyclique, suppression confirmée
- [x] **SYSTÈME** :
  - INFO (hostname, IPs wlan/usb, uptime, CPU temp, RAM, disque, alim, kernel)
  - ALIMENTATION (Éteindre / Redémarrer avec confirmation)
  - SSH (état + ACTIVER/DESACTIVER)
  - USB (mode + état réel + IP côté Pi)
  - ECRAN (slider luminosité PWM, BIP test, CALIBRER)
  - RÉGLAGES (nom du nœud, SSID/passphrase hotspot, fuseau, mesh on/off)
  - MISE À JOUR (vérifier + installer l'OTA)
  - APPLICATION (RELANCER MESHUI)
  - LOG SYSTÈME (`journalctl -n 30` scrollable + rafraîchir)
- [x] **À PROPOS** : matériel, logiciel, projet/auteur
- [x] Modal de calibration tactile (croix rouges, 5 points)

#### Barre d'état verticale (légende des icônes)

Fixée sur le bord **gauche** de l'écran, rafraîchie toutes les 1,5 s. De haut en bas :

| Icône | Rôle | États / couleurs |
|-------|------|------------------|
| 🔌 USB | Lien USB vers le PC | cyan = bail DHCP `usb0` actif ; atténué = non connecté |
| 📶 WiFi | État réseau WiFi | cyan = client connecté ; magenta = point d'accès (hotspot) ; atténué = aucun lien |
| *(séparateur)* | — | sépare le groupe système du groupe LoRa |
| ➤ Lien mesh | Liaison vers `meshtasticd` (API TCP 4403) | vert = établie et configurée ; magenta = absente |
| ▤ + n | Nombre de nœuds vus sur le mesh | valeur en clair (atténuée si lien mesh coupé) |
| ↻ + % | Utilisation du canal (air time) | atténué < 40 % ; ambre ≥ 40 % (canal chargé) |
| ⚡ / batterie | Batterie du nœud | symbole batterie gradué (vert > 20 %, ambre sinon) ; éclair atténué si pas d'alim batterie (cas portduino sur secteur) |

Les valeurs LoRa proviennent de `mesh_self()` (région, preset, batterie, utilisation canal, nœuds) alimenté par le flux protobuf de `meshtasticd`.

#### Gestion des canaux (modal ⚙)

Accessible depuis le menu **MESSAGES** via le bouton ⚙ en bout de la rangée de canaux. Le
modal liste tous les canaux (icône cadenas + couleur magenta si chiffré, étiquette de
rôle **PRIMAIRE** / **SECONDAIRE**) et offre une gestion complète, **sans CLI ni appli
tierce** :

| Action | Effet |
|--------|-------|
| **＋ PUBLIC** | crée un canal secondaire en clair (clé par défaut publique) |
| **＋ CHIFFRÉ** | crée un canal secondaire avec une clé **AES256 aléatoire** (32 octets `/dev/urandom`) |
| **RENOMMER** | renomme un canal (conserve clé et rôle) |
| **PARTAGER** | affiche le **QR code + URL** `https://meshtastic.org/e/#…` à scanner depuis l'appli officielle |
| **SUPPR.** | désactive un canal secondaire (le **primaire** est protégé : pas de bouton) |
| **⬇ IMPORTER** | colle une URL `meshtastic.org/e/#…` pour ajouter le(s) canal(aux) qu'elle contient |

Toutes les opérations passent par des **AdminMessage** (`set_channel`, encadrés par
`begin_edit_settings` / `commit_edit_settings`) envoyés au nœud local sur le port ADMIN.
La liste locale est rafraîchie ~1 s plus tard via un re-handshake `want_config`, ce qui
met à jour le modal automatiquement (pas besoin de le rouvrir). L'URL de partage encode
un `ChannelSet` protobuf (paramètres du canal + preset/région LoRa) en base64 *url-safe*,
identique au format de l'application Meshtastic officielle.

### Intégration Meshtastic (radio SX1262)

- [x] Choix d'archi : `meshtasticd` natif + API TCP locale 4403 (cf. [Radio LoRa](#radio-lora-sx1262))
- [x] Câblage radio (SPI1 dédié) + overlay `config.txt` + `config/lora.yaml`
- [x] Module branché, **SX1262 détecté** (`sx1262 init success`), région EU_868 @ 869.525 MHz
- [x] Provisionnement automatisé (meshtasticd + config + région) dans `provision.sh`
- [x] Pont UI ↔ API TCP 4403 (client protobuf natif C `mesh.c` + `pb.c`)
- [x] Données réelles (nœuds, canaux, messages) à la place du backend factice
- [x] Envoi/réception réels de texte sur LongFast + canaux PSK chiffrés
- [x] Gestion ACK (paquets ROUTING) et reconnexion automatique
- [x] État `mesh_self()` (région/preset/utilisation canal/batterie/nœuds) dans la barre d'état

### Sécurité

- [x] SSH par clé publique uniquement
- [x] Helper privilégié `meshui-ctl` + sudoers NOPASSWD strictement limité à ce binaire

### Déploiement

- [x] Workflow flash : Imager pour l'OS + cloud-init `user-data` + bundle tarball
- [x] `provision.sh` idempotent (apt + LVGL clone + lv_conf généré + build + services + radio)
- [x] Mise à jour OTA via `meshui-update.sh` (déclenchable depuis l'UI)
- [x] 3 voies d'accès SSH : WiFi Freebox, hotspot 10.42.0.1, gadget USB 10.42.1.1 (sous-réseaux distincts → coexistence hotspot + USB)

### Pistes d'évolution

- [ ] Position GPS des nœuds → vue carte hors-ligne (tuiles préchargées)
- [ ] Mode économie (extinction écran après inactivité, réveil au toucher)
- [ ] Carnet de contacts / nœuds favoris (alias, notes)
- [ ] Réglages radio dans l'UI (région, preset, hop limit)
- [ ] Variantes de thèmes (palettes cyberpunk alternatives)
- [ ] Graphes CPU/RAM/temp en temps réel (sparkline)
