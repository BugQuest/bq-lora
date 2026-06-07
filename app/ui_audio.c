#include "ui_common.h"
#include "sys.h"
#include <stdio.h>
#include <stdint.h>

/* ---- Modal AUDIO : piano 8 notes + compose Morse ----
 *
 * S'ouvre via le bouton BIP de SYSTEME/REGLAGES/ECRAN (callback ui_audio_open_e
 * cable depuis ui.c). Permet de jouer des notes au doigt et d'envoyer du Morse
 * arbitraire (utile cote HAM, ou pour un SOS sonore en exterieur).
 *
 * Tout est file-local sauf le point d'entree ui_audio_open_e expose dans ui.h. */

static lv_obj_t *au_ov, *au_ta;
/* C4-C5 (gamme majeure) : DO RE MI FA SOL LA SI DO */
static const int au_freqs[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
static const char *au_names[8] = { "DO", "RE", "MI", "FA", "SOL", "LA", "SI", "DO" };

static void au_close(void) {
    if (au_ov) { lv_obj_delete(au_ov); au_ov = NULL; }
    au_ta = NULL;
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}
static void au_close_e(lv_event_t *e) { (void)e; au_close(); }
static void au_key_e(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 8) sys_beep(au_freqs[idx], 200);
}
static void au_morse_e(lv_event_t *e) {
    (void)e;
    if (!au_ta) return;
    const char *txt = lv_textarea_get_text(au_ta);
    if (txt && txt[0]) sys_morse(txt);
}
static void au_sos_e(lv_event_t *e) { (void)e; sys_morse("SOS"); }
static void au_ta_e(lv_event_t *e) {
    (void)e;
    lv_keyboard_set_textarea(kb, au_ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

void ui_audio_open_e(lv_event_t *e) {
    (void)e;
    if (au_ov) return;
    au_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(au_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(au_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(au_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(au_ov, 0, 0);
    lv_obj_set_style_radius(au_ov, 0, 0);
    lv_obj_set_style_pad_all(au_ov, 8, 0);
    lv_obj_clear_flag(au_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(au_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(au_ov, 6, 0);

    label(au_ov, tr(STR_AUDIO_TITLE), FONT_BIG, CY_MAGENTA);

    /* piano : 8 touches sur 1 ligne */
    label(au_ov, tr(STR_AUDIO_PIANO), FONT_SMALL, CY_AMBER);
    lv_obj_t *keys = lv_obj_create(au_ov);
    lv_obj_set_size(keys, LV_PCT(100), 60);
    flat(keys);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(keys, 3, 0);
    lv_obj_clear_flag(keys, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *k = lv_button_create(keys);
        lv_obj_set_flex_grow(k, 1);
        lv_obj_set_height(k, 58);
        lv_obj_set_style_radius(k, 2, 0);
        lv_obj_set_style_bg_color(k, lv_color_hex(CY_PANEL2), 0);
        lv_obj_set_style_border_color(k, lv_color_hex(CY_CYAN), 0);
        lv_obj_set_style_border_width(k, 1, 0);
        lv_obj_set_style_shadow_width(k, 0, 0);
        lv_obj_add_event_cb(k, au_key_e, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = label(k, au_names[i], FONT_SMALL, CY_CYAN);
        lv_obj_center(l);
    }

    /* morse compose */
    label(au_ov, tr(STR_AUDIO_MORSE), FONT_SMALL, CY_AMBER);
    au_ta = lv_textarea_create(au_ov);
    lv_textarea_set_one_line(au_ta, true);
    lv_textarea_set_placeholder_text(au_ta, tr(STR_AUDIO_MORSE_HINT));
    lv_obj_set_size(au_ta, LV_PCT(100), 34);
    lv_obj_set_style_bg_color(au_ta, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_text_color(au_ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(au_ta, FONT_BODY, 0);
    lv_obj_set_style_border_color(au_ta, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_border_width(au_ta, 1, 0);
    lv_obj_set_style_radius(au_ta, 2, 0);
    lv_obj_add_event_cb(au_ta, au_ta_e, LV_EVENT_CLICKED, NULL);

    /* barre boutons */
    lv_obj_t *row = lv_obj_create(au_ov);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    {
        char bp[24], bs[24];
        snprintf(bp, sizeof(bp), LV_SYMBOL_AUDIO "%s", tr(STR_AUDIO_PLAY));
        snprintf(bs, sizeof(bs), LV_SYMBOL_BELL  "%s", tr(STR_AUDIO_SOS));
        small_button(row, bp, CY_CYAN,    au_morse_e);
        small_button(row, bs, CY_MAGENTA, au_sos_e);
        small_button(row, tr(STR_CLOSE), CY_DIM, au_close_e);
    }
}
