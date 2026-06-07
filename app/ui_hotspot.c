#include "ui_common.h"
#include "ui_hotspot.h"
#include "sys.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

/* ============================================================ */
/* App HOTSPOT : etat + toggle + QR WiFi credentials             */
/* ============================================================ */

static lv_obj_t *hap_lbl_state, *hap_lbl_btn;

/* ---- QR code WiFi du hotspot (modal plein ecran) ---- */
static lv_obj_t *qr_ov;
static void qr_close_cb(lv_event_t *e) { (void)e; if (qr_ov) { lv_obj_delete(qr_ov); qr_ov = NULL; } }
static void qr_open_cb(lv_event_t *e) {
    (void)e;
    qr_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(qr_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(qr_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(qr_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(qr_ov, 0, 0);
    lv_obj_set_style_radius(qr_ov, 0, 0);
    lv_obj_set_style_pad_all(qr_ov, 8, 0);
    lv_obj_clear_flag(qr_ov, LV_OBJ_FLAG_SCROLLABLE);

    char hdr[96]; snprintf(hdr, sizeof(hdr), "Scanner pour rejoindre %s", settings_hotspot_ssid());
    lv_obj_t *t = label(qr_ov, hdr, FONT_SMALL, CY_CYAN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    char payload[160];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
             settings_hotspot_ssid(), settings_hotspot_pass());
    lv_obj_t *qr = lv_qrcode_create(qr_ov);
    lv_qrcode_set_size(qr, 260);
    lv_qrcode_set_dark_color(qr, lv_color_hex(CY_CYAN));
    lv_qrcode_set_light_color(qr, lv_color_hex(CY_BG));
    lv_qrcode_update(qr, payload, (int32_t)strlen(payload));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -10);

    char cred[160]; snprintf(cred, sizeof(cred), "SSID  %s\npass  %s",
                             settings_hotspot_ssid(), settings_hotspot_pass());
    lv_obj_t *c = label(qr_ov, cred, FONT_SMALL, CY_TEXT);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 140);

    lv_obj_t *bar = lv_obj_create(qr_ov);
    lv_obj_set_size(bar, LV_PCT(100), 36);
    flat(bar); lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    small_button(bar, LV_SYMBOL_CLOSE "  FERMER", CY_DIM, qr_close_cb);
}

/* ---- toggle hotspot via confirm dialog ---- */
static void hot_yes(void) { sys_hotspot_set(!sys_hotspot_active()); }
static void hot_toggle_cb(lv_event_t *e) {
    (void)e;
    bool on = sys_hotspot_active();
    confirm_dialog(on ? tr(STR_CONFIRM_HOTSPOT_OFF) : tr(STR_CONFIRM_HOTSPOT_ON), hot_yes);
}

/* ---- refresh timer ---- */
static void hap_refresh(lv_timer_t *t) {
    (void)t;
    if (!hap_lbl_state) return;
    bool on = sys_hotspot_active();
    lv_label_set_text(hap_lbl_state, on ? tr(STR_STATE_ACTIVE) : tr(STR_STATE_INACTIVE));
    lv_obj_set_style_text_color(hap_lbl_state, lv_color_hex(on ? CY_GREEN : CY_DIM), 0);
    lv_label_set_text(hap_lbl_btn, on ? tr(STR_BTN_DISABLE) : tr(STR_BTN_ENABLE));
}

void ui_hotspot_reset(void) {
    hap_lbl_state = NULL;
    hap_lbl_btn = NULL;
}

void ui_hotspot_build(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    label(col, tr(STR_HAP_TITLE), FONT_BIG, CY_MAGENTA);

    lv_obj_t *r = lv_obj_create(col);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    flat(r); lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    hap_lbl_state = label(r, "?", FONT_BODY, CY_DIM);
    lv_obj_t *btn = lv_button_create(r);
    lv_obj_set_size(btn, 150, 32);
    lv_obj_set_style_radius(btn, 2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, hot_toggle_cb, LV_EVENT_CLICKED, NULL);
    hap_lbl_btn = label(btn, "?", FONT_SMALL, CY_TEXT);
    lv_obj_center(hap_lbl_btn);

    char credbuf[160]; snprintf(credbuf, sizeof(credbuf), "SSID  %s\npass  %s",
                                settings_hotspot_ssid(), settings_hotspot_pass());
    label(col, credbuf, FONT_BODY, CY_TEXT);

    char hqr[40]; snprintf(hqr, sizeof(hqr), LV_SYMBOL_IMAGE "%s", tr(STR_HAP_QR_BTN));
    lv_obj_t *qrb = small_button(col, hqr, CY_MAGENTA, qr_open_cb);
    lv_obj_set_flex_grow(qrb, 0);
    lv_obj_set_height(qrb, 38);
    lv_obj_set_width(qrb, LV_PCT(100));

    hap_refresh(NULL);
    sys_refresh_timer = lv_timer_create(hap_refresh, 3000, NULL);
}
