#include "ui_common.h"
#include "ui_settings.h"
#include "ui_dialog.h"
#include "settings.h"
#include "sys.h"
#include "mesh.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ============================================================ */
/* Modal Reglages : nom noeud, SSID/pass hotspot, fuseau         */
/* settings_field + set_ta_focus_e : exposes via ui_common.h     */
/* ============================================================ */

static lv_obj_t *set_ov, *set_ta_node, *set_ta_ssid, *set_ta_pass, *set_ta_tz;

/* Handler de focus d'un textarea : ouvre le clavier virtuel. Expose pour
 * permettre a un module externe (cf. ui_chanmgr) d'attacher ce focus a
 * son propre textarea. */
void set_ta_focus_e(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

static void set_modal_close(void) {
    if (set_ov) { lv_obj_delete(set_ov); set_ov = NULL; }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* Recopie src dans dst en retirant les espaces de debut/fin. */
static void str_trim(const char *src, char *dst, size_t cap) {
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Valide le nom long : 1 a 39 caracteres imprimables. */
static bool node_name_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 39) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

/* Derive un nom court (<=4 car.) a partir du nom long : initiales des
 * mots si plusieurs mots, sinon les 4 premiers caracteres, en MAJ. */
static void derive_short_name(const char *longn, char *out, size_t cap) {
    char ini[8]; size_t k = 0;
    bool prev_sp = true;
    for (const char *p = longn; *p && k < 4; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) { prev_sp = true; continue; }
        if (prev_sp && isalnum(c)) ini[k++] = (char)toupper(c);
        prev_sp = false;
    }
    if (k >= 2) {
        ini[k] = '\0';
    } else {
        k = 0;
        for (const char *p = longn; *p && k < 4; p++) {
            unsigned char c = (unsigned char)*p;
            if (!isspace(c)) ini[k++] = (char)toupper(c);
        }
        ini[k] = '\0';
    }
    if (k == 0) { ini[0] = '?'; ini[1] = '\0'; }
    size_t n = strlen(ini);
    if (n >= cap) n = cap - 1;
    memcpy(out, ini, n);
    out[n] = '\0';
}

static void set_save_cb_e(lv_event_t *e) {
    (void)e;
    char name[48];
    str_trim(lv_textarea_get_text(set_ta_node), name, sizeof(name));
    if (!node_name_valid(name)) {
        ui_dialog_error(tr(STR_INVALID_NODE_NAME));
        return;
    }

    bool name_changed = strcmp(name, settings_node_name()) != 0;

    settings_set_node_name   (name);
    settings_set_hotspot_ssid(lv_textarea_get_text(set_ta_ssid));
    settings_set_hotspot_pass(lv_textarea_get_text(set_ta_pass));
    const char *tz = lv_textarea_get_text(set_ta_tz);
    settings_set_timezone(tz);
    settings_save();
    sys_set_timezone(tz);

    /* Pousse le nouveau nom sur le mesh (AdminMessage set_owner). */
    bool offline = false;
    if (name_changed) {
        char shortn[8];
        derive_short_name(name, shortn, sizeof(shortn));
        if (!mesh_set_owner(name, shortn))
            offline = true;
    }

    set_modal_close();
    if (offline)
        ui_dialog_warning(tr(STR_NAME_SAVED_LOCAL));
}

static void set_cancel_cb_e(lv_event_t *e) { (void)e; set_modal_close(); }

lv_obj_t *settings_field(lv_obj_t *parent, const char *key, const char *val, bool pwd) {
    label(parent, key, FONT_BODY, CY_DIM);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    if (pwd) lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_text(ta, val);
    /* Hauteur 40 (au lieu de 30) pour confort tactile + texte non clippe. */
    lv_obj_set_size(ta, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(ta, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(ta, FONT_BODY, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 3, 0);
    lv_obj_set_style_pad_all(ta, 8, 0);
    /* curseur cyan visible */
    lv_obj_set_style_border_color(ta, lv_color_hex(CY_CYAN), LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
    lv_obj_add_event_cb(ta, set_ta_focus_e, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, set_ta_focus_e, LV_EVENT_FOCUSED, NULL);
    return ta;
}

void ui_settings_open_e(lv_event_t *e) {
    (void)e;
    set_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(set_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(set_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(set_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(set_ov, 0, 0);
    lv_obj_set_style_radius(set_ov, 0, 0);
    lv_obj_set_style_pad_all(set_ov, 8, 0);
    lv_obj_clear_flag(set_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(set_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(set_ov, 4, 0);

    label(set_ov, tr(STR_SET_TITLE), FONT_BIG, CY_CYAN);

    set_ta_node = settings_field(set_ov, tr(STR_FIELD_NODE_NAME),    settings_node_name(),    false);
    set_ta_ssid = settings_field(set_ov, tr(STR_FIELD_HOTSPOT_SSID), settings_hotspot_ssid(), false);
    set_ta_pass = settings_field(set_ov, tr(STR_FIELD_HOTSPOT_PASS), settings_hotspot_pass(), true);
    set_ta_tz   = settings_field(set_ov, tr(STR_FIELD_TIMEZONE),     settings_timezone(),     false);

    lv_obj_t *row = lv_obj_create(set_ov);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, tr(STR_CANCEL), CY_DIM,  set_cancel_cb_e);
    small_button(row, tr(STR_SAVE),   CY_CYAN, set_save_cb_e);
}
