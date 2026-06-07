#pragma once

/* En-tete partage par tous les modules d'UI (ui.c + ui_audio.c, ui_node.c, etc).
 *
 * Expose :
 *  - les helpers de mise en page deja utilises partout (flat/panel/label/small_button)
 *  - le clavier virtuel global (kb)
 *  - le dialog de confirmation (confirm_dialog)
 *
 * Les helpers sont definis une seule fois dans ui.c. Tout module les utilise
 * via cet include, sans avoir besoin de connaitre la structure interne. */

#include "lvgl/lvgl.h"
#include "theme.h"
#include "i18n.h"
#include "config.h"
#include <stdint.h>

/* Clavier virtuel partage entre tous les modals/views qui prennent du texte. */
extern lv_obj_t *kb;

/* ---- Helpers UI (definis dans ui.c) ---- */
void           flat(lv_obj_t *o);
void           panel(lv_obj_t *o, uint32_t border_color);
lv_obj_t      *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t      *small_button(lv_obj_t *parent, const char *txt, uint32_t color, lv_event_cb_t cb);

/* Modale de confirmation OUI/NON. on_yes peut etre NULL (informatif seul). */
void           confirm_dialog(const char *msg, void (*on_yes)(void));
