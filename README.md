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

Les labels « SPI1 » de l'écran correspondent en réalité au **SPI0** du Pi.

| Signal écran | GPIO Pi | Rôle              |
|--------------|---------|-------------------|
| spi1_cs      | 8       | CE0 → écran (`spi0.0`) |
| touch_cs     | 7       | CE1 → tactile (`spi0.1`) |
| spi1_sck     | 11      | SCLK              |
| spi1_mosi    | 10      | MOSI              |
| spi1_miso    | 9       | MISO              |
| spi1_rs      | 24      | DC (data/command) |
| tft_reset    | 25      | Reset écran       |
| touch_int    | 5       | PENIRQ tactile    |
| tft_blk      | 18      | Rétroéclairage    |
| beeper       | 17      | Buzzer            |
| board_5v     | 5V      | Alimentation      |
| gnd          | GND     | Masse             |

> ⚠️ Alimentation 5V mais **logique 3.3V**. Vérifier que les lignes qui
> reviennent vers le Pi (MISO/GPIO9 et INT/GPIO5) ne dépassent pas 3.3V.

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
