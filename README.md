# meshtastic-screen

Écran de commande tactile pour un futur nœud **Meshtastic LoRa**, basé sur un
**Raspberry Pi Zero 2 W** et un écran **MKS TS35-R V2.0** (3.5" 480×320).

L'interface est développée en **C avec LVGL** et rendue directement sur le
framebuffer (`/dev/fb0`), sans serveur graphique (OS en version *Lite*).

> État actuel : base matérielle (affichage + rétroéclairage + tactile) validée.
> UI de test LVGL en cours de mise au point. La liaison LoRa/Meshtastic viendra
> plus tard (l'antenne n'est pas encore disponible).

---

## Matériel

| Élément        | Détail |
|----------------|--------|
| Calculateur    | Raspberry Pi Zero 2 W (hostname `bq-lora`) |
| OS             | Raspberry Pi OS **Lite 64-bit, trixie** (noyau 6.12) |
| Écran          | MKS TS35-R V2.0 — 480×320, contrôleur **ILI9488** (SPI) |
| Tactile        | **XPT2046** résistif (SPI) — driver Linux `ads7846` |
| Extras         | Beeper (GPIO17), rétroéclairage BLK (GPIO18) |

### Câblage (bus matériel SPI0)

Les labels « SPI1 » sérigraphiés sur l'écran correspondent en réalité au
**SPI0** du Pi (GPIO 7/8/9/10/11). Branchement groupé par connecteur de l'écran.

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

### Accès SSH par USB (gadget ethernet)

En plus du WiFi, le Pi est configuré en **gadget USB ethernet** (`dwc2` +
`g_ether`) : relier le **port USB *data*** du Pi (micro-USB du milieu, marqué
`USB`, pas `PWR IN`) à un port USB du PC donne un accès `ssh bqlora` filaire,
indépendant du WiFi. Alimenter le Pi par le port `PWR IN` en parallèle pour
éviter tout manque de courant.

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

## Application LVGL

### Dépendances (sur le Pi)

```bash
sudo apt update
sudo apt install -y build-essential cmake git
```

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
├── app/
│   ├── main.c            # UI LVGL (démo de test pour l'instant)
│   └── CMakeLists.txt    # build CMake (lie lvgl + lvgl_demos)
├── config/
│   └── config.txt.append # overlays fbtft + ads7846 à ajouter sur le Pi
└── docs/
    └── lv_conf.md         # modifications à appliquer à lv_conf.h
```

---

## Feuille de route

- [x] Afficheur ILI9488 fonctionnel (driver `ili9486`, `/dev/fb0`)
- [x] Rétroéclairage automatique (`led_pin=18`)
- [x] Tactile ADS7846 détecté (`evdev`)
- [ ] Build LVGL + rendu à l'écran (en cours)
- [ ] UI de test custom (titre, boutons, zone messages, indicateurs)
- [ ] Calibration tactile (`lv_evdev_set_calibration`)
- [ ] Pilotage beeper (GPIO17)
- [ ] Gestion luminosité (PWM sur GPIO18)
- [ ] Intégration Meshtastic (liaison série/BLE vers le nœud LoRa)
