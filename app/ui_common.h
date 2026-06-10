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
#include <stdbool.h>

/* IDs d'app (cur_tab garde son nom mais semantique = app courante) */
enum {
    APP_HOME    = 0,
    APP_CHAT    = 1,
    APP_NODES   = 2,
    APP_SYS     = 3,
    APP_WIFI    = 4,
    APP_HOTSPOT = 5,
    APP_BADUSB  = 6,
    APP_BT      = 7,
    APP_ABOUT   = 8,
    APP_CAMERA  = 9,
    APP_GALLERY = 10,
    APP_DIAG    = 11,
    APP_GPS     = 12,
    APP_MAP     = 13,
};

extern int       cur_tab;          /* app courante (cf. enum APP_*) */

/* Etat CHAT partage avec HOME (badge non-lus du hub). Definis dans ui_chat.c. */
#define UI_MAX_CHANS 8
extern uint8_t   cur_chan;         /* canal courant (vue CHAT) */
extern unsigned  msg_seen;         /* compteur "lu" global (badge HOME) */
extern unsigned  msg_seen_ch[UI_MAX_CHANS];  /* compteurs "lu" par canal */

/* Clavier virtuel partage entre tous les modals/views qui prennent du texte. */
extern lv_obj_t *kb;

/* Zone centrale (sous la topbar) : reconstruite a chaque changement d'onglet
 * via lv_obj_clean. Les fonctions build_* y creent l'arbo d'objets. */
extern lv_obj_t *content;

/* Timer de rafraichissement de la vue courante (sys, hotspot, badusb...).
 * Une seule vue actif a la fois -> on reutilise le meme slot. show_tab le
 * delete et le set a NULL avant chaque rebuild. */
extern lv_timer_t *sys_refresh_timer;

/* ---- Helpers UI (definis dans ui.c) ---- */
void           flat(lv_obj_t *o);
void           panel(lv_obj_t *o, uint32_t border_color);
lv_obj_t      *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t      *small_button(lv_obj_t *parent, const char *txt, uint32_t color, lv_event_cb_t cb);

/* Modale de confirmation OUI/NON. on_yes peut etre NULL (informatif seul). */
void           confirm_dialog(const char *msg, void (*on_yes)(void));

/* Champ de saisie label + textarea avec ouverture du clavier au tap.
 * pwd=true active le masquage des caracteres. */
lv_obj_t      *settings_field(lv_obj_t *parent, const char *key, const char *val, bool pwd);

/* Handler interne utilise par settings_field (expose pour permettre a un module
 * separe d'attacher ce focus a son propre textarea s'il en cree un a la main). */
void           set_ta_focus_e(lv_event_t *e);
