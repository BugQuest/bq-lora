#pragma once
#include "lvgl/lvgl.h"

/* Modal "Reglages" : nom du noeud + SSID/pass hotspot + fuseau horaire.
 * Detache de ui.c. settings_field et set_ta_focus_e (utilises par d'autres
 * modules comme ui_chanmgr.c) sont declares dans ui_common.h. */
void ui_settings_open_e(lv_event_t *e);
