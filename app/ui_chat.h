#pragma once
#include "lvgl/lvgl.h"
#include <stdbool.h>

/* Vue CHAT a deux niveaux : liste des conversations (canaux) puis chat du canal
 * choisi. Detache de ui.c. msg_seen / msg_seen_ch sont aussi declares dans
 * ui_common.h car la vue HOME les lit pour le badge non-lus global. */

void ui_chat_build(void);
void ui_chat_reset(void);

/* Rafraichit la vue CHAT courante (liste de conversations ou messages) si elle
 * est affichee. Appele depuis mesh_refresh_cb. */
void ui_chat_rebuild_if_visible(void);

/* Force le retour a la liste des conversations au prochain build de l'onglet.
 * A appeler quand on entre dans CHAT depuis une autre app. */
void ui_chat_enter_tab(void);

/* Gestion du bouton retour topbar : true si on a quitte un chat pour revenir a
 * la liste (retour "intercepte"), false si rien a faire (-> retour HOME). */
bool ui_chat_back(void);

/* Handler attache au clavier virtuel : envoie le message a LV_EVENT_READY
 * si on est dans le compose CHAT, puis cache le clavier dans tous les cas. */
void ui_chat_kb_event_cb(lv_event_t *e);
