#include "lvgl/lvgl.h"
#include "ui.h"
#include "theme.h"
#include "mesh.h"
#include "calib.h"
#include "sys.h"
#include "settings.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <strings.h>

/* ---------------------------------------------------------------- état */
/* IDs d'app (cur_tab garde son nom mais sémantique = app courante) */
enum {
    APP_HOME    = 0,
    APP_CHAT    = 1,
    APP_NODES   = 2,
    APP_SYS     = 3,
    APP_WIFI    = 4,
    APP_HOTSPOT = 5,
    APP_BADUSB  = 6,
};

static lv_obj_t *content;          /* zone centrale, reconstruite par app */
static lv_obj_t *kb;               /* clavier virtuel (overlay, masqué) */
static uint8_t   cur_chan = 0;     /* canal courant dans la vue chat */
static int       cur_tab  = APP_HOME;

static void show_tab(int app);

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
static lv_obj_t *tb_clock, *tb_usb, *tb_wifi, *tb_name, *tb_back;
static lv_timer_t *tb_timer;
static void tb_back_cb(lv_event_t *e) { (void)e; show_tab(APP_HOME); }

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
    if (tb_name) lv_label_set_text(tb_name, settings_node_name());
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    char b[16]; snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(tb_clock, b);

    bool usb_up = usb_client_connected();
    lv_obj_set_style_text_color(tb_usb, lv_color_hex(usb_up ? CY_CYAN : CY_BORDER), 0);

    bool wifi_up = read_int_file("/sys/class/net/wlan0/carrier") == 1;
    bool ap = sys_hotspot_active();
    uint32_t wcol = ap ? CY_MAGENTA : (wifi_up ? CY_CYAN : CY_BORDER);
    lv_obj_set_style_text_color(tb_wifi, lv_color_hex(wcol), 0);
}

static void build_topbar(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 32);
    flat(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(bar, 8, 0);
    lv_obj_set_style_pad_right(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* cluster gauche : bouton retour (visible hors HOME) + nom du noeud */
    tb_back = lv_button_create(bar);
    lv_obj_set_size(tb_back, 32, 22);
    lv_obj_align(tb_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(tb_back, 2, 0);
    lv_obj_set_style_bg_opa(tb_back, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(tb_back, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(tb_back, 0, 0);
    lv_obj_set_style_shadow_width(tb_back, 0, 0);
    lv_obj_add_event_cb(tb_back, tb_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = label(tb_back, LV_SYMBOL_LEFT, FONT_SMALL, CY_TEXT);
    lv_obj_center(bl);
    lv_obj_add_flag(tb_back, LV_OBJ_FLAG_HIDDEN);

    tb_name = label(bar, settings_node_name(), FONT_MONO, CY_CYAN);
    lv_obj_set_width(tb_name, 175);     /* borne pour ne pas chevaucher le cluster droit */
    lv_label_set_long_mode(tb_name, LV_LABEL_LONG_DOT);
    lv_obj_align(tb_name, LV_ALIGN_LEFT_MID, 40, 0);

    /* cluster d'icônes + horloge à droite */
    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_PCT(100));
    flat(right);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* Icones toujours visibles, color = etat (dim si decoonnecte) */
    tb_usb = label(right, LV_SYMBOL_USB, FONT_SMALL, CY_BORDER);
    tb_wifi = label(right, LV_SYMBOL_WIFI, FONT_SMALL, CY_BORDER);
    tb_clock = label(right, "--:--", FONT_MONO, CY_TEXT);
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
    show_tab(APP_CHAT);
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
    /* valider n'envoie un message que si on est bien dans la zone compose du chat */
    if (code == LV_EVENT_READY && cur_tab == APP_CHAT && compose_ta &&
        lv_keyboard_get_textarea(kb) == compose_ta) {
        send_cb(e);
    }
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
static lv_obj_t *sys_lbl_wifi_radio_state, *sys_lbl_wifi_radio_btn;
static lv_obj_t *sys_lbl_bt_state, *sys_lbl_bt_btn;
static lv_obj_t *sys_lbl_usb_mode_state, *sys_lbl_usb_mode_btn;
static lv_obj_t *hap_lbl_state, *hap_lbl_btn;     /* app HOTSPOT */
static lv_obj_t *bap_lbl_state, *bap_lbl_btn;     /* app BAD USB */
static lv_obj_t *bap_btn_ncm, *bap_btn_hid, *bap_btn_storage; /* selecteur 3 modes */
static lv_obj_t *upd_lbl_state, *upd_lbl_hash, *upd_btn_install; /* MISES A JOUR */
static lv_obj_t *sys_btn_usb_share, *sys_btn_usb_client;          /* USB > INTERNET */
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

static void wifi_radio_yes(void) { sys_wifi_radio_set(!sys_wifi_radio_on()); }
static void wifi_radio_toggle_cb(lv_event_t *e) {
    (void)e;
    if (sys_wifi_radio_on())
        confirm_dialog("Couper le WiFi ?\n(coupe la session SSH WiFi)", wifi_radio_yes);
    else
        wifi_radio_yes();
}

static void bt_yes(void) { sys_bt_set(!sys_bt_on()); }
static void bt_toggle_cb(lv_event_t *e) { (void)e; bt_yes(); }

/* USB > INTERNET : Pi sert le DHCP OU Pi recoit l'IP via ICS Windows */
static void usb_net_share_yes(void)  { sys_usb_net_set(USB_NET_SHARED); }
static void usb_net_client_yes(void) { sys_usb_net_set(USB_NET_CLIENT); }
static void usb_net_share_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_net_mode() == USB_NET_SHARED) return;
    confirm_dialog("Pi serveur DHCP USB ?\n(pas d'internet sur Pi)", usb_net_share_yes);
}
static void usb_net_client_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_net_mode() == USB_NET_CLIENT) return;
    confirm_dialog("Recevoir internet du PC ?\n(active d'abord l'ICS Windows)", usb_net_client_yes);
}

/* ----- Mises a jour OTA ----- */
static void upd_check_done_cb(bool avail, const char *loc, const char *rem, void *u) {
    (void)u;
    if (!upd_lbl_state) return;
    char b[64];
    snprintf(b, sizeof(b), "local %s   distant %s", loc, rem);
    if (upd_lbl_hash) lv_label_set_text(upd_lbl_hash, b);
    lv_label_set_text(upd_lbl_state,
        avail ? "mise a jour disponible" : "a jour");
    lv_obj_set_style_text_color(upd_lbl_state,
        lv_color_hex(avail ? CY_AMBER : CY_GREEN), 0);
    if (upd_btn_install) {
        if (avail) lv_obj_clear_flag(upd_btn_install, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(upd_btn_install, LV_OBJ_FLAG_HIDDEN);
    }
}
static void upd_check_cb(lv_event_t *e) {
    (void)e;
    if (upd_lbl_state) {
        lv_label_set_text(upd_lbl_state, "verification...");
        lv_obj_set_style_text_color(upd_lbl_state, lv_color_hex(CY_CYAN), 0);
    }
    sys_update_check_async(upd_check_done_cb, NULL);
}
static void upd_apply_done_cb(bool ok, void *u) { (void)ok; (void)u; }
static void upd_apply_yes(void) {
    if (upd_lbl_state) {
        lv_label_set_text(upd_lbl_state, "installation...");
        lv_obj_set_style_text_color(upd_lbl_state, lv_color_hex(CY_MAGENTA), 0);
    }
    sys_update_apply_async(upd_apply_done_cb, NULL);
}
static void upd_apply_cb(lv_event_t *e) {
    (void)e;
    confirm_dialog("Installer la maj ?\n(meshui redemarrera)", upd_apply_yes);
}

/* ----- BAD USB ----- */
#define BADUSB_DIR "/home/bq-lora/meshui/badusb"
static lv_obj_t *bad_run_ov, *bad_run_skull, *bad_run_status;
static lv_timer_t *bad_run_timer;
static int bad_run_frame;
static bool bad_run_active;

static const char *SKULL_FRAME_A =
"    .-=-.\n"
"   /     \\\n"
"  | O   O |\n"
"  |   v   |\n"
"   \\ === /\n"
"    '---'\n"
"    /   \\\n"
"   /  X  \\\n"
"  /   X   \\";
static const char *SKULL_FRAME_B =
"    .-=-.\n"
"   /     \\\n"
"  | -   - |\n"
"  |   v   |\n"
"   \\ === /\n"
"    '---'\n"
"    /   \\\n"
"   /  X  \\\n"
"  /   X   \\";

static void bad_run_tick(lv_timer_t *t) {
    (void)t;
    if (!bad_run_skull) return;
    bad_run_frame ^= 1;
    lv_label_set_text(bad_run_skull, bad_run_frame ? SKULL_FRAME_A : SKULL_FRAME_B);
    lv_obj_set_style_text_color(bad_run_skull,
        lv_color_hex(bad_run_frame ? CY_MAGENTA : CY_CYAN), 0);
}

static void bad_run_close_e(lv_event_t *e) {
    (void)e;
    if (bad_run_timer) { lv_timer_delete(bad_run_timer); bad_run_timer = NULL; }
    if (bad_run_ov)    { lv_obj_delete(bad_run_ov);     bad_run_ov = NULL; }
    bad_run_active = false;
}
static void bad_run_auto_close(lv_timer_t *t) { lv_timer_delete(t); bad_run_close_e(NULL); }

static void bad_run_done_cb(bool ok, void *user) {
    (void)user;
    if (!bad_run_status) return;
    lv_label_set_text(bad_run_status, ok ? "  TERMINE  " : "  ECHEC  ");
    lv_obj_set_style_text_color(bad_run_status,
        lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    if (bad_run_timer) { lv_timer_delete(bad_run_timer); bad_run_timer = NULL; }
    lv_timer_create(bad_run_auto_close, 1500, NULL);
}

static void bad_run_open(const char *path, const char *name) {
    if (bad_run_active) return;
    bad_run_active = true;
    bad_run_frame = 0;
    bad_run_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(bad_run_ov, 0, 0);
    lv_obj_set_size(bad_run_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bad_run_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(bad_run_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bad_run_ov, 0, 0);
    lv_obj_set_style_radius(bad_run_ov, 0, 0);
    lv_obj_set_style_pad_all(bad_run_ov, 0, 0);
    lv_obj_clear_flag(bad_run_ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = label(bad_run_ov, "// BAD USB //", FONT_MONO, CY_MAGENTA);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 10);

    bad_run_skull = label(bad_run_ov, SKULL_FRAME_A, FONT_MONO, CY_MAGENTA);
    lv_obj_set_style_text_line_space(bad_run_skull, 2, 0);
    lv_obj_align(bad_run_skull, LV_ALIGN_CENTER, 0, -20);

    char nb[80]; snprintf(nb, sizeof(nb), "exec : %s", name);
    lv_obj_t *nm = label(bad_run_ov, nb, FONT_SMALL, CY_DIM);
    lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -70);

    bad_run_status = label(bad_run_ov, "  EN COURS...  ", FONT_BODY, CY_CYAN);
    lv_obj_align(bad_run_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t *bar = lv_obj_create(bad_run_ov);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    flat(bar); lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    small_button(bar, LV_SYMBOL_CLOSE "  FERMER", CY_DIM, bad_run_close_e);

    bad_run_timer = lv_timer_create(bad_run_tick, 350, NULL);
    sys_badusb_run_async(path, bad_run_done_cb, NULL);
}

typedef struct { char path[256]; char name[64]; bool is_dir; } badusb_entry_t;
static badusb_entry_t bad_entries[32];
static int bad_entries_n = 0;
static char bap_cwd[256] = BADUSB_DIR;     /* dossier courant dans l'explorateur */
static lv_obj_t *bap_list_obj, *bap_lbl_path;

static int bap_entry_cmp(const void *a, const void *b) {
    const badusb_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;  /* dossiers d'abord */
    return strcmp(ea->name, eb->name);
}
static void bap_refresh_list(void);

static void bap_parent_cb(lv_event_t *e) {
    (void)e;
    if (strcmp(bap_cwd, BADUSB_DIR) == 0) return;
    char *p = strrchr(bap_cwd, '/');
    if (p && p > bap_cwd) *p = 0;
    /* securite : ne pas sortir de BADUSB_DIR */
    if (strncmp(bap_cwd, BADUSB_DIR, strlen(BADUSB_DIR)) != 0)
        strcpy(bap_cwd, BADUSB_DIR);
    bap_refresh_list();
}

static void bap_entry_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= bad_entries_n) return;
    badusb_entry_t *en = &bad_entries[idx];
    if (en->is_dir) {
        strncpy(bap_cwd, en->path, sizeof(bap_cwd) - 1);
        bap_cwd[sizeof(bap_cwd) - 1] = 0;
        bap_refresh_list();
        return;
    }
    if (sys_usb_mode() != USB_MODE_HID) {
        confirm_dialog("Active d'abord le mode CLAVIER", NULL);
        return;
    }
    bad_run_open(en->path, en->name);
}

static void bap_refresh_list(void) {
    if (!bap_list_obj) return;
    lv_obj_clean(bap_list_obj);
    bad_entries_n = 0;
    DIR *d = opendir(bap_cwd);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && bad_entries_n < (int)(sizeof(bad_entries)/sizeof(bad_entries[0]))) {
            if (de->d_name[0] == '.') continue;
            /* filtre les dechets crees par Windows / Mac sur la cle FAT */
            if (strcasecmp(de->d_name, "System Volume Information") == 0 ||
                strcasecmp(de->d_name, "$RECYCLE.BIN")               == 0 ||
                strcasecmp(de->d_name, "RECYCLER")                   == 0 ||
                strcasecmp(de->d_name, "desktop.ini")                == 0 ||
                strcasecmp(de->d_name, "Thumbs.db")                  == 0 ||
                strcasecmp(de->d_name, ".Trashes")                   == 0 ||
                strcasecmp(de->d_name, ".Spotlight-V100")            == 0 ||
                strcasecmp(de->d_name, ".fseventsd")                 == 0)
                continue;
            badusb_entry_t *e = &bad_entries[bad_entries_n];
            snprintf(e->path, sizeof(e->path), "%s/%s", bap_cwd, de->d_name);
            strncpy(e->name, de->d_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = 0;
            struct stat st;
            e->is_dir = (stat(e->path, &st) == 0 && S_ISDIR(st.st_mode));
            bad_entries_n++;
        }
        closedir(d);
    }
    qsort(bad_entries, bad_entries_n, sizeof(badusb_entry_t), bap_entry_cmp);

    /* Affiche le chemin relatif dans le label en tete */
    if (bap_lbl_path) {
        const char *rel = bap_cwd + strlen(BADUSB_DIR);
        if (!*rel) rel = "/";
        char buf[80]; snprintf(buf, sizeof(buf), "%s", rel);
        lv_label_set_text(bap_lbl_path, buf);
    }

    /* Ligne ".." pour remonter si on n'est pas a la racine */
    if (strcmp(bap_cwd, BADUSB_DIR) != 0) {
        lv_obj_t *up = lv_button_create(bap_list_obj);
        lv_obj_set_size(up, LV_PCT(100), 32);
        lv_obj_set_style_radius(up, 2, 0);
        lv_obj_set_style_bg_opa(up, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(up, lv_color_hex(CY_DIM), 0);
        lv_obj_set_style_border_width(up, 1, 0);
        lv_obj_set_style_border_color(up, lv_color_hex(CY_DIM), 0);
        lv_obj_set_style_shadow_width(up, 0, 0);
        lv_obj_add_event_cb(up, bap_parent_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ul = label(up, LV_SYMBOL_LEFT "  ..", FONT_BODY, CY_TEXT);
        lv_obj_align(ul, LV_ALIGN_LEFT_MID, 8, 0);
    }

    if (bad_entries_n == 0 && strcmp(bap_cwd, BADUSB_DIR) == 0) {
        label(bap_list_obj, "(dossier vide)", FONT_SMALL, CY_DIM);
        return;
    }
    for (int i = 0; i < bad_entries_n; i++) {
        badusb_entry_t *en = &bad_entries[i];
        lv_obj_t *r = lv_button_create(bap_list_obj);
        lv_obj_set_size(r, LV_PCT(100), 32);
        lv_obj_set_style_radius(r, 2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_20, 0);
        uint32_t col = en->is_dir ? CY_AMBER : CY_MAGENTA;
        lv_obj_set_style_bg_color(r, lv_color_hex(col), 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(col), 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_add_event_cb(r, bap_entry_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char txt[96];
        snprintf(txt, sizeof(txt), "%s  %s",
                 en->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                 en->name);
        lv_obj_t *rl = label(r, txt, FONT_BODY, CY_TEXT);
        lv_obj_align(rl, LV_ALIGN_LEFT_MID, 8, 0);
        if (!en->is_dir) {
            lv_obj_t *rr = label(r, "RUN", FONT_SMALL, CY_TEXT);
            lv_obj_align(rr, LV_ALIGN_RIGHT_MID, -8, 0);
        }
    }
}

static int usb_mode_target;
static void usb_mode_switch_done(bool ok, void *user) { (void)ok; (void)user; }
static void usb_mode_yes(void) {
    sys_usb_mode_set_async((usb_mode_t)usb_mode_target, usb_mode_switch_done, NULL);
}
static void usb_mode_btn_ncm_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_mode() == USB_MODE_NCM) return;
    usb_mode_target = USB_MODE_NCM;
    confirm_dialog("Mode RESEAU ?\n(restaure SSH USB)", usb_mode_yes);
}
static void usb_mode_btn_hid_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_mode() == USB_MODE_HID) return;
    usb_mode_target = USB_MODE_HID;
    confirm_dialog("Mode CLAVIER ?\n(coupe SSH USB)", usb_mode_yes);
}
static void usb_mode_btn_storage_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_mode() == USB_MODE_STORAGE) return;
    usb_mode_target = USB_MODE_STORAGE;
    confirm_dialog("Mode STOCKAGE ?\n(expose le dossier badusb\nau PC, coupe SSH USB)", usb_mode_yes);
}
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

    char hdr[96]; snprintf(hdr, sizeof(hdr), "Scanner pour rejoindre %s", settings_hotspot_ssid());
    lv_obj_t *t = label(qr_ov, hdr, FONT_SMALL, CY_CYAN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    /* WIFI:T:WPA;S:<ssid>;P:<pass>;; */
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

/* ----- modal Reglages : node name, SSID/pass hotspot, fuseau ----- */
static lv_obj_t *set_ov, *set_ta_node, *set_ta_ssid, *set_ta_pass, *set_ta_tz;

static void set_ta_focus_e(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

static void set_modal_close(void) {
    if (set_ov) { lv_obj_delete(set_ov); set_ov = NULL; }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void set_save_cb_e(lv_event_t *e) {
    (void)e;
    settings_set_node_name   (lv_textarea_get_text(set_ta_node));
    settings_set_hotspot_ssid(lv_textarea_get_text(set_ta_ssid));
    settings_set_hotspot_pass(lv_textarea_get_text(set_ta_pass));
    const char *tz = lv_textarea_get_text(set_ta_tz);
    settings_set_timezone(tz);
    settings_save();
    sys_set_timezone(tz);
    set_modal_close();
}
static void set_cancel_cb_e(lv_event_t *e) { (void)e; set_modal_close(); }

static lv_obj_t *settings_field(lv_obj_t *parent, const char *key, const char *val, bool pwd) {
    label(parent, key, FONT_SMALL, CY_DIM);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    if (pwd) lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_text(ta, val);
    lv_obj_set_size(ta, LV_PCT(100), 30);
    lv_obj_set_style_bg_color(ta, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(ta, FONT_BODY, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 2, 0);
    lv_obj_add_event_cb(ta, set_ta_focus_e, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, set_ta_focus_e, LV_EVENT_FOCUSED, NULL);
    return ta;
}

static void settings_modal_open_e(lv_event_t *e) {
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

    label(set_ov, "Reglages", FONT_BIG, CY_CYAN);

    set_ta_node = settings_field(set_ov, "nom du noeud",       settings_node_name(),    false);
    set_ta_ssid = settings_field(set_ov, "SSID hotspot",       settings_hotspot_ssid(), false);
    set_ta_pass = settings_field(set_ov, "passphrase hotspot", settings_hotspot_pass(), true);
    set_ta_tz   = settings_field(set_ov, "fuseau horaire",     settings_timezone(),     false);

    lv_obj_t *row = lv_obj_create(set_ov);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, "ANNULER",     CY_DIM,  set_cancel_cb_e);
    small_button(row, "ENREGISTRER", CY_CYAN, set_save_cb_e);
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

    if (sys_lbl_wifi) {
        if (i.wifi_signal >= 0)
            snprintf(b, sizeof(b), "%s  (%d%%)", i.wifi_ssid, i.wifi_signal);
        else
            snprintf(b, sizeof(b), "%s", i.wifi_ssid);
        lv_label_set_text(sys_lbl_wifi, b);
    }

    if (sys_lbl_wifi_radio_state) {
        bool radio = sys_wifi_radio_on();
        lv_label_set_text(sys_lbl_wifi_radio_state, radio ? "radio active" : "radio coupee");
        lv_obj_set_style_text_color(sys_lbl_wifi_radio_state,
            lv_color_hex(radio ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_wifi_radio_btn, radio ? "DESACTIVER" : "ACTIVER");
    }
    if (sys_lbl_bt_state) {
        bool bt = sys_bt_on();
        lv_label_set_text(sys_lbl_bt_state, bt ? "actif" : "coupe");
        lv_obj_set_style_text_color(sys_lbl_bt_state,
            lv_color_hex(bt ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_bt_btn, bt ? "DESACTIVER" : "ACTIVER");
    }
    if (sys_lbl_usb_mode_state) {
        usb_mode_t m = sys_usb_mode();
        const char *txt = (m == USB_MODE_HID) ? "CLAVIER" :
                          (m == USB_MODE_NCM) ? "RESEAU"  : "?";
        lv_label_set_text(sys_lbl_usb_mode_state, txt);
        lv_obj_set_style_text_color(sys_lbl_usb_mode_state,
            lv_color_hex(m == USB_MODE_HID ? CY_MAGENTA : CY_CYAN), 0);
        lv_label_set_text(sys_lbl_usb_mode_btn,
            m == USB_MODE_HID ? "MODE RESEAU" : "MODE CLAVIER");
    }

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
    if (sys_btn_usb_share && sys_btn_usb_client) {
        usb_net_mode_t un = sys_usb_net_mode();
        lv_obj_set_style_bg_opa(sys_btn_usb_share,
            un == USB_NET_SHARED ? LV_OPA_80 : LV_OPA_20, 0);
        lv_obj_set_style_bg_opa(sys_btn_usb_client,
            un == USB_NET_CLIENT ? LV_OPA_80 : LV_OPA_20, 0);
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

    /* BLUETOOTH */
    s = section(col, "BLUETOOTH");
    lv_obj_t *rb = lv_obj_create(s);
    lv_obj_set_size(rb, LV_PCT(100), LV_SIZE_CONTENT);
    flat(rb); lv_obj_set_flex_flow(rb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rb, LV_OBJ_FLAG_SCROLLABLE);
    sys_lbl_bt_state = label(rb, "?", FONT_BODY, CY_DIM);
    lv_obj_t *bbtn = lv_button_create(rb);
    lv_obj_set_size(bbtn, 130, 30);
    lv_obj_set_style_radius(bbtn, 2, 0);
    lv_obj_set_style_bg_opa(bbtn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(bbtn, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(bbtn, 1, 0);
    lv_obj_set_style_border_color(bbtn, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_shadow_width(bbtn, 0, 0);
    lv_obj_add_event_cb(bbtn, bt_toggle_cb, LV_EVENT_CLICKED, NULL);
    sys_lbl_bt_btn = label(bbtn, "?", FONT_SMALL, CY_TEXT);
    lv_obj_center(sys_lbl_bt_btn);

    s = section(col, "USB");
    sys_lbl_usb_state = info_row(s, "etat");
    sys_lbl_usb_ip    = info_row(s, "ip Pi");
    lv_obj_t *usbhint = label(s, "Brancher PC sur le port USB (milieu) du Pi", FONT_SMALL, CY_DIM);
    (void)usbhint;

    /* USB > INTERNET : choisir qui sert le DHCP (Pi ou PC via ICS) */
    label(s, "internet :", FONT_SMALL, CY_DIM);
    lv_obj_t *unrow = lv_obj_create(s);
    lv_obj_set_size(unrow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(unrow); lv_obj_set_flex_flow(unrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(unrow, 6, 0);
    lv_obj_clear_flag(unrow, LV_OBJ_FLAG_SCROLLABLE);
    sys_btn_usb_share  = small_button(unrow, "PARTAGE Pi",  CY_CYAN,  usb_net_share_cb);
    sys_btn_usb_client = small_button(unrow, "CLIENT (ICS)", CY_AMBER, usb_net_client_cb);

    s = section(col, "REGLAGES");
    small_button(s, LV_SYMBOL_SETTINGS "  MODIFIER", CY_CYAN, settings_modal_open_e);
    label(s, "noeud, SSID hotspot, passphrase, fuseau", FONT_SMALL, CY_DIM);

    s = section(col, "APPLICATION");
    small_button(s, LV_SYMBOL_REFRESH "  RELANCER MESHUI", CY_CYAN, restart_app_cb);

    /* MISES A JOUR (git pull + rebuild + restart, depuis github) */
    s = section(col, "MISES A JOUR");
    upd_lbl_state = label(s, "?", FONT_BODY, CY_DIM);
    upd_lbl_hash  = label(s, "-", FONT_SMALL, CY_DIM);
    lv_obj_t *urow = lv_obj_create(s);
    lv_obj_set_size(urow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(urow); lv_obj_set_flex_flow(urow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(urow, 6, 0);
    lv_obj_clear_flag(urow, LV_OBJ_FLAG_SCROLLABLE);
    small_button(urow, LV_SYMBOL_REFRESH "  VERIFIER", CY_CYAN,  upd_check_cb);
    upd_btn_install = small_button(urow, LV_SYMBOL_DOWNLOAD "  INSTALLER", CY_MAGENTA, upd_apply_cb);
    lv_obj_add_flag(upd_btn_install, LV_OBJ_FLAG_HIDDEN);
    /* auto-check au chargement de la page */
    upd_check_cb(NULL);

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
    lv_obj_set_size(wifi_list_ov, LV_PCT(100), 390);
    lv_obj_align(wifi_list_ov, LV_ALIGN_TOP_MID, 0, 70);
    flat(wifi_list_ov);
    lv_obj_set_flex_flow(wifi_list_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list_ov, 4, 0);
    lv_obj_set_scroll_dir(wifi_list_ov, LV_DIR_VER);
    sys_wifi_scan_async(wifi_scan_done, NULL);
}

/* ---------------------------------------------------------------- routage */
static void build_home(void);
static void build_hotspot_app(void);
static void build_badusb_app(void);

static void show_tab(int app) {
    if (sys_refresh_timer) { lv_timer_delete(sys_refresh_timer); sys_refresh_timer = NULL; }
    /* null tous les pointeurs vers des labels qu'on s'apprete a liberer */
    sys_lbl_host = NULL;
    sys_lbl_wifi = NULL;
    sys_lbl_wifi_radio_state = NULL; sys_lbl_wifi_radio_btn = NULL;
    sys_lbl_hot_state = NULL; sys_lbl_hot_btn = NULL;
    sys_lbl_usb_mode_state = NULL; sys_lbl_usb_mode_btn = NULL;
    sys_lbl_usb_state = NULL; sys_lbl_usb_ip = NULL;
    sys_lbl_bt_state = NULL; sys_lbl_bt_btn = NULL;
    sys_lbl_ssh_state = NULL; sys_lbl_ssh_btn = NULL;
    hap_lbl_state = NULL; hap_lbl_btn = NULL;
    bap_lbl_state = NULL; bap_lbl_btn = NULL;
    bap_btn_ncm = NULL; bap_btn_hid = NULL; bap_btn_storage = NULL;
    bap_list_obj = NULL; bap_lbl_path = NULL;
    upd_lbl_state = NULL; upd_lbl_hash = NULL; upd_btn_install = NULL;
    sys_btn_usb_share = NULL; sys_btn_usb_client = NULL;
    sys_log_ta = NULL;
    sys_bl_slider = NULL; sys_bl_lbl = NULL;
    compose_ta   = NULL;
    cur_tab = app;
    lv_obj_clean(content);

    /* bouton retour + repositionnement du nom selon que le retour est visible */
    if (tb_back && tb_name) {
        if (app == APP_HOME) {
            lv_obj_add_flag(tb_back, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(tb_name, LV_ALIGN_LEFT_MID, 0, 0);
        } else {
            lv_obj_clear_flag(tb_back, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(tb_name, LV_ALIGN_LEFT_MID, 40, 0);
        }
    }

    switch (app) {
        case APP_HOME:    build_home();          break;
        case APP_CHAT:    build_chat();          break;
        case APP_NODES:   build_nodes();         break;
        case APP_SYS:     build_sys();           break;
        case APP_HOTSPOT: build_hotspot_app();   break;
        case APP_BADUSB:  build_badusb_app();    break;
        case APP_WIFI:
            /* WIFI = modal flottant ; on reste conceptuellement sur HOME */
            cur_tab = APP_HOME;
            build_home();
            if (tb_back) {
                lv_obj_add_flag(tb_back, LV_OBJ_FLAG_HIDDEN);
                if (tb_name) lv_obj_align(tb_name, LV_ALIGN_LEFT_MID, 0, 0);
            }
            wifi_modal_open();
            break;
        default: build_home(); break;
    }
}

/* ---------------------------------------------------------------- HOME hub */
typedef struct {
    int          app_id;
    const char  *title;
    const char  *icon;
    uint32_t     color;
} app_card_t;

static const app_card_t HOME_APPS[] = {
    { APP_CHAT,    "MESSAGES", LV_SYMBOL_ENVELOPE, CY_CYAN    },
    { APP_NODES,   "NODES",    LV_SYMBOL_GPS,      CY_CYAN    },
    { APP_WIFI,    "WIFI",     LV_SYMBOL_WIFI,     CY_CYAN    },
    { APP_HOTSPOT, "HOTSPOT",  LV_SYMBOL_IMAGE,    CY_MAGENTA },
    { APP_BADUSB,  "BAD USB",  LV_SYMBOL_USB,      CY_MAGENTA },
    { APP_SYS,     "SYSTEME",  LV_SYMBOL_SETTINGS, CY_AMBER   },
};

static void home_card_cb(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    show_tab(id);
}

static void build_home(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(col, 6, 0);

    /* grille 2 col x 3 lignes — pleine hauteur disponible */
    lv_obj_t *grid = lv_obj_create(col);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    flat(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int n = (int)(sizeof(HOME_APPS) / sizeof(HOME_APPS[0]));
    for (int i = 0; i < n; i++) {
        const app_card_t *a = &HOME_APPS[i];
        lv_obj_t *c = lv_button_create(grid);
        lv_obj_set_size(c, LV_PCT(48), 130);
        lv_obj_set_style_radius(c, 2, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(CY_PANEL), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(a->color), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_shadow_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 4, 0);
        lv_obj_add_event_cb(c, home_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a->app_id);

        lv_obj_t *ic = label(c, a->icon, &lv_font_montserrat_28, a->color);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -10);
        lv_obj_t *t = label(c, a->title, FONT_MONO, CY_TEXT);
        lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
}

/* ---------------------------------------------------------------- app HOTSPOT */
static void hap_qr_cb(lv_event_t *e) { qr_open_cb(e); }
static void hap_refresh(lv_timer_t *t) {
    (void)t;
    if (!hap_lbl_state) return;
    bool on = sys_hotspot_active();
    lv_label_set_text(hap_lbl_state, on ? "actif" : "inactif");
    lv_obj_set_style_text_color(hap_lbl_state, lv_color_hex(on ? CY_GREEN : CY_DIM), 0);
    lv_label_set_text(hap_lbl_btn, on ? "DESACTIVER" : "ACTIVER");
}
static void build_hotspot_app(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(col, "HOTSPOT", FONT_BIG, CY_MAGENTA);
    (void)title;

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

    /* creds */
    char credbuf[160]; snprintf(credbuf, sizeof(credbuf), "SSID  %s\npass  %s",
                                settings_hotspot_ssid(), settings_hotspot_pass());
    label(col, credbuf, FONT_BODY, CY_TEXT);

    /* small_button utilise flex_grow=1 (utile en ROW), ici en COLUMN il s'etire :
     * on retire le grow et on borne la hauteur */
    lv_obj_t *qrb = small_button(col, LV_SYMBOL_IMAGE "  QR CODE WIFI", CY_MAGENTA, hap_qr_cb);
    lv_obj_set_flex_grow(qrb, 0);
    lv_obj_set_height(qrb, 38);
    lv_obj_set_width(qrb, LV_PCT(100));

    /* refresh */
    hap_refresh(NULL);
    sys_refresh_timer = lv_timer_create(hap_refresh, 3000, NULL);
}

/* ---------------------------------------------------------------- app BAD USB */
static void bap_refresh(lv_timer_t *t) {
    (void)t;
    if (!bap_lbl_state) return;
    usb_mode_t m = sys_usb_mode();
    const char *s = (m == USB_MODE_HID)     ? "CLAVIER actif"
                  : (m == USB_MODE_STORAGE) ? "STOCKAGE actif"
                  : (m == USB_MODE_NCM)     ? "RESEAU actif"  : "?";
    uint32_t col = (m == USB_MODE_HID)     ? CY_MAGENTA
                 : (m == USB_MODE_STORAGE) ? CY_AMBER
                 : (m == USB_MODE_NCM)     ? CY_CYAN  : CY_DIM;
    lv_label_set_text(bap_lbl_state, s);
    lv_obj_set_style_text_color(bap_lbl_state, lv_color_hex(col), 0);

    if (bap_btn_ncm)
        lv_obj_set_style_bg_opa(bap_btn_ncm,     m == USB_MODE_NCM     ? LV_OPA_80 : LV_OPA_20, 0);
    if (bap_btn_hid)
        lv_obj_set_style_bg_opa(bap_btn_hid,     m == USB_MODE_HID     ? LV_OPA_80 : LV_OPA_20, 0);
    if (bap_btn_storage)
        lv_obj_set_style_bg_opa(bap_btn_storage, m == USB_MODE_STORAGE ? LV_OPA_80 : LV_OPA_20, 0);
}

static void build_badusb_app(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    label(col, "BAD USB", FONT_BIG, CY_MAGENTA);

    bap_lbl_state = label(col, "?", FONT_BODY, CY_DIM);

    /* sélecteur 3 modes USB */
    lv_obj_t *mr = lv_obj_create(col);
    lv_obj_set_size(mr, LV_PCT(100), LV_SIZE_CONTENT);
    flat(mr); lv_obj_set_flex_flow(mr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mr, 4, 0);
    lv_obj_clear_flag(mr, LV_OBJ_FLAG_SCROLLABLE);
    bap_btn_ncm     = small_button(mr, "RESEAU",   CY_CYAN,    usb_mode_btn_ncm_cb);
    bap_btn_hid     = small_button(mr, "CLAVIER",  CY_MAGENTA, usb_mode_btn_hid_cb);
    bap_btn_storage = small_button(mr, "STOCKAGE", CY_AMBER,   usb_mode_btn_storage_cb);

    /* explorateur de l'arbo BADUSB */
    label(col, "scripts", FONT_SMALL, CY_DIM);
    bap_lbl_path = label(col, "/", FONT_MONO, CY_AMBER);
    lv_obj_set_width(bap_lbl_path, LV_PCT(100));
    lv_label_set_long_mode(bap_lbl_path, LV_LABEL_LONG_DOT);

    bap_list_obj = lv_obj_create(col);
    lv_obj_set_size(bap_list_obj, LV_PCT(100), LV_PCT(100));
    flat(bap_list_obj);
    lv_obj_set_flex_flow(bap_list_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bap_list_obj, 4, 0);
    lv_obj_set_scroll_dir(bap_list_obj, LV_DIR_VER);

    /* on revient toujours a la racine quand on rouvre l'app */
    strcpy(bap_cwd, BADUSB_DIR);
    bap_refresh_list();

    bap_refresh(NULL);
    sys_refresh_timer = lv_timer_create(bap_refresh, 3000, NULL);
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
    lv_obj_set_pos(splash_ov, 0, 0);
    lv_obj_set_size(splash_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(splash_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_ov, 0, 0);
    lv_obj_set_style_radius(splash_ov, 0, 0);
    lv_obj_set_style_pad_all(splash_ov, 0, 0);
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

    char subbuf[80]; snprintf(subbuf, sizeof(subbuf), "node // %s", settings_node_name());
    lv_obj_t *node = label(splash_ov, subbuf, FONT_SMALL, CY_DIM);
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

    /* clavier overlay sur le top layer : visible au-dessus de tout (chat + modaux) */
    kb = lv_keyboard_create(lv_layer_top());

    /* AZERTY minuscules */
    static const char *kb_map_lower[] = {
        "1","2","3","4","5","6","7","8","9","0","\n",
        "a","z","e","r","t","y","u","i","o","p","\n",
        "q","s","d","f","g","h","j","k","l","m","\n",
        "ABC","w","x","c","v","b","n","'","-",LV_SYMBOL_BACKSPACE,"\n",
        "1#"," ",".",LV_SYMBOL_OK,""
    };
    /* AZERTY majuscules */
    static const char *kb_map_upper[] = {
        "1","2","3","4","5","6","7","8","9","0","\n",
        "A","Z","E","R","T","Y","U","I","O","P","\n",
        "Q","S","D","F","G","H","J","K","L","M","\n",
        "abc","W","X","C","V","B","N","'","-",LV_SYMBOL_BACKSPACE,"\n",
        "1#"," ",".",LV_SYMBOL_OK,""
    };
    static const lv_buttonmatrix_ctrl_t kb_ctrl[] = {
        1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,
        2,6,1,2
    };
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lower, kb_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_upper, kb_ctrl);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(55));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);

    show_tab(APP_HOME);
}
