#include "ui_common.h"
#include "ui_chanmgr.h"
#include "ui_dialog.h"
#include "mesh.h"
#include "sys.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ============================================================ */
/* Gestionnaire de canaux (modal plein écran) -- detache de ui.c */
/* ============================================================ */
static lv_obj_t *cm_ov, *cm_list;
static lv_obj_t *cm_name_ov, *cm_name_ta;          /* sous-modal saisie nom */
static lv_obj_t *cm_imp_ov, *cm_imp_ta;            /* sous-modal import URL */
static lv_obj_t *cm_fr_ov;                          /* sous-modal canaux FR connus */
static lv_obj_t *cm_share_ov;                       /* sous-modal QR partage */
#if CFG_WIFI_QR
static lv_obj_t *cm_qr_panel, *cm_qr_canvas, *cm_qr_status;
#define CM_QR_W 320
#define CM_QR_H 240
static uint8_t   cm_qr_buf[CM_QR_W * CM_QR_H * 2] __attribute__((aligned(4)));
#endif
static int  cm_name_target = -1;            /* -1 = creation, >=0 = renommage */
static bool cm_name_enc;
static int  cm_del_target = -1;

static void cm_rebuild_list(void);

/* ---- sous-modal saisie de nom (creation / renommage) ---- */
static void cm_name_close(void) {
    if (cm_name_ov) { lv_obj_delete(cm_name_ov); cm_name_ov = NULL; }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}
static void cm_name_cancel_e(lv_event_t *e) { (void)e; cm_name_close(); }
static void cm_name_ok_e(lv_event_t *e) {
    (void)e;
    const char *nm = lv_textarea_get_text(cm_name_ta);
    if (nm && nm[0]) {
        if (cm_name_target < 0) mesh_channel_create(nm, cm_name_enc);
        else                    mesh_channel_rename(cm_name_target, nm);
    }
    cm_name_close();
}
static void cm_name_open(int target, bool enc, const char *cur) {
    cm_name_target = target;
    cm_name_enc    = enc;
    cm_name_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cm_name_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cm_name_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(cm_name_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cm_name_ov, 0, 0);
    lv_obj_set_style_radius(cm_name_ov, 0, 0);
    lv_obj_set_style_pad_all(cm_name_ov, 8, 0);
    lv_obj_clear_flag(cm_name_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cm_name_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cm_name_ov, 6, 0);

    const char *title = target < 0
        ? (enc ? "Nouveau canal chiffré" : "Nouveau canal public")
        : "Renommer le canal";
    label(cm_name_ov, title, FONT_BIG, enc ? CY_MAGENTA : CY_CYAN);

    cm_name_ta = settings_field(cm_name_ov, "nom du canal", cur ? cur : "", false);

    lv_obj_t *row = lv_obj_create(cm_name_ov);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, tr(STR_CANCEL),   CY_DIM,  cm_name_cancel_e);
    small_button(row, tr(STR_VALIDATE), CY_CYAN, cm_name_ok_e);
}

static void cm_create_pub_e(lv_event_t *e) { (void)e; cm_name_open(-1, false, ""); }
static void cm_create_enc_e(lv_event_t *e) { (void)e; cm_name_open(-1, true,  ""); }
static void cm_rename_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const mesh_channel_t *c = mesh_channel(i);
    cm_name_open(i, c ? c->enc : false, c ? c->name : "");
}

/* ---- suppression ---- */
static void cm_delete_yes(void) {
    if (cm_del_target >= 0) mesh_channel_delete(cm_del_target);
    cm_del_target = -1;
}
static void cm_delete_e(lv_event_t *e) {
    cm_del_target = (int)(intptr_t)lv_event_get_user_data(e);
    confirm_dialog(tr(STR_CHAN_DELETE_CONFIRM), cm_delete_yes);
}

/* ---- partage (QR + URL) ---- */
static void cm_share_close_e(lv_event_t *e) {
    (void)e;
    if (cm_share_ov) { lv_obj_delete(cm_share_ov); cm_share_ov = NULL; }
}
static void cm_share_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const char *url = mesh_channel_share_url(i);
    if (!url) { ui_dialog_warning(tr(STR_SHARE_UNAVAILABLE)); return; }
    const mesh_channel_t *c = mesh_channel(i);

    cm_share_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cm_share_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cm_share_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(cm_share_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cm_share_ov, 0, 0);
    lv_obj_set_style_radius(cm_share_ov, 0, 0);
    lv_obj_set_style_pad_all(cm_share_ov, 8, 0);
    lv_obj_clear_flag(cm_share_ov, LV_OBJ_FLAG_SCROLLABLE);

    char hdr[64]; snprintf(hdr, sizeof(hdr), tr(STR_FMT_SHARE_HDR), c ? c->name : "");
    lv_obj_t *htop = label(cm_share_ov, hdr, FONT_SMALL, CY_CYAN);
    lv_obj_align(htop, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *qr = lv_qrcode_create(cm_share_ov);
    lv_qrcode_set_size(qr, 240);
    lv_qrcode_set_dark_color(qr, lv_color_hex(CY_CYAN));
    lv_qrcode_set_light_color(qr, lv_color_hex(CY_BG));
    lv_qrcode_update(qr, url, (int32_t)strlen(url));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -24);

    lv_obj_t *u = label(cm_share_ov, url, FONT_SMALL, CY_DIM);
    lv_label_set_long_mode(u, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(u, LV_PCT(100));
    lv_obj_align(u, LV_ALIGN_CENTER, 0, 120);

    lv_obj_t *bar = lv_obj_create(cm_share_ov);
    lv_obj_set_size(bar, LV_PCT(100), 36);
    flat(bar); lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    {
        char fb[24]; snprintf(fb, sizeof(fb), LV_SYMBOL_CLOSE "  %s", tr(STR_CLOSE));
        small_button(bar, fb, CY_DIM, cm_share_close_e);
    }
}

/* ---- import (coller URL) ---- */
static void cm_imp_close(void) {
    if (cm_imp_ov) { lv_obj_delete(cm_imp_ov); cm_imp_ov = NULL; }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}
static void cm_imp_cancel_e(lv_event_t *e) { (void)e; cm_imp_close(); }
static void cm_imp_ok_e(lv_event_t *e) {
    (void)e;
    const char *url = lv_textarea_get_text(cm_imp_ta);
    int n = (url && url[0]) ? mesh_channel_import_url(url) : -1;
    cm_imp_close();
    if (n < 0)       ui_dialog_error(tr(STR_URL_INVALID));
    else if (n == 0) ui_dialog_warning(tr(STR_NO_CHANNEL_ADDED));
    else             ui_dialog_info(tr(STR_CHAN_ADDED));
}
static void cm_import_e(lv_event_t *e) {
    (void)e;
    cm_imp_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cm_imp_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cm_imp_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(cm_imp_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cm_imp_ov, 0, 0);
    lv_obj_set_style_radius(cm_imp_ov, 0, 0);
    lv_obj_set_style_pad_all(cm_imp_ov, 8, 0);
    lv_obj_clear_flag(cm_imp_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cm_imp_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cm_imp_ov, 6, 0);

    label(cm_imp_ov, tr(STR_IMPORT_CHAN_TITLE), FONT_BIG, CY_CYAN);
    cm_imp_ta = settings_field(cm_imp_ov, tr(STR_PASTE_URL_HINT), "", false);

    lv_obj_t *row = lv_obj_create(cm_imp_ov);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, tr(STR_CANCEL),       CY_DIM,  cm_imp_cancel_e);
    small_button(row, tr(STR_CHAN_IMPORT) + 1, CY_CYAN, cm_imp_ok_e);
}

/* ---- canaux publics FR connus (import en un tap) ----
 * URL = ChannelSet protobuf base64url ; l'import ne lit que les ChannelSettings
 * (nom + PSK), il NE touche PAS au preset/region/freq radio (ceux-ci se reglent
 * dans Reglages radio). PSK "AQ==" (0x01) = cle publique par defaut Meshtastic. */
typedef struct {
    const char *label;   /* nom affiche */
    const char *desc;    /* courte description */
    const char *url;     /* URL de partage meshtastic.org/e/# */
} fr_chan_t;

static const fr_chan_t FR_CHANS[] = {
    {
        "Gaulix (Fr_Balise / Fr_BlaBla / Fr_EMCOM)",
        "Reseau national FR. Ajoute les 3 canaux publics. "
        "Pense a regler la radio sur LONG_MODERATE + 869.4625 MHz.",
        "https://meshtastic.org/e/#ChYSAQEaCUZyX0JhbGlzZSgBMAE6AgggChUSAQEaCEZy"
        "X0VNQ09NKAEwAToCCCAKFhIBARoJRnJfQmxhQmxhKAEwAToCCCASFggBEAc4A0ADSAFQG2gBdZpdWUTIBgE",
    },
};
#define FR_CHANS_N ((int)(sizeof(FR_CHANS) / sizeof(FR_CHANS[0])))

static void cm_fr_close(void) {
    if (cm_fr_ov) { lv_obj_delete(cm_fr_ov); cm_fr_ov = NULL; }
}
static void cm_fr_close_e(lv_event_t *e) { (void)e; cm_fr_close(); }

static void cm_fr_import_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= FR_CHANS_N) return;
    const char *url = FR_CHANS[i].url;
    cm_fr_close();
    int n = mesh_channel_import_url(url);
    if (n < 0)       ui_dialog_error(tr(STR_URL_INVALID));
    else if (n == 0) ui_dialog_warning(tr(STR_NO_CHANNEL_ADDED));
    else             ui_dialog_info(tr(STR_CHAN_ADDED));
}

static void cm_fr_open_e(lv_event_t *e) {
    (void)e;
    cm_fr_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cm_fr_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cm_fr_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(cm_fr_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cm_fr_ov, 0, 0);
    lv_obj_set_style_radius(cm_fr_ov, 0, 0);
    lv_obj_set_style_pad_all(cm_fr_ov, 8, 0);
    lv_obj_clear_flag(cm_fr_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cm_fr_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cm_fr_ov, 6, 0);

    label(cm_fr_ov, "Canaux publics FR", FONT_BIG, CY_CYAN);

    lv_obj_t *list = lv_obj_create(cm_fr_ov);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flat(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < FR_CHANS_N; i++) {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        panel(card, CY_BORDER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);

        label(card, FR_CHANS[i].label, FONT_BODY, CY_CYAN);
        lv_obj_t *d = label(card, FR_CHANS[i].desc, FONT_SMALL, CY_DIM);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, LV_PCT(100));

        lv_obj_t *act = lv_obj_create(card);
        lv_obj_set_size(act, LV_PCT(100), 34);
        flat(act); lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);
        char bi[40];
        snprintf(bi, sizeof(bi), LV_SYMBOL_DOWNLOAD "%s", tr(STR_CHAN_IMPORT));
        lv_obj_t *b = small_button(act, bi, CY_GREEN, NULL);
        lv_obj_add_event_cb(b, cm_fr_import_e, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_t *bar = lv_obj_create(cm_fr_ov);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    flat(bar); lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    {
        char fb[24]; snprintf(fb, sizeof(fb), LV_SYMBOL_CLOSE "  %s", tr(STR_CLOSE));
        small_button(bar, fb, CY_DIM, cm_fr_close_e);
    }
}

#if CFG_WIFI_QR
/* ---- scanner QR de canal (camera + libzbar via sys_qr_*) ---- */
static void cm_qr_close(void) {
    sys_qr_stop();
    if (cm_qr_panel) { lv_obj_delete(cm_qr_panel); cm_qr_panel = NULL; }
    cm_qr_canvas = NULL; cm_qr_status = NULL;
}
static void cm_qr_close_e(lv_event_t *e) { (void)e; cm_qr_close(); }
static void cm_qr_frame_cb(void *user) {
    (void)user;
    if (cm_qr_canvas) lv_obj_invalidate(cm_qr_canvas);
}
static bool cm_qr_is_meshtastic_url(const char *s) {
    if (!s) return false;
    return strstr(s, "meshtastic.org/e/") != NULL;
}
static void cm_qr_hit_cb(const char *payload, void *user) {
    (void)user;
    if (cm_qr_is_meshtastic_url(payload)) {
        char url[256]; snprintf(url, sizeof(url), "%s", payload);
        cm_qr_close();
        int n = mesh_channel_import_url(url);
        if (n < 0)       ui_dialog_error(tr(STR_URL_INVALID));
        else if (n == 0) ui_dialog_warning(tr(STR_NO_CHANNEL_ADDED));
        else             ui_dialog_info(tr(STR_CHAN_ADDED));
    } else if (cm_qr_status) {
        lv_label_set_text(cm_qr_status, tr(STR_CHAN_QR_NOT_CHAN));
        lv_obj_set_style_text_color(cm_qr_status, lv_color_hex(CY_AMBER), 0);
    }
}
static void cm_qr_open_e(lv_event_t *e) {
    (void)e;
    if (cm_qr_panel) return;
    cm_qr_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cm_qr_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cm_qr_panel, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(cm_qr_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cm_qr_panel, 0, 0);
    lv_obj_set_style_radius(cm_qr_panel, 0, 0);
    lv_obj_set_style_pad_all(cm_qr_panel, 6, 0);
    lv_obj_clear_flag(cm_qr_panel, LV_OBJ_FLAG_SCROLLABLE);

    {
        char tt[40]; snprintf(tt, sizeof(tt), LV_SYMBOL_IMAGE "%s", tr(STR_CHAN_QR_TITLE));
        lv_obj_t *tt_lbl = label(cm_qr_panel, tt, FONT_BODY, CY_AMBER);
        lv_obj_align(tt_lbl, LV_ALIGN_TOP_MID, 0, 0);
    }

    lv_obj_t *frame = lv_obj_create(cm_qr_panel);
    lv_obj_set_size(frame, CM_QR_W + 6, CM_QR_H + 6);
    panel(frame, CY_BORDER);
    lv_obj_set_style_pad_all(frame, 2, 0);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    cm_qr_canvas = lv_canvas_create(frame);
    lv_canvas_set_buffer(cm_qr_canvas, cm_qr_buf, CM_QR_W, CM_QR_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_center(cm_qr_canvas);
    lv_canvas_fill_bg(cm_qr_canvas, lv_color_black(), LV_OPA_COVER);

    cm_qr_status = label(cm_qr_panel, tr(STR_CHAN_QR_HINT), FONT_SMALL, CY_DIM);
    lv_obj_align(cm_qr_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t *bb = lv_button_create(cm_qr_panel);
    lv_obj_set_size(bb, 120, 32);
    lv_obj_align(bb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_add_event_cb(bb, cm_qr_close_e, LV_EVENT_CLICKED, NULL);
    {
        char canc[24]; snprintf(canc, sizeof(canc), LV_SYMBOL_CLOSE "  %s", tr(STR_CANCEL));
        lv_obj_t *bl = label(bb, canc, FONT_SMALL, CY_DIM);
        lv_obj_center(bl);
    }

    sys_qr_start(cm_qr_buf, CM_QR_W, CM_QR_H, cm_qr_frame_cb, cm_qr_hit_cb, NULL);
}
#endif /* CFG_WIFI_QR */

/* ---- liste des canaux ---- */
static void cm_rebuild_list(void) {
    if (!cm_list) return;
    lv_obj_clean(cm_list);
    for (int i = 0; i < mesh_channel_count(); i++) {
        const mesh_channel_t *c = mesh_channel(i);
        if (!c) continue;
        lv_obj_t *card = lv_obj_create(cm_list);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        panel(card, c->enc ? CY_MAGENTA : CY_BORDER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);

        char nm[64];
        snprintf(nm, sizeof(nm), "%s%s",
                 c->enc ? LV_SYMBOL_EYE_CLOSE " " : "# ", c->name);
        lv_obj_t *top = lv_obj_create(card);
        lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
        flat(top); lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
        label(top, nm, FONT_BODY, c->enc ? CY_MAGENTA : CY_CYAN);
        const char *tag = c->role == 1 ? tr(STR_ROLE_PRIMARY)
                        : c->role == 2 ? tr(STR_ROLE_SECONDARY) : "";
        label(top, tag, FONT_SMALL, CY_DIM);

        lv_obj_t *act = lv_obj_create(card);
        lv_obj_set_size(act, LV_PCT(100), 34);
        flat(act); lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(act, 6, 0);
        lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *b;
        b = small_button(act, tr(STR_CHAN_RENAME), CY_CYAN, NULL);
        lv_obj_add_event_cb(b, cm_rename_e, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        b = small_button(act, tr(STR_CHAN_SHARE), CY_AMBER, NULL);
        lv_obj_add_event_cb(b, cm_share_e, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (c->role != 1) {
            b = small_button(act, tr(STR_CHAN_DELETE), CY_MAGENTA, NULL);
            lv_obj_add_event_cb(b, cm_delete_e, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
}

static void cm_close_e(lv_event_t *e) {
    (void)e;
#if CFG_WIFI_QR
    sys_qr_stop();
    if (cm_qr_panel) { lv_obj_delete(cm_qr_panel); cm_qr_panel = NULL; }
    cm_qr_canvas = NULL; cm_qr_status = NULL;
#endif
    if (cm_ov) { lv_obj_delete(cm_ov); cm_ov = NULL; cm_list = NULL; }
}

void ui_chanmgr_open_e(lv_event_t *e) {
    (void)e;
    cm_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cm_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cm_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(cm_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cm_ov, 0, 0);
    lv_obj_set_style_radius(cm_ov, 0, 0);
    lv_obj_set_style_pad_all(cm_ov, 8, 0);
    lv_obj_clear_flag(cm_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cm_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cm_ov, 6, 0);

    label(cm_ov, tr(STR_CHANNELS_TITLE), FONT_BIG, CY_CYAN);

    cm_list = lv_obj_create(cm_ov);
    lv_obj_set_width(cm_list, LV_PCT(100));
    lv_obj_set_flex_grow(cm_list, 1);
    flat(cm_list);
    lv_obj_set_flex_flow(cm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cm_list, 6, 0);
    lv_obj_set_scroll_dir(cm_list, LV_DIR_VER);
    cm_rebuild_list();

    lv_obj_t *r1 = lv_obj_create(cm_ov);
    lv_obj_set_size(r1, LV_PCT(100), 36);
    flat(r1); lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r1, 6, 0);
    lv_obj_clear_flag(r1, LV_OBJ_FLAG_SCROLLABLE);
    {
        char bp[32], be[32];
        snprintf(bp, sizeof(bp), LV_SYMBOL_PLUS "%s", tr(STR_CHAN_NEW_PUBLIC));
        snprintf(be, sizeof(be), LV_SYMBOL_PLUS "%s", tr(STR_CHAN_NEW_ENC));
        small_button(r1, bp, CY_CYAN,    cm_create_pub_e);
        small_button(r1, be, CY_MAGENTA, cm_create_enc_e);
    }

    lv_obj_t *r2 = lv_obj_create(cm_ov);
    lv_obj_set_size(r2, LV_PCT(100), 36);
    flat(r2); lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r2, 6, 0);
    lv_obj_clear_flag(r2, LV_OBJ_FLAG_SCROLLABLE);
    {
        char bi[32];
        snprintf(bi, sizeof(bi), LV_SYMBOL_DOWNLOAD "%s", tr(STR_CHAN_IMPORT));
        small_button(r2, bi, CY_AMBER, cm_import_e);
#if CFG_WIFI_QR
        char bq[32];
        snprintf(bq, sizeof(bq), LV_SYMBOL_IMAGE "%s",    tr(STR_CHAN_QR_BTN));
        small_button(r2, bq, CY_GREEN, cm_qr_open_e);
#endif
        small_button(r2, LV_SYMBOL_GPS "  FR", CY_CYAN, cm_fr_open_e);
    }

    lv_obj_t *r3 = lv_obj_create(cm_ov);
    lv_obj_set_size(r3, LV_PCT(100), 36);
    flat(r3); lv_obj_set_flex_flow(r3, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r3, 6, 0);
    lv_obj_clear_flag(r3, LV_OBJ_FLAG_SCROLLABLE);
    {
        char bc[32];
        snprintf(bc, sizeof(bc), LV_SYMBOL_CLOSE "%s", tr(STR_CHAN_CLOSE));
        small_button(r3, bc, CY_DIM, cm_close_e);
    }
}

void ui_chanmgr_refresh_if_open(void) {
    if (cm_ov && cm_list) cm_rebuild_list();
}
