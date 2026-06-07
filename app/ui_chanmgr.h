#pragma once
#include "lvgl/lvgl.h"

/* Gestionnaire de canaux (modal plein-ecran). Detache de ui.c.
 *  - ui_chanmgr_open_e : ouvre le modal (callback de bouton)
 *  - ui_chanmgr_refresh_if_open : a appeler depuis le tick mesh_refresh quand
 *    les canaux ont change, pour rafraichir la liste si le modal est encore
 *    affiche. Tester l'ouverture est fait en interne. */
void ui_chanmgr_open_e(lv_event_t *e);
void ui_chanmgr_refresh_if_open(void);
