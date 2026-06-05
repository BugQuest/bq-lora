# Configuration LVGL (`lv_conf.h`)

`lv_conf.h` n'est pas versionné directement (gros fichier généré depuis le template
LVGL). Il se crée sur le Pi à partir du template, avec les modifications ci-dessous.

```bash
cp ~/lvgl/lv_conf_template.h ~/bq-lora-ui/lv_conf.h
```

Puis éditer `~/bq-lora-ui/lv_conf.h` :

| Ligne d'origine                              | Nouvelle valeur            | Raison |
|----------------------------------------------|----------------------------|--------|
| `#if 0` (tout en haut)                       | `#if 1`                    | Active la conf |
| `#define LV_COLOR_DEPTH 32`                  | `16`                       | Framebuffer RGB565 16 bits |
| `#define LV_MEM_SIZE (64 * 1024U)`           | `(1024 * 1024U)`           | Pool suffisant pour la démo widgets |
| `#define LV_USE_LINUX_FBDEV 0`               | `1`                        | Sortie framebuffer |
| `#define LV_LINUX_FBDEV_DEVICE "/dev/fb0"`   | (vérifier `/dev/fb0`)      | Périphérique écran |
| `#define LV_USE_EVDEV 0`                     | `1`                        | Entrée tactile |
| `#define LV_USE_DEMO_WIDGETS 0`              | `1`                        | Démo de test (temporaire) |

> `LV_USE_DEMO_WIDGETS` pourra repasser à `0` une fois l'UI Meshtastic en place.
