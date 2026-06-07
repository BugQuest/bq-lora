#include "ui_common.h"
#include "ui_bt.h"
#include "sys.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ============================================================ */
/* Modal Bluetooth (scan BLE + appairage + console serie SPP)   */
/* ============================================================ */
static lv_obj_t *bt_ov, *bt_list_ov, *bt_status, *bt_act_panel, *bt_serial_btn_lbl;
static bt_device_t bt_kept[48];
static int  bt_kept_n = 0;

static void bt_modal_close_e(lv_event_t *e) {
    (void)e;
    if (bt_ov) {
        lv_obj_delete(bt_ov);
        bt_ov = NULL;
        bt_list_ov = bt_status = bt_act_panel = bt_serial_btn_lbl = NULL;
    }
}

static void bt_scan_done(const bt_device_t *list, int n, void *user);

static void bt_rescan_e(lv_event_t *e) {
    (void)e;
    if (bt_status) lv_label_set_text(bt_status, tr(STR_BT_SCANNING));
    sys_bt_scan_async(bt_scan_done, NULL);
}

static void bt_action_done(bool ok, const char *msg, void *user) {
    (void)user;
    if (bt_status) {
        lv_label_set_text(bt_status, ok ? tr(STR_BT_OK_RESCAN)
                                        : (msg && msg[0] ? msg : tr(STR_BT_FAILED)));
        lv_obj_set_style_text_color(bt_status, lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    }
    if (ok) sys_bt_scan_async(bt_scan_done, NULL);
}

static void bt_act_close(void) {
    if (bt_act_panel) { lv_obj_delete(bt_act_panel); bt_act_panel = NULL; }
}
static void bt_act_close_e(lv_event_t *e) { (void)e; bt_act_close(); }

/* boutons du panneau d'action : user_data = index dans bt_kept */
static void bt_do_pair_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_act_close();
    if (bt_status) { lv_label_set_text(bt_status, tr(STR_BT_PAIRING));
                     lv_obj_set_style_text_color(bt_status, lv_color_hex(CY_CYAN), 0); }
    sys_bt_action_async("pair", bt_kept[i].addr, bt_action_done, NULL);
}
static void bt_do_connect_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_act_close();
    if (bt_status) { lv_label_set_text(bt_status, tr(STR_BT_CONNECTING));
                     lv_obj_set_style_text_color(bt_status, lv_color_hex(CY_CYAN), 0); }
    sys_bt_action_async("connect", bt_kept[i].addr, bt_action_done, NULL);
}
static void bt_do_disconnect_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_act_close();
    sys_bt_action_async("disconnect", bt_kept[i].addr, bt_action_done, NULL);
}
static void bt_do_remove_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_act_close();
    sys_bt_action_async("remove", bt_kept[i].addr, bt_action_done, NULL);
}

static void bt_row_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= bt_kept_n) return;
    bt_act_close();
    bt_device_t *d = &bt_kept[i];
    bt_act_panel = lv_obj_create(bt_ov);
    lv_obj_set_size(bt_act_panel, LV_PCT(94), 150);
    lv_obj_align(bt_act_panel, LV_ALIGN_CENTER, 0, 0);
    panel(bt_act_panel, CY_CYAN);
    lv_obj_clear_flag(bt_act_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bt_act_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bt_act_panel, 6, 0);

    char b[96]; snprintf(b, sizeof(b), "%s\n%s", d->name, d->addr);
    label(bt_act_panel, b, FONT_SMALL, CY_TEXT);

    lv_obj_t *row = lv_obj_create(bt_act_panel);
    lv_obj_set_size(row, LV_PCT(100), 36);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    void *ud = (void *)(intptr_t)i;
    if (d->connected) {
        lv_obj_add_event_cb(small_button(row, tr(STR_DISCONNECT), CY_AMBER,  NULL), bt_do_disconnect_e, LV_EVENT_CLICKED, ud);
        lv_obj_add_event_cb(small_button(row, tr(STR_FORGET),     CY_MAGENTA, NULL), bt_do_remove_e,     LV_EVENT_CLICKED, ud);
    } else if (d->paired) {
        lv_obj_add_event_cb(small_button(row, tr(STR_CONNECT),    CY_GREEN,   NULL), bt_do_connect_e,    LV_EVENT_CLICKED, ud);
        lv_obj_add_event_cb(small_button(row, tr(STR_FORGET),     CY_MAGENTA, NULL), bt_do_remove_e,     LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_add_event_cb(small_button(row, tr(STR_PAIR),       CY_CYAN,    NULL), bt_do_pair_e,       LV_EVENT_CLICKED, ud);
    }

    lv_obj_t *row2 = lv_obj_create(bt_act_panel);
    lv_obj_set_size(row2, LV_PCT(100), 34);
    flat(row2); lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);
    {
        char fb[24]; snprintf(fb, sizeof(fb), LV_SYMBOL_CLOSE "  %s", tr(STR_CLOSE));
        small_button(row2, fb, CY_DIM, bt_act_close_e);
    }
}

static void bt_scan_done(const bt_device_t *list, int n, void *user) {
    (void)user;
    if (!bt_list_ov) return;
    lv_obj_clean(bt_list_ov);
    bt_kept_n = (n > 48) ? 48 : n;
    for (int i = 0; i < bt_kept_n; i++) bt_kept[i] = list[i];
    if (bt_kept_n == 0) { if (bt_status) lv_label_set_text(bt_status, tr(STR_BT_NO_DEVICE)); return; }
    if (bt_status) lv_label_set_text(bt_status, "");
    for (int i = 0; i < bt_kept_n; i++) {
        bt_device_t *d = &bt_kept[i];
        uint32_t col = d->connected ? CY_GREEN : (d->paired ? CY_CYAN : CY_BORDER);
        lv_obj_t *r = lv_button_create(bt_list_ov);
        lv_obj_set_size(r, LV_PCT(100), 38);
        lv_obj_set_style_radius(r, 2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(col), 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_add_event_cb(r, bt_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        const char *tag = d->connected ? LV_SYMBOL_OK " "
                        : (d->paired ? LV_SYMBOL_BLUETOOTH " " : "");
        lv_obj_t *l = label(r, "", FONT_SMALL, col == CY_BORDER ? CY_TEXT : col);
        if (d->rssi != 0) lv_label_set_text_fmt(l, "%s%s  %ddBm", tag, d->name, d->rssi);
        else              lv_label_set_text_fmt(l, "%s%s",        tag, d->name);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    }
}

static void bt_serial_toggle_e(lv_event_t *e) {
    (void)e;
    bool on = !sys_bt_serial_active();
    sys_bt_serial_set(on);
    if (bt_serial_btn_lbl) {
        char cb[48]; snprintf(cb, sizeof(cb), LV_SYMBOL_USB "%s",
                              on ? tr(STR_BT_CONSOLE_ON) : tr(STR_BT_CONSOLE_OFF));
        lv_label_set_text(bt_serial_btn_lbl, cb);
    }
    if (bt_status) {
        lv_label_set_text(bt_status, on ? tr(STR_BT_SERIAL_VISIBLE) : tr(STR_BT_SERIAL_HIDDEN));
        lv_obj_set_style_text_color(bt_status, lv_color_hex(on ? CY_GREEN : CY_DIM), 0);
    }
}

void ui_bt_modal_open(void) {
    bt_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bt_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bt_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(bt_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bt_ov, 0, 0);
    lv_obj_set_style_radius(bt_ov, 0, 0);
    lv_obj_set_style_pad_all(bt_ov, 6, 0);
    lv_obj_clear_flag(bt_ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(bt_ov);
    lv_obj_set_size(bar, LV_PCT(100), 34);
    flat(bar); lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    {
        char b1[32], b2[32];
        snprintf(b1, sizeof(b1), LV_SYMBOL_LEFT    "  %s", tr(STR_CLOSE));
        snprintf(b2, sizeof(b2), LV_SYMBOL_REFRESH "  %s", tr(STR_SCAN));
        small_button(bar, b1, CY_DIM,  bt_modal_close_e);
        small_button(bar, b2, CY_CYAN, bt_rescan_e);
    }
    bool ser = sys_bt_serial_active();
    lv_obj_t *sbtn = small_button(bar, "", ser ? CY_GREEN : CY_DIM, bt_serial_toggle_e);
    char sbb[48]; snprintf(sbb, sizeof(sbb), LV_SYMBOL_USB "%s",
                            ser ? tr(STR_BT_CONSOLE_ON) : tr(STR_BT_CONSOLE_OFF));
    bt_serial_btn_lbl = label(sbtn, sbb, FONT_SMALL, ser ? CY_GREEN : CY_TEXT);
    lv_obj_center(bt_serial_btn_lbl);

    bt_status = label(bt_ov, tr(STR_BT_SCANNING), FONT_SMALL, CY_CYAN);
    lv_obj_align(bt_status, LV_ALIGN_TOP_MID, 0, 44);

    bt_list_ov = lv_obj_create(bt_ov);
    lv_obj_set_size(bt_list_ov, LV_PCT(100), 388);
    lv_obj_align(bt_list_ov, LV_ALIGN_TOP_MID, 0, 70);
    flat(bt_list_ov);
    lv_obj_set_flex_flow(bt_list_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bt_list_ov, 4, 0);
    lv_obj_set_scroll_dir(bt_list_ov, LV_DIR_VER);

    bt_act_panel = NULL;
    sys_bt_scan_async(bt_scan_done, NULL);
}
