#include "ui_common.h"
#include "ui_wifi.h"
#include "ui_dialog.h"
#include "sys.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ============================================================ */
/* Modal WiFi (scan + connect + QR + WPS)                        */
/* Detache de ui.c -- statics file-local                         */
/* ============================================================ */

static lv_obj_t *wifi_ov, *wifi_list_ov, *wifi_status, *wifi_pwd_panel, *wifi_pwd_ta;
static char wifi_pending_ssid[64];

#if CFG_WIFI_QR
#define WIFI_QR_W 320
#define WIFI_QR_H 240
static uint8_t  wifi_qr_buf[WIFI_QR_W * WIFI_QR_H * 2] __attribute__((aligned(4)));
static lv_obj_t *wifi_qr_panel, *wifi_qr_canvas, *wifi_qr_status;
#endif

#if CFG_WPS
static lv_obj_t *wifi_wps_panel, *wifi_wps_status;
static lv_timer_t *wifi_wps_timer;
static int       wifi_wps_remaining;
#endif

static void wifi_modal_close_e(lv_event_t *e) {
    (void)e;
#if CFG_WIFI_QR
    sys_qr_stop();
    wifi_qr_canvas = NULL; wifi_qr_status = NULL; wifi_qr_panel = NULL;
#endif
#if CFG_WPS
    if (wifi_wps_timer) { lv_timer_delete(wifi_wps_timer); wifi_wps_timer = NULL; }
    wifi_wps_status = NULL; wifi_wps_panel = NULL;
#endif
    if (wifi_ov) { lv_obj_delete(wifi_ov); wifi_ov = NULL; }
}

static void wifi_scan_done(const wifi_net_t *list, int n, void *user);
static void wifi_rescan_e(lv_event_t *e) {
    (void)e;
    lv_label_set_text(wifi_status, tr(STR_WIFI_SCANNING));
    if (wifi_list_ov) lv_obj_clean(wifi_list_ov);
    sys_wifi_scan_async(wifi_scan_done, NULL);
}

static void wifi_connect_done(bool ok, const char *msg, void *user) {
    (void)user;
    ui_dialog_loading_hide();
    if (wifi_status) {
        lv_label_set_text(wifi_status, ok ? tr(STR_WIFI_CONNECTED) : msg);
        lv_obj_set_style_text_color(wifi_status, lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    }
    if (ok) ui_dialog_info(tr(STR_WIFI_CONNECTED));
    else    ui_dialog_error(msg ? msg : "Echec connexion WiFi");
}

static void wifi_pwd_ok_e(lv_event_t *e) {
    (void)e;
    const char *p = lv_textarea_get_text(wifi_pwd_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (wifi_pwd_panel) { lv_obj_delete(wifi_pwd_panel); wifi_pwd_panel = NULL; }
    lv_label_set_text(wifi_status, tr(STR_WIFI_CONNECTING));
    lv_obj_set_style_text_color(wifi_status, lv_color_hex(CY_CYAN), 0);
    ui_dialog_loading_show(tr(STR_WIFI_CONNECTING));
    sys_wifi_connect_async(wifi_pending_ssid, p, wifi_connect_done, NULL);
}
static void wifi_pwd_cancel_e(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (wifi_pwd_panel) { lv_obj_delete(wifi_pwd_panel); wifi_pwd_panel = NULL; }
}
static void wifi_pwd_ta_e(lv_event_t *e) {
    (void)e;
    lv_keyboard_set_textarea(kb, wifi_pwd_ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

static void wifi_open_pwd(const char *ssid) {
    strncpy(wifi_pending_ssid, ssid, sizeof(wifi_pending_ssid) - 1);
    wifi_pwd_panel = lv_obj_create(wifi_ov);
    lv_obj_set_size(wifi_pwd_panel, LV_PCT(94), 130);
    lv_obj_align(wifi_pwd_panel, LV_ALIGN_CENTER, 0, -10);
    panel(wifi_pwd_panel, CY_MAGENTA);
    lv_obj_clear_flag(wifi_pwd_panel, LV_OBJ_FLAG_SCROLLABLE);
    char b[96]; snprintf(b, sizeof(b), "SSID : %s", ssid);
    lv_obj_t *t = label(wifi_pwd_panel, b, FONT_SMALL, CY_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    wifi_pwd_ta = lv_textarea_create(wifi_pwd_panel);
    lv_textarea_set_one_line(wifi_pwd_ta, true);
    lv_textarea_set_password_mode(wifi_pwd_ta, true);
    lv_textarea_set_placeholder_text(wifi_pwd_ta, tr(STR_WIFI_PASSPHRASE));
    lv_obj_set_size(wifi_pwd_ta, LV_PCT(100), 32);
    lv_obj_align(wifi_pwd_ta, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_pwd_ta, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_text_font(wifi_pwd_ta, FONT_BODY, 0);
    lv_obj_set_style_text_color(wifi_pwd_ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_add_event_cb(wifi_pwd_ta, wifi_pwd_ta_e, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rr = lv_obj_create(wifi_pwd_panel);
    lv_obj_set_size(rr, LV_PCT(100), 34);
    flat(rr); lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(rr, 6, 0);
    lv_obj_align(rr, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(rr, LV_OBJ_FLAG_SCROLLABLE);
    small_button(rr, tr(STR_CANCEL),  CY_DIM,     wifi_pwd_cancel_e);
    small_button(rr, tr(STR_CONNECT), CY_MAGENTA, wifi_pwd_ok_e);
}

static void wifi_row_cb(lv_event_t *e) {
    wifi_net_t *n = lv_event_get_user_data(e);
    if (n->secured) {
        wifi_open_pwd(n->ssid);
    } else {
        lv_label_set_text(wifi_status, tr(STR_WIFI_CONNECTING));
        ui_dialog_loading_show(tr(STR_WIFI_CONNECTING));
        sys_wifi_connect_async(n->ssid, "", wifi_connect_done, NULL);
    }
}

static wifi_net_t wifi_kept[32];
static int wifi_kept_n = 0;

static void wifi_scan_done(const wifi_net_t *list, int n, void *user) {
    (void)user;
    if (!wifi_list_ov) return;
    lv_obj_clean(wifi_list_ov);
    if (n == 0) { lv_label_set_text(wifi_status, tr(STR_WIFI_NO_NETWORK)); return; }
    wifi_kept_n = 0;
    for (int i = 0; i < n && wifi_kept_n < 32; i++) {
        int found = -1;
        for (int j = 0; j < wifi_kept_n; j++)
            if (strcmp(wifi_kept[j].ssid, list[i].ssid) == 0) { found = j; break; }
        if (found < 0) wifi_kept[wifi_kept_n++] = list[i];
        else if (list[i].signal > wifi_kept[found].signal) wifi_kept[found] = list[i];
    }
    for (int i = 0; i < wifi_kept_n; i++) {
        lv_obj_t *r = lv_button_create(wifi_list_ov);
        lv_obj_set_size(r, LV_PCT(100), 34);
        lv_obj_set_style_radius(r, 2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(wifi_kept[i].active ? CY_CYAN : CY_BORDER), 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_add_event_cb(r, wifi_row_cb, LV_EVENT_CLICKED, &wifi_kept[i]);
        char b[96];
        snprintf(b, sizeof(b), "%s%s  %s  %d%%",
                 wifi_kept[i].active ? LV_SYMBOL_OK " " : "",
                 wifi_kept[i].secured ? LV_SYMBOL_EYE_CLOSE : "#",
                 wifi_kept[i].ssid, wifi_kept[i].signal);
        lv_obj_t *l = label(r, b, FONT_SMALL,
            wifi_kept[i].active ? CY_GREEN : (wifi_kept[i].secured ? CY_MAGENTA : CY_TEXT));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    }
    lv_label_set_text(wifi_status, "");
}

#if CFG_WIFI_QR
/* ----- Parser standard WiFi-QR (NFC/Wi-Fi Alliance) -----
 * Format : WIFI:T:<type>;S:<ssid>;P:<pwd>;[H:<bool>;];;
 * Caracteres ';' ':' ',' '\\' '"' echappes par '\'. */
static bool wifi_qr_parse(const char *src, char *ssid, size_t ss, char *pass, size_t ps,
                          char *auth, size_t as)
{
    ssid[0] = pass[0] = auth[0] = 0;
    if (!src || strncmp(src, "WIFI:", 5) != 0) return false;
    const char *p = src + 5;
    while (*p && *p != ';') {
        if (p[1] != ':') break;
        char key = *p; p += 2;
        char val[128]; size_t k = 0;
        while (*p && *p != ';') {
            if (*p == '\\' && p[1]) { if (k < sizeof(val) - 1) val[k++] = p[1]; p += 2; }
            else                      { if (k < sizeof(val) - 1) val[k++] = *p; p++; }
        }
        val[k] = 0;
        if (*p == ';') p++;
        switch (key) {
            case 'S': strncpy(ssid, val, ss - 1); ssid[ss - 1] = 0; break;
            case 'P': strncpy(pass, val, ps - 1); pass[ps - 1] = 0; break;
            case 'T': strncpy(auth, val, as - 1); auth[as - 1] = 0; break;
            default: break;
        }
    }
    return ssid[0] != 0;
}

/* ----- Panneau QR : flux camera + decodage zbar ----- */
static void wifi_qr_close(void) {
    sys_qr_stop();
    if (wifi_qr_panel) { lv_obj_delete(wifi_qr_panel); wifi_qr_panel = NULL; }
    wifi_qr_canvas = NULL; wifi_qr_status = NULL;
}
static void wifi_qr_close_e(lv_event_t *e) { (void)e; wifi_qr_close(); }
static void wifi_qr_frame_cb(void *user) {
    (void)user;
    if (wifi_qr_canvas) lv_obj_invalidate(wifi_qr_canvas);
}
static void wifi_qr_hit_cb(const char *payload, void *user) {
    (void)user;
    char ssid[64], pass[128], auth[16];
    if (wifi_qr_parse(payload, ssid, sizeof(ssid), pass, sizeof(pass), auth, sizeof(auth))) {
        wifi_qr_close();
        if (wifi_status) {
            char b[160];
            snprintf(b, sizeof(b), tr(STR_FMT_QR_CONNECTING), ssid);
            lv_label_set_text(wifi_status, b);
            lv_obj_set_style_text_color(wifi_status, lv_color_hex(CY_CYAN), 0);
        }
        ui_dialog_loading_show(tr(STR_WIFI_CONNECTING));
        sys_wifi_connect_async(ssid, pass, wifi_connect_done, NULL);
    } else if (wifi_qr_status) {
        lv_label_set_text(wifi_qr_status, tr(STR_WIFI_QR_NOT_WIFI));
        lv_obj_set_style_text_color(wifi_qr_status, lv_color_hex(CY_AMBER), 0);
    }
}
static void wifi_qr_open_e(lv_event_t *e) {
    (void)e;
    if (wifi_qr_panel) return;
    wifi_qr_panel = lv_obj_create(wifi_ov);
    lv_obj_set_size(wifi_qr_panel, LV_PCT(96), 360);
    lv_obj_align(wifi_qr_panel, LV_ALIGN_CENTER, 0, 0);
    panel(wifi_qr_panel, CY_AMBER);
    lv_obj_clear_flag(wifi_qr_panel, LV_OBJ_FLAG_SCROLLABLE);
    {
        char tt[40]; snprintf(tt, sizeof(tt), LV_SYMBOL_IMAGE "%s", tr(STR_WIFI_QR_TITLE));
        lv_obj_t *tt_lbl = label(wifi_qr_panel, tt, FONT_BODY, CY_AMBER);
        lv_obj_align(tt_lbl, LV_ALIGN_TOP_MID, 0, 0);
    }
    lv_obj_t *frame = lv_obj_create(wifi_qr_panel);
    lv_obj_set_size(frame, WIFI_QR_W + 6, WIFI_QR_H + 6);
    panel(frame, CY_BORDER);
    lv_obj_set_style_pad_all(frame, 2, 0);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    wifi_qr_canvas = lv_canvas_create(frame);
    lv_canvas_set_buffer(wifi_qr_canvas, wifi_qr_buf, WIFI_QR_W, WIFI_QR_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_center(wifi_qr_canvas);
    lv_canvas_fill_bg(wifi_qr_canvas, lv_color_black(), LV_OPA_COVER);

    wifi_qr_status = label(wifi_qr_panel, tr(STR_WIFI_QR_HINT), FONT_SMALL, CY_DIM);
    lv_obj_align(wifi_qr_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t *bb = lv_button_create(wifi_qr_panel);
    lv_obj_set_size(bb, 120, 28);
    lv_obj_align(bb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_add_event_cb(bb, wifi_qr_close_e, LV_EVENT_CLICKED, NULL);
    char canc[24]; snprintf(canc, sizeof(canc), LV_SYMBOL_CLOSE "  %s", tr(STR_CANCEL));
    lv_obj_t *bl = label(bb, canc, FONT_SMALL, CY_DIM);
    lv_obj_center(bl);

    sys_qr_start(wifi_qr_buf, WIFI_QR_W, WIFI_QR_H,
                 wifi_qr_frame_cb, wifi_qr_hit_cb, NULL);
}
#endif /* CFG_WIFI_QR */

#if CFG_WPS
/* ----- Panneau WPS push-button ----- */
static void wifi_wps_close(void) {
    if (wifi_wps_timer) { lv_timer_delete(wifi_wps_timer); wifi_wps_timer = NULL; }
    if (wifi_wps_panel) { lv_obj_delete(wifi_wps_panel); wifi_wps_panel = NULL; }
    wifi_wps_status = NULL;
}
static void wifi_wps_close_e(lv_event_t *e) { (void)e; wifi_wps_close(); }
static void wifi_wps_tick(lv_timer_t *t) {
    (void)t;
    if (--wifi_wps_remaining <= 0) {
        if (wifi_wps_timer) { lv_timer_delete(wifi_wps_timer); wifi_wps_timer = NULL; }
        return;
    }
    if (wifi_wps_status) {
        char b[80]; snprintf(b, sizeof(b), tr(STR_WPS_FMT_REMAINING), wifi_wps_remaining);
        lv_label_set_text(wifi_wps_status, b);
    }
}
static void wifi_wps_done(bool ok, const char *msg, void *user) {
    (void)user;
    if (wifi_wps_timer) { lv_timer_delete(wifi_wps_timer); wifi_wps_timer = NULL; }
    if (wifi_wps_status) {
        lv_label_set_text(wifi_wps_status, msg);
        lv_obj_set_style_text_color(wifi_wps_status,
            lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    }
    if (wifi_status) {
        lv_label_set_text(wifi_status, ok ? tr(STR_WPS_CONNECTED) : tr(STR_WPS_FAILED));
        lv_obj_set_style_text_color(wifi_status,
            lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    }
}
static void wifi_wps_open_e(lv_event_t *e) {
    (void)e;
    if (wifi_wps_panel) return;
    wifi_wps_panel = lv_obj_create(wifi_ov);
    lv_obj_set_size(wifi_wps_panel, LV_PCT(94), 140);
    lv_obj_align(wifi_wps_panel, LV_ALIGN_CENTER, 0, -10);
    panel(wifi_wps_panel, CY_AMBER);
    lv_obj_clear_flag(wifi_wps_panel, LV_OBJ_FLAG_SCROLLABLE);
    {
        char tt[40]; snprintf(tt, sizeof(tt), LV_SYMBOL_WIFI "%s", tr(STR_WPS_TITLE));
        lv_obj_t *tt_lbl = label(wifi_wps_panel, tt, FONT_BODY, CY_AMBER);
        lv_obj_align(tt_lbl, LV_ALIGN_TOP_MID, 0, 0);
    }
    wifi_wps_status = label(wifi_wps_panel, tr(STR_WPS_HINT), FONT_SMALL, CY_TEXT);
    lv_obj_align(wifi_wps_status, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(wifi_wps_status, LV_PCT(95));
    lv_label_set_long_mode(wifi_wps_status, LV_LABEL_LONG_WRAP);

    lv_obj_t *bb = lv_button_create(wifi_wps_panel);
    lv_obj_set_size(bb, 120, 28);
    lv_obj_align(bb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_add_event_cb(bb, wifi_wps_close_e, LV_EVENT_CLICKED, NULL);
    {
        char cb[24]; snprintf(cb, sizeof(cb), LV_SYMBOL_CLOSE "  %s", tr(STR_CLOSE));
        lv_obj_t *bl = label(bb, cb, FONT_SMALL, CY_DIM);
        lv_obj_center(bl);
    }
    wifi_wps_remaining = 120;
    wifi_wps_timer = lv_timer_create(wifi_wps_tick, 1000, NULL);
    sys_wifi_wps_async(wifi_wps_done, NULL);
}
#endif /* CFG_WPS */

void ui_wifi_modal_open(void) {
    wifi_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wifi_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(wifi_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_ov, 0, 0);
    lv_obj_set_style_radius(wifi_ov, 0, 0);
    lv_obj_set_style_pad_all(wifi_ov, 6, 0);
    lv_obj_clear_flag(wifi_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = lv_obj_create(wifi_ov);
    lv_obj_set_size(bar, LV_PCT(100), 34);
    flat(bar); lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    {
        char b1[32], b2[32];
        snprintf(b1, sizeof(b1), LV_SYMBOL_LEFT    "  %s", tr(STR_CLOSE));
        snprintf(b2, sizeof(b2), LV_SYMBOL_REFRESH "  %s", tr(STR_RESCAN));
        small_button(bar, b1, CY_DIM,   wifi_modal_close_e);
        small_button(bar, b2, CY_CYAN,  wifi_rescan_e);
#if CFG_WIFI_QR
        char b3[32];
        snprintf(b3, sizeof(b3), LV_SYMBOL_IMAGE "%s", tr(STR_WIFI_QR_BTN));
        small_button(bar, b3, CY_AMBER, wifi_qr_open_e);
#endif
#if CFG_WPS
        char b4[32];
        snprintf(b4, sizeof(b4), LV_SYMBOL_WIFI "%s", tr(STR_WIFI_WPS_BTN));
        small_button(bar, b4, CY_AMBER, wifi_wps_open_e);
#endif
    }
    wifi_status = label(wifi_ov, tr(STR_WIFI_SCANNING), FONT_SMALL, CY_CYAN);
    lv_obj_align(wifi_status, LV_ALIGN_TOP_MID, 0, 44);
    wifi_list_ov = lv_obj_create(wifi_ov);
    lv_obj_set_size(wifi_list_ov, LV_PCT(100), 390);
    lv_obj_align(wifi_list_ov, LV_ALIGN_TOP_MID, 0, 70);
    flat(wifi_list_ov);
    lv_obj_set_flex_flow(wifi_list_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list_ov, 4, 0);
    lv_obj_set_scroll_dir(wifi_list_ov, LV_DIR_VER);
    sys_wifi_scan_async(wifi_scan_done, NULL);
}
