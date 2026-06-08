#pragma once

#include "lvgl/lvgl.h"

/* Modale "Reglages radio" :
 *  - region (EU_868, US, JP, ...)
 *  - modemPreset (LongFast, LongMod, MediumFast, ShortFast, ...)
 *  - hop_limit (1..7)
 *  - tx_power (0 = auto/max legal, sinon dBm)
 *
 * Sauvegarde de presets nommes dans /home/bq-lora/bq-lora-ui/radio_presets.tsv
 * (texte plat : nom<TAB>region<TAB>preset<TAB>hop<TAB>tx). Survit aux redemarrages.
 *
 * Apply -> meshtastic CLI (--set lora.* + --set device.* via system()) puis
 * mesh_refresh_config() pour que l'UI repere les changements. */
void ui_radio_open_e(lv_event_t *e);
