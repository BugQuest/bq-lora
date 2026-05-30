#include "lvgl/lvgl.h"
#include "ui.h"
#include "theme.h"
#include "mesh.h"
#include "calib.h"
#include "sys.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- état */
static lv_obj_t *content;          /* zone centrale, reconstruite par onglet */
static lv_obj_t *kb;               /* clavier virtuel (overlay, masqué) */
static lv_obj_t *nav_btn[3];
static uint8_t   cur_chan = 0;     /* canal courant dans la vue chat */
static int       cur_tab = 0;      /* 0 chat, 1 nodes, 2 sys */

static void show_tab(int tab);

/* ---------------------------------------------------------------- helpers */
static void flat(lv_obj_t *o) {
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
}

static void panel(lv_obj_t *o, uint32_t border) {
    lv_obj_set_style_bg_color(o, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_set_style_pad_all(o, 6, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* ---------------------------------------------------------------- barre haute */
static lv_obj_t *tb_clock, *tb_usb, *tb_wifi;
static lv_timer_t *tb_timer;

static int read_int_file(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return 0;
    int v = 0; fscanf(f, "%d", &v); fclose(f); return v;
}

/* Le carrier du gadget g_ether reste a 1 meme cable debranche.
 * Indicateur fiable : un client a-t-il un bail DHCP actif sur usb0, ou un
 * voisin ARP sur l'interface ? */
static bool usb_client_connected(void) {
    FILE *f = fopen("/var/lib/NetworkManager/dnsmasq-usb0.leases", "r");
    if (f) {
        int c = fgetc(f); fclose(f);
        if (c != EOF) return true;
    }
    FILE *p = popen("ip neigh show dev usb0 2>/dev/null", "r");
    if (p) {
        int c = fgetc(p); pclose(p);
        if (c != EOF) return true;
    }
    return false;
}

static void topbar_refresh(lv_timer_t *t) {
    (void)t;
    if (!tb_clock) return;
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    char b[16]; snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(tb_clock, b);

    bool usb_up = usb_client_connected();
    if (usb_up) lv_obj_clear_flag(tb_usb, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(tb_usb, LV_OBJ_FLAG_HIDDEN);

    bool wifi_up = read_int_file("/sys/class/net/wlan0/carrier") == 1;
    if (wifi_up) {
        lv_obj_clear_flag(tb_wifi, LV_OBJ_FLAG_HIDDEN);
        bool ap = sys_hotspot_active();
        lv_obj_set_style_text_color(tb_wifi, lv_color_hex(ap ? CY_MAGENTA : CY_CYAN), 0);
    } else {
        lv_obj_add_flag(tb_wifi, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_topbar(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 26);
    flat(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(bar, 8, 0);
    lv_obj_set_style_pad_right(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = label(bar, "NODE-7F3A", FONT_MONO, CY_CYAN);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    /* cluster d'icônes + horloge à droite */
    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_PCT(100));
    flat(right);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    tb_usb = label(right, LV_SYMBOL_USB, FONT_SMALL, CY_CYAN);
    lv_obj_add_flag(tb_usb, LV_OBJ_FLAG_HIDDEN);
    tb_wifi = label(right, LV_SYMBOL_WIFI, FONT_SMALL, CY_CYAN);
    lv_obj_add_flag(tb_wifi, LV_OBJ_FLAG_HIDDEN);
    tb_clock = label(right, "--:--", FONT_MONO, CY_TEXT);
}

/* ---------------------------------------------------------------- nav basse */
static void nav_cb(lv_event_t *e) {
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    show_tab(tab);
}

static void build_nav(lv_obj_t *parent) {
    static const char *labels[3] = { LV_SYMBOL_ENVELOPE " CHAT",
                                     LV_SYMBOL_GPS " NODES",
                                     LV_SYMBOL_SETTINGS " SYS" };
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    flat(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_set_height(b, 28);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = label(b, labels[i], FONT_SMALL, CY_DIM);
        lv_obj_center(l);
        nav_btn[i] = b;
    }
}

static void refresh_nav(void) {
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = lv_obj_get_child(nav_btn[i], 0);
        bool active = (i == cur_tab);
        lv_obj_set_style_text_color(l, lv_color_hex(active ? CY_CYAN : CY_DIM), 0);
        lv_obj_set_style_bg_opa(nav_btn[i], active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(nav_btn[i], lv_color_hex(CY_CYAN), 0);
    }
}

/* ---------------------------------------------------------------- vue CHAT */
static lv_obj_t *msg_list;
static lv_obj_t *compose_ta;

static void add_bubble(lv_obj_t *parent, const mesh_message_t *m) {
    const mesh_channel_t *c = mesh_channel(m->ch);

    /* conteneur pleine largeur : aligne la bulle à gauche (reçu) ou droite (envoyé) */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    flat(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m->out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *b = lv_obj_create(row);
    lv_obj_set_width(b, LV_PCT(80));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    panel(b, m->out ? CY_CYAN : CY_BORDER);
    lv_obj_set_style_bg_color(b, lv_color_hex(m->out ? CY_PANEL2 : CY_PANEL), 0);
    lv_obj_set_style_pad_all(b, 5, 0);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);

    char head[64];
    const char *mark = m->out ? (m->ack == 2 ? "  " LV_SYMBOL_OK : "  " LV_SYMBOL_UPLOAD) : "";
    snprintf(head, sizeof(head), "%s  %s%s", m->from, m->time, mark);
    lv_obj_t *h = label(b, head, FONT_SMALL, c->enc ? CY_MAGENTA : CY_CYAN);
    lv_obj_set_width(h, LV_PCT(100));
    if (m->out && m->ack == 2) lv_obj_set_style_text_color(h, lv_color_hex(CY_GREEN), 0);

    lv_obj_t *t = label(b, m->text, FONT_BODY, CY_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_PCT(100));
}

static void rebuild_messages(void) {
    lv_obj_clean(msg_list);
    int n = mesh_message_count(cur_chan);
    for (int i = 0; i < n; i++) {
        const mesh_message_t *m = mesh_message(cur_chan, i);
        if (m) add_bubble(msg_list, m);
    }
    lv_obj_t *last = lv_obj_get_child(msg_list, -1);
    if (last) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
}

static void chan_cb(lv_event_t *e) {
    cur_chan = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    show_tab(0);
}

static void send_cb(lv_event_t *e) {
    (void)e;
    const char *txt = lv_textarea_get_text(compose_ta);
    if (txt && txt[0]) {
        mesh_send(cur_chan, txt);
        lv_textarea_set_text(compose_ta, "");
        rebuild_messages();
    }
}

static void ta_cb(lv_event_t *e) {
    (void)e;
    lv_keyboard_set_textarea(kb, compose_ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

static void kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) send_cb(e);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void build_chat(void) {
    /* rangée des canaux */
    lv_obj_t *chans = lv_obj_create(content);
    lv_obj_set_size(chans, LV_PCT(100), 28);
    flat(chans);
    lv_obj_set_flex_flow(chans, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chans, 4, 0);
    lv_obj_set_style_pad_all(chans, 3, 0);
    lv_obj_clear_flag(chans, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < mesh_channel_count(); i++) {
        const mesh_channel_t *c = mesh_channel(i);
        lv_obj_t *chip = lv_button_create(chans);
        lv_obj_set_height(chip, 22);
        lv_obj_set_style_radius(chip, 2, 0);
        lv_obj_set_style_shadow_width(chip, 0, 0);
        bool active = (i == cur_chan);
        uint32_t col = c->enc ? CY_MAGENTA : CY_CYAN;
        lv_obj_set_style_bg_opa(chip, active ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(col), 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(active ? col : CY_BORDER), 0);
        lv_obj_add_event_cb(chip, chan_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char nm[32];
        snprintf(nm, sizeof(nm), "%s%s", c->enc ? LV_SYMBOL_EYE_CLOSE " " : "# ", c->name);
        lv_obj_t *l = label(chip, nm, FONT_SMALL, active ? CY_TEXT : col);
        lv_obj_center(l);
    }

    /* liste des messages (défilante) */
    msg_list = lv_obj_create(content);
    lv_obj_set_width(msg_list, LV_PCT(100));
    lv_obj_set_flex_grow(msg_list, 1);
    flat(msg_list);
    lv_obj_set_flex_flow(msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(msg_list, 6, 0);
    lv_obj_set_style_pad_all(msg_list, 4, 0);
    lv_obj_set_scroll_dir(msg_list, LV_DIR_VER);

    /* barre de composition */
    lv_obj_t *bar = lv_obj_create(content);
    lv_obj_set_size(bar, LV_PCT(100), 34);
    flat(bar);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    compose_ta = lv_textarea_create(bar);
    lv_textarea_set_one_line(compose_ta, true);
    lv_textarea_set_placeholder_text(compose_ta, "message...");
    lv_obj_set_flex_grow(compose_ta, 1);
    lv_obj_set_height(compose_ta, 30);
    lv_obj_set_style_bg_color(compose_ta, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_border_color(compose_ta, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_border_width(compose_ta, 1, 0);
    lv_obj_set_style_radius(compose_ta, 2, 0);
    lv_obj_set_style_text_color(compose_ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(compose_ta, FONT_BODY, 0);
    lv_obj_add_event_cb(compose_ta, ta_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send = lv_button_create(bar);
    lv_obj_set_size(send, 44, 30);
    lv_obj_set_style_radius(send, 2, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_add_event_cb(send, send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = label(send, LV_SYMBOL_UPLOAD, FONT_BODY, CY_BG);
    lv_obj_center(sl);

    rebuild_messages();
}

/* ---------------------------------------------------------------- vue NODES */
static void build_nodes(void) {
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    flat(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_set_style_pad_all(list, 5, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < mesh_node_count(); i++) {
        const mesh_node_t *n = mesh_node(i);
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        panel(row, n->self ? CY_CYAN : CY_BORDER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = label(row, n->name, FONT_BODY, n->self ? CY_CYAN : CY_TEXT);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *id = label(row, n->id, FONT_SMALL, CY_DIM);
        lv_obj_align(id, LV_ALIGN_TOP_RIGHT, 0, 0);

        char stat[80];
        snprintf(stat, sizeof(stat), "SNR %ddB  RSSI %ddBm  " LV_SYMBOL_CHARGE "%d%%  %dhop  %s",
                 n->snr, n->rssi, n->batt, n->hops, n->last);
        lv_obj_t *st = label(row, stat, FONT_SMALL, CY_DIM);
        lv_obj_align(st, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_pad_top(st, 18, 0);
    }
}

/* ---------------------------------------------------------------- vue SYS */
static lv_obj_t *sys_lbl_host, *sys_lbl_ipw, *sys_lbl_ipu, *sys_lbl_uptime;
static lv_obj_t *sys_lbl_cpu, *sys_lbl_mem, *sys_lbl_disk, *sys_lbl_thr, *sys_lbl_kernel;
static lv_obj_t *sys_lbl_ssh_state, *sys_lbl_ssh_btn, *sys_btn_ssh;
static lv_obj_t *sys_lbl_wifi;
static lv_obj_t *sys_lbl_hot_state, *sys_lbl_hot_btn;
static lv_obj_t *sys_lbl_usb_state, *sys_lbl_usb_ip;
static lv_obj_t *sys_log_ta;
static lv_obj_t *sys_bl_slider, *sys_bl_lbl;
static lv_timer_t *sys_refresh_timer;

static lv_obj_t *info_row(lv_obj_t *parent, const char *key) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    flat(r);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(r, 1, 0);
    label(r, key, FONT_SMALL, CY_DIM);
    lv_obj_t *v = label(r, "-", FONT_SMALL, CY_TEXT);
    return v;
}

static lv_obj_t *section(lv_obj_t *parent, const char *title) {
    label(parent, title, FONT_SMALL, CY_CYAN);
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
    panel(p, CY_BORDER);
    lv_obj_set_style_pad_all(p, 6, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 3, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *txt, uint32_t color, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 32);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, txt, FONT_SMALL, CY_TEXT);
    lv_obj_center(l);
    return b;
}

static void confirm_dialog(const char *msg, void (*on_yes)(void));
static void reboot_yes(void)      { sys_reboot(); }
static void shutdown_yes(void)    { sys_shutdown(); }
static void restart_app_yes(void) { sys_restart_app(); }
static void reboot_cb(lv_event_t *e)      { (void)e; confirm_dialog("Redemarrer le Pi ?", reboot_yes); }
static void shutdown_cb(lv_event_t *e)    { (void)e; confirm_dialog("Eteindre le Pi ?",    shutdown_yes); }
static void restart_app_cb(lv_event_t *e) { (void)e; confirm_dialog("Relancer meshui ?",   restart_app_yes); }
static void calib_cb(lv_event_t *e)    { (void)e; calib_start(NULL); }
static void ssh_toggle_cb(lv_event_t *e) { (void)e; sys_ssh_set(!sys_ssh_running()); }

static void hot_yes(void)        { sys_hotspot_set(!sys_hotspot_active()); }
static void beep_cb(lv_event_t *e) { (void)e; sys_beep(1500, 120); }

static void bl_slider_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = (int)lv_slider_get_value(s);
    if (sys_bl_lbl) {
        char b[8]; snprintf(b, sizeof(b), "%d%%", v);
        lv_label_set_text(sys_bl_lbl, b);
    }
    sys_backlight_set(v);
}

static void log_refresh(lv_obj_t *ta) {
    static char buf[4096];
    sys_log_tail(buf, sizeof(buf), 30);
    lv_textarea_set_text(ta, buf);
    /* place le curseur en bas pour scroller au dernier message */
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
}
static void log_refresh_cb(lv_event_t *e) { (void)e; if (sys_log_ta) log_refresh(sys_log_ta); }

/* QR code WiFi du hotspot : ouvre un modal plein écran avec le QR */
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

    lv_obj_t *t = label(qr_ov, "Scanner pour rejoindre " HOTSPOT_SSID, FONT_SMALL, CY_CYAN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    /* WIFI:T:WPA;S:<ssid>;P:<pass>;; */
    char payload[160];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", HOTSPOT_SSID, HOTSPOT_PASS);
    lv_obj_t *qr = lv_qrcode_create(qr_ov);
    lv_qrcode_set_size(qr, 260);
    lv_qrcode_set_dark_color(qr, lv_color_hex(CY_CYAN));
    lv_qrcode_set_light_color(qr, lv_color_hex(CY_BG));
    lv_qrcode_update(qr, payload, (int32_t)strlen(payload));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -10);

    char cred[96]; snprintf(cred, sizeof(cred), "SSID  %s\npass  %s", HOTSPOT_SSID, HOTSPOT_PASS);
    lv_obj_t *c = label(qr_ov, cred, FONT_SMALL, CY_TEXT);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 140);

    lv_obj_t *bar = lv_obj_create(qr_ov);
    lv_obj_set_size(bar, LV_PCT(100), 36);
    flat(bar); lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    small_button(bar, LV_SYMBOL_CLOSE "  FERMER", CY_DIM, qr_close_cb);
}
static void hot_toggle_cb(lv_event_t *e) {
    (void)e;
    bool on = sys_hotspot_active();
    confirm_dialog(on ? "Desactiver le hotspot ?"
                      : "Activer le hotspot ?\n(coupe le WiFi actuel)",
                   hot_yes);
}
static void wifi_modal_open(void);
static void wifi_btn_cb(lv_event_t *e) { (void)e; wifi_modal_open(); }

static void sys_refresh(lv_timer_t *t) {
    (void)t;
    if (!sys_lbl_host) return;
    sys_info_t i;
    sys_info_get(&i);
    char b[80];
    lv_label_set_text(sys_lbl_host,   i.hostname);
    lv_label_set_text(sys_lbl_ipw,    i.ip_wlan);
    lv_label_set_text(sys_lbl_ipu,    i.ip_usb);
    lv_label_set_text(sys_lbl_uptime, i.uptime);
    snprintf(b, sizeof(b), "%.1f C", i.cpu_temp_c);  lv_label_set_text(sys_lbl_cpu, b);
    snprintf(b, sizeof(b), "%d / %d MB", i.mem_used_mb, i.mem_total_mb); lv_label_set_text(sys_lbl_mem, b);
    snprintf(b, sizeof(b), "%d %%", i.disk_used_pct); lv_label_set_text(sys_lbl_disk, b);
    lv_label_set_text(sys_lbl_thr, i.throttled_now ? "SOUS-TENSION" : (i.throttled_ever ? "deja eu" : "ok"));
    lv_obj_set_style_text_color(sys_lbl_thr,
        lv_color_hex(i.throttled_now ? CY_AMBER : (i.throttled_ever ? CY_DIM : CY_GREEN)), 0);
    lv_label_set_text(sys_lbl_kernel, i.kernel);

    bool running = sys_ssh_running();
    lv_label_set_text(sys_lbl_ssh_state, running ? "actif" : "arrete");
    lv_obj_set_style_text_color(sys_lbl_ssh_state,
        lv_color_hex(running ? CY_GREEN : CY_DIM), 0);
    lv_label_set_text(sys_lbl_ssh_btn, running ? "DESACTIVER" : "ACTIVER");

    if (i.wifi_signal >= 0)
        snprintf(b, sizeof(b), "%s  (%d%%)", i.wifi_ssid, i.wifi_signal);
    else
        snprintf(b, sizeof(b), "%s", i.wifi_ssid);
    lv_label_set_text(sys_lbl_wifi, b);

    if (sys_lbl_hot_state) {
        bool hot = sys_hotspot_active();
        lv_label_set_text(sys_lbl_hot_state, hot ? "actif" : "inactif");
        lv_obj_set_style_text_color(sys_lbl_hot_state,
            lv_color_hex(hot ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_hot_btn, hot ? "DESACTIVER" : "ACTIVER");
    }

    if (sys_lbl_usb_state) {
        bool usb_up = usb_client_connected();
        lv_label_set_text(sys_lbl_usb_state, usb_up ? "connecte" : "deconnecte");
        lv_obj_set_style_text_color(sys_lbl_usb_state,
            lv_color_hex(usb_up ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_usb_ip, i.ip_usb);
    }
}

static void build_sys(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 5, 0);
    lv_obj_set_style_pad_row(col, 3, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    lv_obj_t *s = section(col, "INFO");
    sys_lbl_host   = info_row(s, "hostname");
    sys_lbl_ipw    = info_row(s, "ip wlan");
    sys_lbl_ipu    = info_row(s, "ip usb");
    sys_lbl_uptime = info_row(s, "uptime");
    sys_lbl_cpu    = info_row(s, "cpu");
    sys_lbl_mem    = info_row(s, "ram");
    sys_lbl_disk   = info_row(s, "disque /");
    sys_lbl_thr    = info_row(s, "alim");
    sys_lbl_kernel = info_row(s, "noyau");

    s = section(col, "ALIMENTATION");
    lv_obj_t *row = lv_obj_create(s);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0); lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, LV_SYMBOL_POWER "  ETEINDRE",   CY_MAGENTA, shutdown_cb);
    small_button(row, LV_SYMBOL_REFRESH "  REDEMARRER", CY_CYAN,  reboot_cb);

    s = section(col, "SSH");
    lv_obj_t *r2 = lv_obj_create(s);
    lv_obj_set_size(r2, LV_PCT(100), LV_SIZE_CONTENT);
    flat(r2); lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r2, LV_OBJ_FLAG_SCROLLABLE);
    sys_lbl_ssh_state = label(r2, "?", FONT_BODY, CY_DIM);
    sys_btn_ssh = lv_button_create(r2);
    lv_obj_set_size(sys_btn_ssh, 130, 30);
    lv_obj_set_style_radius(sys_btn_ssh, 2, 0);
    lv_obj_set_style_bg_opa(sys_btn_ssh, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(sys_btn_ssh, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(sys_btn_ssh, 1, 0);
    lv_obj_set_style_border_color(sys_btn_ssh, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_shadow_width(sys_btn_ssh, 0, 0);
    lv_obj_add_event_cb(sys_btn_ssh, ssh_toggle_cb, LV_EVENT_CLICKED, NULL);
    sys_lbl_ssh_btn = label(sys_btn_ssh, "?", FONT_SMALL, CY_TEXT);
    lv_obj_center(sys_lbl_ssh_btn);

    s = section(col, "WIFI");
    lv_obj_t *r3 = lv_obj_create(s);
    lv_obj_set_size(r3, LV_PCT(100), LV_SIZE_CONTENT);
    flat(r3); lv_obj_set_flex_flow(r3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r3, LV_OBJ_FLAG_SCROLLABLE);
    sys_lbl_wifi = label(r3, "-", FONT_SMALL, CY_TEXT);
    lv_obj_t *wbtn = lv_button_create(r3);
    lv_obj_set_size(wbtn, 110, 30);
    lv_obj_set_style_radius(wbtn, 2, 0);
    lv_obj_set_style_bg_opa(wbtn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(wbtn, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_border_width(wbtn, 1, 0);
    lv_obj_set_style_border_color(wbtn, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_shadow_width(wbtn, 0, 0);
    lv_obj_add_event_cb(wbtn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = label(wbtn, LV_SYMBOL_WIFI "  RESEAUX", FONT_SMALL, CY_TEXT);
    lv_obj_center(wl);

    s = section(col, "HOTSPOT");
    lv_obj_t *r4 = lv_obj_create(s);
    lv_obj_set_size(r4, LV_PCT(100), LV_SIZE_CONTENT);
    flat(r4); lv_obj_set_flex_flow(r4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r4, LV_OBJ_FLAG_SCROLLABLE);
    sys_lbl_hot_state = label(r4, "?", FONT_BODY, CY_DIM);
    lv_obj_t *hbtn = lv_button_create(r4);
    lv_obj_set_size(hbtn, 130, 30);
    lv_obj_set_style_radius(hbtn, 2, 0);
    lv_obj_set_style_bg_opa(hbtn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(hbtn, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_border_width(hbtn, 1, 0);
    lv_obj_set_style_border_color(hbtn, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_shadow_width(hbtn, 0, 0);
    lv_obj_add_event_cb(hbtn, hot_toggle_cb, LV_EVENT_CLICKED, NULL);
    sys_lbl_hot_btn = label(hbtn, "?", FONT_SMALL, CY_TEXT);
    lv_obj_center(sys_lbl_hot_btn);
    lv_obj_t *credline = label(s, "SSID: " HOTSPOT_SSID "   pass: " HOTSPOT_PASS, FONT_SMALL, CY_DIM);
    (void)credline;
    small_button(s, LV_SYMBOL_IMAGE "  QR CODE WIFI", CY_MAGENTA, qr_open_cb);

    s = section(col, "USB");
    sys_lbl_usb_state = info_row(s, "etat");
    sys_lbl_usb_ip    = info_row(s, "ip Pi");
    lv_obj_t *usbhint = label(s, "Brancher PC sur le port USB (milieu) du Pi", FONT_SMALL, CY_DIM);
    (void)usbhint;

    s = section(col, "APPLICATION");
    small_button(s, LV_SYMBOL_REFRESH "  RELANCER MESHUI", CY_CYAN, restart_app_cb);

    s = section(col, "ECRAN");
    /* Luminosité — slider en % */
    lv_obj_t *brow = lv_obj_create(s);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(brow);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
    label(brow, "luminosite", FONT_SMALL, CY_DIM);
    sys_bl_lbl = label(brow, "?%", FONT_SMALL, CY_CYAN);
    sys_bl_slider = lv_slider_create(s);
    lv_obj_set_size(sys_bl_slider, LV_PCT(100), 14);
    lv_slider_set_range(sys_bl_slider, 5, 100);   /* min 5% pour ne pas eteindre */
    int cur_bl = sys_backlight_get();
    if (cur_bl < 5) cur_bl = 100;
    lv_slider_set_value(sys_bl_slider, cur_bl, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sys_bl_slider, lv_color_hex(CY_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sys_bl_slider, lv_color_hex(CY_CYAN),   LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sys_bl_slider, lv_color_hex(CY_CYAN),   LV_PART_KNOB);
    lv_obj_set_style_radius(sys_bl_slider, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sys_bl_slider, 2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sys_bl_slider, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(sys_bl_slider, bl_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    char b0[8]; snprintf(b0, sizeof(b0), "%d%%", cur_bl);
    lv_label_set_text(sys_bl_lbl, b0);

    /* Actions ecran */
    lv_obj_t *erow = lv_obj_create(s);
    lv_obj_set_size(erow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(erow); lv_obj_set_flex_flow(erow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(erow, 6, 0);
    lv_obj_clear_flag(erow, LV_OBJ_FLAG_SCROLLABLE);
    small_button(erow, LV_SYMBOL_AUDIO "  BIP",         CY_MAGENTA, beep_cb);
    small_button(erow, LV_SYMBOL_GPS   "  CALIBRER",    CY_CYAN,    calib_cb);

    /* Section LOG */
    s = section(col, "LOG SYSTEME");
    sys_log_ta = lv_textarea_create(s);
    lv_obj_set_size(sys_log_ta, LV_PCT(100), 140);
    lv_textarea_set_one_line(sys_log_ta, false);
    lv_obj_set_style_bg_color(sys_log_ta, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_text_color(sys_log_ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(sys_log_ta, FONT_SMALL, 0);
    lv_obj_set_style_border_color(sys_log_ta, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_border_width(sys_log_ta, 1, 0);
    lv_obj_set_style_radius(sys_log_ta, 2, 0);
    lv_obj_set_style_pad_all(sys_log_ta, 4, 0);
    lv_textarea_set_text(sys_log_ta, "");
    log_refresh(sys_log_ta);
    small_button(s, LV_SYMBOL_REFRESH "  RAFRAICHIR", CY_CYAN, log_refresh_cb);

    sys_refresh(NULL);
    sys_refresh_timer = lv_timer_create(sys_refresh, 5000, NULL);
}

/* ------------- confirmation modale ------------- */
static lv_obj_t *confirm_ov;
static void (*confirm_yes_cb)(void);
static void confirm_close(void) { if (confirm_ov) { lv_obj_delete(confirm_ov); confirm_ov = NULL; } }
static void confirm_yes_e(lv_event_t *e) { (void)e; void (*cb)(void) = confirm_yes_cb; confirm_close(); if (cb) cb(); }
static void confirm_no_e (lv_event_t *e) { (void)e; confirm_close(); }
static void confirm_dialog(const char *msg, void (*on_yes)(void)) {
    confirm_yes_cb = on_yes;
    confirm_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(confirm_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(confirm_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(confirm_ov, LV_OPA_80, 0);
    lv_obj_set_style_border_width(confirm_ov, 0, 0);
    lv_obj_set_style_radius(confirm_ov, 0, 0);
    lv_obj_clear_flag(confirm_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *box = lv_obj_create(confirm_ov);
    lv_obj_set_size(box, 260, 140);
    lv_obj_center(box);
    panel(box, CY_CYAN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = label(box, msg, FONT_BODY, CY_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, "ANNULER",   CY_DIM,     confirm_no_e);
    small_button(row, "CONFIRMER", CY_MAGENTA, confirm_yes_e);
}

/* ------------- modal WiFi ------------- */
static lv_obj_t *wifi_ov, *wifi_list_ov, *wifi_status, *wifi_pwd_panel, *wifi_pwd_ta;
static char wifi_pending_ssid[64];

static void wifi_modal_close_e(lv_event_t *e) { (void)e; if (wifi_ov) { lv_obj_delete(wifi_ov); wifi_ov = NULL; } }

static void wifi_scan_done(const wifi_net_t *list, int n, void *user);
static void wifi_rescan_e(lv_event_t *e) {
    (void)e;
    lv_label_set_text(wifi_status, "scan en cours...");
    if (wifi_list_ov) lv_obj_clean(wifi_list_ov);
    sys_wifi_scan_async(wifi_scan_done, NULL);
}

static void wifi_connect_done(bool ok, const char *msg, void *user) {
    (void)user;
    if (!wifi_status) return;
    lv_label_set_text(wifi_status, ok ? "connecte" : msg);
    lv_obj_set_style_text_color(wifi_status, lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
}

static void wifi_pwd_ok_e(lv_event_t *e) {
    (void)e;
    const char *p = lv_textarea_get_text(wifi_pwd_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (wifi_pwd_panel) { lv_obj_delete(wifi_pwd_panel); wifi_pwd_panel = NULL; }
    lv_label_set_text(wifi_status, "connexion...");
    lv_obj_set_style_text_color(wifi_status, lv_color_hex(CY_CYAN), 0);
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
    lv_textarea_set_placeholder_text(wifi_pwd_ta, "passphrase");
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
    small_button(rr, "ANNULER",  CY_DIM,     wifi_pwd_cancel_e);
    small_button(rr, "CONNECTER", CY_MAGENTA, wifi_pwd_ok_e);
}

static void wifi_row_cb(lv_event_t *e) {
    wifi_net_t *n = lv_event_get_user_data(e);
    if (n->secured) {
        wifi_open_pwd(n->ssid);
    } else {
        lv_label_set_text(wifi_status, "connexion...");
        sys_wifi_connect_async(n->ssid, "", wifi_connect_done, NULL);
    }
}

static wifi_net_t wifi_kept[32];
static int wifi_kept_n = 0;

static void wifi_scan_done(const wifi_net_t *list, int n, void *user) {
    (void)user;
    if (!wifi_list_ov) return;
    lv_obj_clean(wifi_list_ov);
    if (n == 0) { lv_label_set_text(wifi_status, "aucun reseau"); return; }
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

static void wifi_modal_open(void) {
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
    small_button(bar, LV_SYMBOL_LEFT "  FERMER",   CY_DIM,  wifi_modal_close_e);
    small_button(bar, LV_SYMBOL_REFRESH "  RESCAN", CY_CYAN, wifi_rescan_e);
    wifi_status = label(wifi_ov, "scan en cours...", FONT_SMALL, CY_CYAN);
    lv_obj_align(wifi_status, LV_ALIGN_TOP_MID, 0, 44);
    wifi_list_ov = lv_obj_create(wifi_ov);
    lv_obj_set_size(wifi_list_ov, LV_PCT(100), LV_PCT(100) - 70);
    lv_obj_align(wifi_list_ov, LV_ALIGN_BOTTOM_MID, 0, -2);
    flat(wifi_list_ov);
    lv_obj_set_flex_flow(wifi_list_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list_ov, 4, 0);
    lv_obj_set_scroll_dir(wifi_list_ov, LV_DIR_VER);
    sys_wifi_scan_async(wifi_scan_done, NULL);
}

/* ---------------------------------------------------------------- routage */
static void show_tab(int tab) {
    if (sys_refresh_timer) { lv_timer_delete(sys_refresh_timer); sys_refresh_timer = NULL; }
    sys_lbl_host = NULL;   /* invalidé par lv_obj_clean */
    cur_tab = tab;
    lv_obj_clean(content);
    if (tab == 0)      build_chat();
    else if (tab == 1) build_nodes();
    else               build_sys();
    refresh_nav();
}

/* ---------------------------------------------------------------- splash */
static lv_obj_t *splash_ov;
static void (*splash_done)(void);

static void bar_anim_cb(void *bar, int32_t v) {
    lv_bar_set_value((lv_obj_t *)bar, v, LV_ANIM_OFF);
}

static void splash_close(lv_timer_t *t) {
    lv_timer_delete(t);
    lv_obj_delete(splash_ov);
    splash_ov = NULL;
    if (splash_done) splash_done();
}

void ui_show_splash(void (*done)(void)) {
    splash_done = done;

    splash_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(splash_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(splash_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_ov, 0, 0);
    lv_obj_set_style_radius(splash_ov, 0, 0);
    lv_obj_clear_flag(splash_ov, LV_OBJ_FLAG_SCROLLABLE);

    /* Crochets cyan aux 4 coins */
    static const int corners[4][4] = {
        {  6,    6,    1,  1 },
        { -7,    6,   -1,  1 },
        {  6,   -7,    1, -1 },
        { -7,   -7,   -1, -1 },
    };
    for (int i = 0; i < 4; i++) {
        int x = corners[i][0] >= 0 ? corners[i][0] : (320 + corners[i][0]);
        int y = corners[i][1] >= 0 ? corners[i][1] : (480 + corners[i][1]);
        int dx = corners[i][2], dy = corners[i][3];
        lv_obj_t *h = lv_obj_create(splash_ov);
        lv_obj_set_size(h, 22, 2);
        lv_obj_set_pos(h, dx > 0 ? x : x - 22, y);
        lv_obj_set_style_bg_color(h, lv_color_hex(CY_CYAN), 0);
        lv_obj_set_style_border_width(h, 0, 0); lv_obj_set_style_radius(h, 0, 0);
        lv_obj_t *v = lv_obj_create(splash_ov);
        lv_obj_set_size(v, 2, 22);
        lv_obj_set_pos(v, x, dy > 0 ? y : y - 22);
        lv_obj_set_style_bg_color(v, lv_color_hex(CY_CYAN), 0);
        lv_obj_set_style_border_width(v, 0, 0); lv_obj_set_style_radius(v, 0, 0);
    }

    lv_obj_t *title = label(splash_ov, "BugQuest", &lv_font_montserrat_28, CY_CYAN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *sub = label(splash_ov, "/ / L O R A", &lv_font_montserrat_20, CY_MAGENTA);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *divider = lv_obj_create(splash_ov);
    lv_obj_set_size(divider, 160, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_align(divider, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *node = label(splash_ov, "node // NODE-7F3A", FONT_SMALL, CY_DIM);
    lv_obj_align(node, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *bar = lv_bar_create(splash_ov);
    lv_obj_set_size(bar, 180, 4);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 52);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CY_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CY_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);

    lv_obj_t *foot = label(splash_ov, "[ initialisation ]", FONT_SMALL, CY_CYAN);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_t *ver = label(splash_ov, "v0.1", FONT_SMALL, CY_DIM);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, bar_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 1500);
    lv_anim_start(&a);

    lv_timer_create(splash_close, 1800, NULL);
}

/* ---------------------------------------------------------------- init */
void ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_topbar(scr);
    topbar_refresh(NULL);
    tb_timer = lv_timer_create(topbar_refresh, 5000, NULL);

    content = lv_obj_create(scr);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    flat(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    build_nav(scr);

    /* clavier overlay sur le top layer : visible au-dessus de tout (chat + modaux) */
    kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(55));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);

    show_tab(0);
}
