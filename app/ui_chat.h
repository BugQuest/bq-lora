#pragma once
#include "lvgl/lvgl.h"

/* Vue CHAT : strip canaux + liste des messages + barre de composition.
 * Detache de ui.c. msg_seen / msg_seen_ch sont aussi declares dans
 * ui_common.h car la vue HOME les lit pour le badge non-lus global. */

void ui_chat_build(void);
void ui_chat_reset(void);

/* Reconstruit la liste des messages du canal courant si la vue est
 * actuellement affichee. Appele depuis mesh_refresh_cb. */
void ui_chat_rebuild_if_visible(void);

/* Handler attache au clavier virtuel : envoie le message a LV_EVENT_READY
 * si on est dans le compose CHAT, puis cache le clavier dans tous les cas. */
void ui_chat_kb_event_cb(lv_event_t *e);
