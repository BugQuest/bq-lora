#include "ui.h"
#include "ui_common.h"
#include "ui_audio.h"
#include "ui_node_views.h"
#include "ui_chanmgr.h"
#include "ui_badusb.h"
#include "mesh.h"
#include "calib.h"
#include "sys.h"
#include "settings.h"
#include "bme280.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>

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
    APP_BT      = 7,
    APP_ABOUT   = 8,
    APP_CAMERA  = 9,
    APP_GALLERY = 10,
};

lv_obj_t *content;                 /* zone centrale, reconstruite par app -- partage */
lv_obj_t *kb;                      /* clavier virtuel (overlay, masqué) -- partage */
static uint8_t   cur_chan = 0;     /* canal courant dans la vue chat */
static int       cur_tab  = APP_HOME;

static void show_tab(int app);
/* Les helpers UI partages (kb, flat, panel, label, small_button, confirm_dialog)
 * sont declares dans ui_common.h pour etre utilises par les modules separes
 * (ui_audio.c, etc.) -- pas besoin de forward decls locales ici. */
static void bt_modal_open(void);
/* gestionnaire de canaux : deplace dans ui_chanmgr.c (point d'entree
 * ui_chanmgr_open_e, rafraichi via ui_chanmgr_refresh_if_open). */

/* ---------------------------------------------------------------- helpers */
void flat(lv_obj_t *o) {
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
}

void panel(lv_obj_t *o, uint32_t border) {
    lv_obj_set_style_bg_color(o, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_set_style_pad_all(o, 6, 0);
}

lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* ---------------------------------------------------------------- barre haute */
static lv_obj_t *tb_clock, *tb_name, *tb_back;
/* barre d'état verticale (droite) : icônes système + LoRa */
static lv_obj_t *sb_usb, *sb_wifi, *sb_link, *sb_nodes, *sb_util, *sb_batt;
/* ----- Warnings (auto-cachees quand pas d'alerte) -----
 * - throttle/sous-tension : magenta (en cours) ou ambre (deja eu)
 * - CPU temp >= 70C : ambre, >= 80C : magenta, avec valeur en C
 * - disque >= 85% : ambre, >= 95% : magenta, avec %
 * - gadget USB en mode non-RESEAU : KEYBOARD (HID, magenta) ou DRIVE (storage, ambre)
 * Le but est de voir immediatement les pepins systeme sans aller dans l'onglet. */
static lv_obj_t *sb_warn_thr_cell, *sb_warn_thr_ic;
static lv_obj_t *sb_warn_temp_cell, *sb_warn_temp_ic, *sb_warn_temp_val;
static lv_obj_t *sb_warn_disk_cell, *sb_warn_disk_ic, *sb_warn_disk_val;
static lv_obj_t *sb_warn_usbmode_cell, *sb_warn_usbmode_ic;
static lv_timer_t *tb_timer;

/* Badge non-lus sur la carte MESSAGES du hub. msg_seen = compteur "lu" (remis
 * a niveau a l'ouverture du CHAT) ; non-lus = mesh_rx_msg_total() - msg_seen. */
static lv_obj_t *home_msg_card, *home_msg_badge;
static unsigned  msg_seen;
/* Idem par canal : badge sur les chips. msg_seen_ch[ch] = "lu" pour ce canal. */
#define UI_MAX_CHANS 8
static unsigned  msg_seen_ch[UI_MAX_CHANS];
static void      update_msg_badge(void);

static void tb_back_cb(lv_event_t *e) { (void)e; show_tab(APP_HOME); }

static int read_int_file(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return 0;
    int v = 0; fscanf(f, "%d", &v); fclose(f); return v;
}

/* Le carrier du gadget (et l'etat UDC) reste a "configured/1" meme cable
 * debranche : le Pi Zero n'a pas de detection VBUS, on ne peut pas s'y fier.
 * Indicateur fiable = un client est-il REELLEMENT present :
 *   1) un bail DHCP NON EXPIRE sur usb0, ou
 *   2) un voisin ARP dans un etat VIVANT (pas STALE/FAILED : ceux-ci
 *      persistent longtemps apres debranchement et donnaient un faux positif). */
static bool usb_client_connected(void) {
    char line[256];
    time_t now = time(NULL);

    /* 1) bail DHCP : 1er champ = epoch d'expiration ; ignore si deja expire. */
    FILE *f = fopen("/var/lib/NetworkManager/dnsmasq-usb0.leases", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            long exp = strtol(line, NULL, 10);
            if (exp > (long)now) { fclose(f); return true; }
        }
        fclose(f);
    }

    /* 2) voisin ARP dans un etat actif uniquement. */
    FILE *p = popen("ip neigh show dev usb0 2>/dev/null", "r");
    if (p) {
        while (fgets(line, sizeof(line), p)) {
            if (strstr(line, "REACHABLE") || strstr(line, "DELAY") ||
                strstr(line, "PROBE")     || strstr(line, "PERMANENT")) {
                pclose(p);
                return true;
            }
        }
        pclose(p);
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
}

/* Met à jour la barre d'état verticale (système + LoRa). */
static void statusbar_refresh(lv_timer_t *t) {
    (void)t;
    if (!sb_usb) return;

    /* --- système --- */
    bool usb_up = usb_client_connected();
    lv_obj_set_style_text_color(sb_usb, lv_color_hex(usb_up ? CY_CYAN : CY_BORDER), 0);

    bool wifi_up = read_int_file("/sys/class/net/wlan0/carrier") == 1;
    bool ap = sys_hotspot_active();
    uint32_t wcol = ap ? CY_MAGENTA : (wifi_up ? CY_CYAN : CY_BORDER);
    lv_obj_set_style_text_color(sb_wifi, lv_color_hex(wcol), 0);

    /* --- LoRa / mesh --- */
    bool link = mesh_connected();
    const mesh_self_t *s = mesh_self();

    /* lien vers meshtasticd : vert si établi, sinon rouge atténué */
    lv_obj_set_style_text_color(sb_link, lv_color_hex(link ? CY_GREEN : CY_MAGENTA), 0);

    /* nombre de nœuds vus */
    char nb[8]; snprintf(nb, sizeof(nb), "%d", s->nodes);
    lv_label_set_text(sb_nodes, nb);
    lv_obj_set_style_text_color(sb_nodes, lv_color_hex(link ? CY_TEXT : CY_DIM), 0);

    /* utilisation canal (air time) : ambre si chargé */
    int util = (int)(s->chan_util + 0.5f);
    char ub[8]; snprintf(ub, sizeof(ub), "%d%%", util);
    lv_label_set_text(sb_util, ub);
    lv_obj_set_style_text_color(sb_util, lv_color_hex(util >= 40 ? CY_AMBER : CY_DIM), 0);

    /* batterie du nœud (0 = non alimenté par batterie -> tiret) */
    if (s->batt > 0 && s->batt <= 100) {
        const char *sym = s->batt > 80 ? LV_SYMBOL_BATTERY_FULL :
                          s->batt > 55 ? LV_SYMBOL_BATTERY_3 :
                          s->batt > 30 ? LV_SYMBOL_BATTERY_2 :
                          s->batt > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text(sb_batt, sym);
        lv_obj_set_style_text_color(sb_batt, lv_color_hex(s->batt > 20 ? CY_GREEN : CY_AMBER), 0);
    } else {
        lv_label_set_text(sb_batt, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(sb_batt, lv_color_hex(CY_DIM), 0);
    }

    /* --- alertes systeme (cachees tant que tout va bien) --- */
    sys_warn_t w; sys_warn_get(&w);

    /* throttling : magenta = en cours, ambre = passe seulement, cache sinon */
    if (sb_warn_thr_cell) {
        if (w.throttled_now || w.throttled_ever) {
            lv_obj_clear_flag(sb_warn_thr_cell, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(sb_warn_thr_ic,
                lv_color_hex(w.throttled_now ? CY_MAGENTA : CY_AMBER), 0);
        } else {
            lv_obj_add_flag(sb_warn_thr_cell, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* CPU temp : seuil 70C (ambre) / 80C (magenta) */
    if (sb_warn_temp_cell) {
        int tc = (int)(w.cpu_temp_c + 0.5f);
        if (tc >= 70) {
            char tb[8]; snprintf(tb, sizeof(tb), "%dC", tc);
            lv_label_set_text(sb_warn_temp_val, tb);
            lv_obj_set_style_text_color(sb_warn_temp_ic,
                lv_color_hex(tc >= 80 ? CY_MAGENTA : CY_AMBER), 0);
            lv_obj_clear_flag(sb_warn_temp_cell, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sb_warn_temp_cell, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* disque / : seuil 85% (ambre) / 95% (magenta) */
    if (sb_warn_disk_cell) {
        if (w.disk_used_pct >= 85) {
            char db[8]; snprintf(db, sizeof(db), "%d%%", w.disk_used_pct);
            lv_label_set_text(sb_warn_disk_val, db);
            lv_obj_set_style_text_color(sb_warn_disk_ic,
                lv_color_hex(w.disk_used_pct >= 95 ? CY_MAGENTA : CY_AMBER), 0);
            lv_obj_clear_flag(sb_warn_disk_cell, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sb_warn_disk_cell, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* gadget USB en mode HID / STORAGE : signaler la perte de SSH-USB */
    if (sb_warn_usbmode_cell) {
        usb_mode_t um = sys_usb_mode();
        if (um == USB_MODE_HID) {
            lv_label_set_text(sb_warn_usbmode_ic, LV_SYMBOL_KEYBOARD);
            lv_obj_set_style_text_color(sb_warn_usbmode_ic, lv_color_hex(CY_MAGENTA), 0);
            lv_obj_clear_flag(sb_warn_usbmode_cell, LV_OBJ_FLAG_HIDDEN);
        } else if (um == USB_MODE_STORAGE) {
            lv_label_set_text(sb_warn_usbmode_ic, LV_SYMBOL_DRIVE);
            lv_obj_set_style_text_color(sb_warn_usbmode_ic, lv_color_hex(CY_AMBER), 0);
            lv_obj_clear_flag(sb_warn_usbmode_cell, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sb_warn_usbmode_cell, LV_OBJ_FLAG_HIDDEN);
        }
    }
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

    /* horloge à droite (les icônes d'état vivent désormais dans la barre verticale) */
    tb_clock = label(bar, "--:--", FONT_MONO, CY_TEXT);
    lv_obj_align(tb_clock, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* Une entrée de la barre d'état : icône + valeur optionnelle dessous.
   Renvoie le label icône ; *val_out (si non NULL) reçoit le label valeur. */
static lv_obj_t *sb_item(lv_obj_t *parent, const char *icon, const char *val,
                         uint32_t color, lv_obj_t **val_out) {
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, LV_PCT(100), LV_SIZE_CONTENT);
    flat(cell);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 1, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = label(cell, icon, FONT_BODY, color);
    if (val_out) {
        lv_obj_t *v = label(cell, val, FONT_SMALL, CY_DIM);
        *val_out = v;
    }
    return ic;
}

/* Barre d'état verticale fixée à droite : icônes système + indicateurs LoRa. */
static void build_statusbar(lv_obj_t *parent) {
    lv_obj_t *sb = lv_obj_create(parent);
    lv_obj_set_size(sb, 42, LV_PCT(100));
    lv_obj_set_style_bg_color(sb, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(sb, 0, 0);
    lv_obj_set_style_pad_top(sb, 8, 0);
    lv_obj_set_style_pad_bottom(sb, 8, 0);
    lv_obj_set_style_pad_left(sb, 0, 0);
    lv_obj_set_style_pad_right(sb, 0, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sb, 10, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    /* --- système --- */
    sb_usb  = sb_item(sb, LV_SYMBOL_USB,  NULL, CY_BORDER, NULL);
    sb_wifi = sb_item(sb, LV_SYMBOL_WIFI, NULL, CY_BORDER, NULL);

    /* séparateur */
    lv_obj_t *sep = lv_obj_create(sb);
    lv_obj_set_size(sep, 24, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    /* --- LoRa / mesh --- */
    sb_link = sb_item(sb, LV_SYMBOL_GPS, NULL, CY_MAGENTA, NULL);    /* lien meshtasticd (couleur) */
    sb_item(sb, LV_SYMBOL_LIST, "0",  CY_DIM, &sb_nodes);            /* nb nœuds (valeur) */
    sb_item(sb, LV_SYMBOL_LOOP, "0%", CY_DIM, &sb_util);             /* util canal (valeur) */
    sb_batt = sb_item(sb, LV_SYMBOL_CHARGE, NULL, CY_DIM, NULL);     /* batterie nœud (symbole) */

    /* --- alertes systeme (cachees tant que tout va bien) --- */
    sb_warn_thr_ic       = sb_item(sb, LV_SYMBOL_WARNING, NULL, CY_MAGENTA, NULL);
    sb_warn_thr_cell     = lv_obj_get_parent(sb_warn_thr_ic);
    lv_obj_add_flag(sb_warn_thr_cell, LV_OBJ_FLAG_HIDDEN);

    sb_warn_temp_ic      = sb_item(sb, LV_SYMBOL_WARNING, "", CY_AMBER, &sb_warn_temp_val);
    sb_warn_temp_cell    = lv_obj_get_parent(sb_warn_temp_ic);
    lv_obj_add_flag(sb_warn_temp_cell, LV_OBJ_FLAG_HIDDEN);

    sb_warn_disk_ic      = sb_item(sb, LV_SYMBOL_SD_CARD, "", CY_AMBER, &sb_warn_disk_val);
    sb_warn_disk_cell    = lv_obj_get_parent(sb_warn_disk_ic);
    lv_obj_add_flag(sb_warn_disk_cell, LV_OBJ_FLAG_HIDDEN);

    sb_warn_usbmode_ic   = sb_item(sb, LV_SYMBOL_KEYBOARD, NULL, CY_MAGENTA, NULL);
    sb_warn_usbmode_cell = lv_obj_get_parent(sb_warn_usbmode_ic);
    lv_obj_add_flag(sb_warn_usbmode_cell, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------- vue CHAT */
static lv_obj_t *msg_list;
static lv_obj_t *compose_ta;
static lv_obj_t *compose_bar;   /* barre de saisie : remontée au-dessus du clavier */

static void add_bubble(lv_obj_t *parent, const mesh_message_t *m) {
    const mesh_channel_t *c = mesh_channel(m->ch);
    bool ch_enc = c ? c->enc : false;   /* canal absent (MESH inactif) -> non chiffré */

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
    lv_obj_t *h = label(b, head, FONT_SMALL, ch_enc ? CY_MAGENTA : CY_CYAN);
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
    if (cur_chan < UI_MAX_CHANS)
        msg_seen_ch[cur_chan] = mesh_rx_msg_count(cur_chan);  /* lu en arrivant */
    show_tab(APP_CHAT);
}

static void send_cb(lv_event_t *e) {
    (void)e;
    const char *txt = lv_textarea_get_text(compose_ta);
    if (!txt || !txt[0]) return;
    /* Throttle TX : refuse l'envoi si l'air-time depasse 10% (regle ETSI EU868
     * 1% duty cycle, on prend 10% comme garde-fou applicatif). */
    const mesh_self_t *sf = mesh_self();
    if (sf && sf->air_tx > 10.0f) {
        confirm_dialog(tr(STR_TX_THROTTLED), NULL);
        return;
    }
    mesh_send(cur_chan, txt);
    lv_textarea_set_text(compose_ta, "");
    rebuild_messages();
}

static void ta_cb(lv_event_t *e) {
    (void)e;
    lv_keyboard_set_textarea(kb, compose_ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
    /* remonte la barre de saisie au-dessus du clavier pour voir ce qu'on tape */
    if (compose_bar) {
        lv_obj_update_layout(kb);
        lv_obj_set_parent(compose_bar, lv_layer_top());
        lv_obj_set_width(compose_bar, LV_PCT(100));
        lv_obj_align(compose_bar, LV_ALIGN_BOTTOM_MID, 0, -lv_obj_get_height(kb));
        lv_obj_move_foreground(compose_bar);
    }
}

/* Replace la barre de saisie dans le flux du chat (sous la liste). */
static void compose_bar_restore(void) {
    if (compose_bar && cur_tab == APP_CHAT) {
        lv_obj_set_parent(compose_bar, content);
        lv_obj_set_width(compose_bar, LV_PCT(100));
    }
}

static void kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    /* valider n'envoie un message que si on est bien dans la zone compose du chat */
    if (code == LV_EVENT_READY && cur_tab == APP_CHAT && compose_ta &&
        lv_keyboard_get_textarea(kb) == compose_ta) {
        send_cb(e);
    }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    compose_bar_restore();
}

static void build_chat(void) {
    msg_seen = mesh_rx_msg_total();   /* ouverture du CHAT = tout marque comme lu */
    if (cur_chan < UI_MAX_CHANS)
        msg_seen_ch[cur_chan] = mesh_rx_msg_count(cur_chan);
    /* rangée des canaux */
    lv_obj_t *chans = lv_obj_create(content);
    lv_obj_set_size(chans, LV_PCT(100), 38);
    flat(chans);
    lv_obj_set_flex_flow(chans, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chans, 4, 0);
    lv_obj_set_style_pad_all(chans, 3, 0);
    lv_obj_clear_flag(chans, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < mesh_channel_count(); i++) {
        const mesh_channel_t *c = mesh_channel(i);
        lv_obj_t *chip = lv_button_create(chans);
        lv_obj_set_height(chip, 28);
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
        /* badge non-lus : petit cercle magenta en coin avec le compte (9+) */
        unsigned unread = 0;
        if (i < UI_MAX_CHANS) {
            unsigned tot = mesh_rx_msg_count((uint8_t)i);
            if (tot > msg_seen_ch[i]) unread = tot - msg_seen_ch[i];
        }
        if (unread > 0) {
            lv_obj_t *b = lv_label_create(chip);
            char bb[8]; snprintf(bb, sizeof(bb), "%u", (unsigned)(unread > 9 ? 9 : unread));
            if (unread > 9) strcat(bb, "+");
            lv_label_set_text(b, bb);
            lv_obj_set_style_text_font(b, FONT_SMALL, 0);
            lv_obj_set_style_text_color(b, lv_color_hex(CY_TEXT), 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(CY_MAGENTA), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_hor(b, 4, 0);
            lv_obj_set_style_pad_ver(b, 0, 0);
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 2, -3);
        }
    }

    /* bouton de gestion des canaux (⚙) en bout de rangée — plus large pour
     * etre facile a viser au doigt sur un ecran resistif */
    lv_obj_t *mgr = lv_button_create(chans);
    lv_obj_set_size(mgr, 48, 30);
    lv_obj_set_style_radius(mgr, 2, 0);
    lv_obj_set_style_shadow_width(mgr, 0, 0);
    lv_obj_set_style_bg_opa(mgr, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(mgr, lv_color_hex(CY_AMBER), 0);
    lv_obj_set_style_border_width(mgr, 1, 0);
    lv_obj_set_style_border_color(mgr, lv_color_hex(CY_AMBER), 0);
    lv_obj_add_event_cb(mgr, ui_chanmgr_open_e, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = label(mgr, LV_SYMBOL_SETTINGS, FONT_BODY, CY_AMBER);
    lv_obj_center(ml);

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
    compose_bar = bar;
    lv_obj_set_size(bar, LV_PCT(100), 34);
    flat(bar);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    compose_ta = lv_textarea_create(bar);
    lv_textarea_set_one_line(compose_ta, true);
    lv_textarea_set_placeholder_text(compose_ta, tr(STR_CHAT_PLACEHOLDER));
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
/* Maj incrémentale : on garde une ligne LVGL par nœud (clé = num) et on met à
 * jour les libellés en place au lieu de tout recréer -> pas de saut de scroll.
 * Tri commutable (vu récemment / meilleur SNR). */
#define NODE_ROW_MAX 200
typedef struct {
    uint32_t  num;
    lv_obj_t *row;
    lv_obj_t *name_lbl;
    lv_obj_t *right_lbl;   /* id court ou badge VOUS */
    lv_obj_t *meta_lbl;    /* ligne SNR/RSSI/batt/hop/vu */
    bool      self;        /* dernier état (évite de re-styler la bordure) */
} node_row_t;

static node_row_t s_nrows[NODE_ROW_MAX];
static int        s_nrow_count;
static lv_obj_t  *nodes_list;
static int        nodes_sort;          /* 0 = vu récemment, 1 = meilleur SNR */
static lv_obj_t  *nodes_sort_lbl;
static lv_obj_t  *nodes_radio_lbl;     /* région / preset / TX / sauts */

static node_row_t *node_row_find(uint32_t num) {
    for (int i = 0; i < s_nrow_count; i++)
        if (s_nrows[i].num == num) return &s_nrows[i];
    return NULL;
}

/* Modal "details du node" deplace dans ui_node_views.c.
 * Le click handler ci-dessous est conserve ici car il manipule l'objet row. */
static void node_row_click_e(lv_event_t *e) {
    uint32_t num = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    node_detail_open(num);
}

static node_row_t *node_row_create(uint32_t num) {
    if (s_nrow_count >= NODE_ROW_MAX || !nodes_list) return NULL;
    node_row_t *r = &s_nrows[s_nrow_count++];
    r->num = num;
    r->self = false;
    r->row = lv_obj_create(nodes_list);
    lv_obj_set_size(r->row, LV_PCT(100), LV_SIZE_CONTENT);
    panel(r->row, CY_BORDER);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->row, node_row_click_e, LV_EVENT_CLICKED, (void *)(uintptr_t)num);
    r->name_lbl = label(r->row, "", FONT_BODY, CY_TEXT);
    lv_obj_align(r->name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    r->right_lbl = label(r->row, "", FONT_SMALL, CY_DIM);
    lv_obj_align(r->right_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    r->meta_lbl = label(r->row, "", FONT_SMALL, CY_DIM);
    lv_obj_align(r->meta_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_pad_top(r->meta_lbl, 18, 0);
    return r;
}

static void node_row_update(node_row_t *r, const mesh_node_t *n) {
    if (r->self != n->self) {
        lv_obj_set_style_border_color(r->row, lv_color_hex(n->self ? CY_CYAN : CY_BORDER), 0);
        lv_obj_set_style_text_color(r->name_lbl, lv_color_hex(n->self ? CY_CYAN : CY_TEXT), 0);
        r->self = n->self;
    }
    lv_label_set_text(r->name_lbl, n->name);
    if (n->self) {
        {
            char yb[24]; snprintf(yb, sizeof(yb), LV_SYMBOL_HOME "%s", tr(STR_YOU_BADGE));
            lv_label_set_text(r->right_lbl, yb);
        }
        lv_obj_set_style_text_color(r->right_lbl, lv_color_hex(CY_CYAN), 0);
    } else {
        lv_label_set_text(r->right_lbl, n->id);
        lv_obj_set_style_text_color(r->right_lbl, lv_color_hex(CY_DIM), 0);
    }
    char best[12];
    if (n->best_snr == -128) snprintf(best, sizeof(best), "-");
    else                     snprintf(best, sizeof(best), "%d", n->best_snr);
    char stat[128];
    snprintf(stat, sizeof(stat), tr(STR_FMT_NODE_META),
             n->snr, best, n->rssi, LV_SYMBOL_CHARGE, n->batt, n->hops, n->last);
    lv_label_set_text(r->meta_lbl, stat);
}

static int node_cmp(const void *a, const void *b) {
    const mesh_node_t *na = *(const mesh_node_t * const *)a;
    const mesh_node_t *nb = *(const mesh_node_t * const *)b;
    if (na->self != nb->self) return na->self ? -1 : 1;     /* soi toujours en tête */
    if (nodes_sort == 1) {                                  /* meilleur SNR d'abord */
        if (na->snr != nb->snr) return nb->snr - na->snr;
    }
    /* défaut / égalité SNR : vu le plus récemment d'abord */
    if (na->last_heard != nb->last_heard)
        return (nb->last_heard > na->last_heard) ? 1 : -1;
    return 0;
}

/* Synchronise la liste avec l'état backend (création/maj/réordonnancement). */
static void nodes_sync(void) {
    if (!nodes_list) return;

    /* ligne radio : conditionne directement la portée (région/preset/TX/sauts) */
    if (nodes_radio_lbl) {
        const mesh_self_t *sf = mesh_self();
        char tx[16];
        if (sf->tx_power > 0) snprintf(tx, sizeof(tx), "%ddBm", sf->tx_power);
        else                  snprintf(tx, sizeof(tx), "%s", tr(STR_TX_AUTO));
        char rb[128];
        snprintf(rb, sizeof(rb), LV_SYMBOL_GPS "%s",
                 ""); /* prefixe symbole + ligne i18n */
        int off = (int)strlen(rb);
        snprintf(rb + off, sizeof(rb) - off, tr(STR_FMT_RADIO_LINE),
                 sf->region, sf->preset, tx, sf->hop_limit);
        lv_label_set_text(nodes_radio_lbl, rb);
    }

    static const mesh_node_t *arr[NODE_ROW_MAX];
    int n = mesh_node_count();
    if (n > NODE_ROW_MAX) n = NODE_ROW_MAX;
    for (int i = 0; i < n; i++) arr[i] = mesh_node(i);
    qsort(arr, n, sizeof(arr[0]), node_cmp);

    for (int i = 0; i < n; i++) {
        const mesh_node_t *nd = arr[i];
        node_row_t *r = node_row_find(nd->num);
        if (!r) r = node_row_create(nd->num);
        if (!r) continue;
        node_row_update(r, nd);
        lv_obj_move_to_index(r->row, i);     /* applique l'ordre du tri */
    }
}

static void nodes_sort_cb(lv_event_t *e) {
    (void)e;
    nodes_sort = !nodes_sort;
    if (nodes_sort_lbl) {
        char b[40]; snprintf(b, sizeof(b), LV_SYMBOL_GPS "%s",
                             tr(nodes_sort ? STR_SORT_SNR : STR_SORT_RECENT));
        lv_label_set_text(nodes_sort_lbl, b);
    }
    nodes_sync();
}

/* Modal "Arbre" deplace dans ui_node_views.c (callback ui_nodes_tree_open_e). */

static void build_nodes(void) {
    s_nrow_count = 0;        /* anciennes lignes détruites par lv_obj_clean(content) */
    nodes_list = NULL;
    nodes_radio_lbl = NULL;

    /* en-tête : bouton de bascule du tri + résumé radio (portée) */
    lv_obj_t *hdr = lv_obj_create(content);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    flat(hdr);
    lv_obj_set_style_pad_all(hdr, 5, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr, 4, 0);
    /* rangee boutons : TRI + ARBRE (topologie mesh par sauts) */
    lv_obj_t *brow = lv_obj_create(hdr);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(brow);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(brow, 6, 0);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_button_create(brow);
    lv_obj_set_size(btn, 150, 28);
    lv_obj_set_style_radius(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, nodes_sort_cb, LV_EVENT_CLICKED, NULL);
    {
        char b[40]; snprintf(b, sizeof(b), LV_SYMBOL_GPS "%s",
                             tr(nodes_sort ? STR_SORT_BTN_SNR : STR_SORT_BTN_RECENT));
        nodes_sort_lbl = label(btn, b, FONT_SMALL, CY_CYAN);
    }
    lv_obj_center(nodes_sort_lbl);

    lv_obj_t *tbtn = lv_button_create(brow);
    lv_obj_set_size(tbtn, 110, 28);
    lv_obj_set_style_radius(tbtn, 2, 0);
    lv_obj_set_style_bg_color(tbtn, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_shadow_width(tbtn, 0, 0);
    lv_obj_add_event_cb(tbtn, ui_nodes_tree_open_e, LV_EVENT_CLICKED, NULL);
    {
        char b[24]; snprintf(b, sizeof(b), LV_SYMBOL_LIST "%s", tr(STR_TREE_BTN));
        lv_obj_t *tl = label(tbtn, b, FONT_SMALL, CY_AMBER);
        lv_obj_center(tl);
    }

    nodes_radio_lbl = label(hdr, "", FONT_SMALL, CY_DIM);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flat(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_set_style_pad_all(list, 5, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    nodes_list = list;

    nodes_sync();
}

/* Rafraîchit la vue courante quand le backend Meshtastic signale du neuf
   (nouveau message reçu, ACK, nœud découvert…). */
static void mesh_refresh_cb(lv_timer_t *t) {
    (void)t;
    /* Vue NODES : maj incrémentale à chaque tick (nouveaux nœuds, signaux ET
     * rafraîchissement du « vu il y a X ») sans recréer la liste -> scroll gardé. */
    if (cur_tab == APP_NODES && nodes_list) nodes_sync();

    if (!mesh_take_dirty()) return;
    ui_chanmgr_refresh_if_open();   /* canaux changés -> rafraîchit le gestionnaire */
    if (cur_tab == APP_CHAT && msg_list) rebuild_messages();
    else if (cur_tab == APP_HOME)        update_msg_badge();
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
/* App BAD USB (bap_*, bad_run_*) deplacee dans ui_badusb.c. */
static lv_obj_t *upd_lbl_state, *upd_lbl_hash, *upd_btn_install; /* MISES A JOUR */
static lv_obj_t  *upd_ov, *upd_bar, *upd_pct;     /* overlay barre de chargement maj */
static lv_timer_t *upd_timer;
static float       upd_disp;                       /* % affiché (lissé) */
static lv_obj_t *sys_btn_usb_share, *sys_btn_usb_client;          /* USB > INTERNET */
static lv_obj_t *sys_log_ta;
static lv_obj_t *sys_bl_slider, *sys_bl_lbl;
static lv_obj_t *sys_sleep_lbl;
lv_timer_t *sys_refresh_timer;     /* partage : timer de la vue courante */

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

lv_obj_t *small_button(lv_obj_t *parent, const char *txt, uint32_t color, lv_event_cb_t cb) {
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

static void reboot_yes(void)      { sys_reboot(); }
static void shutdown_yes(void)    { sys_shutdown(); }
static void restart_app_yes(void) { sys_restart_app(); }
static void reboot_cb(lv_event_t *e)      { (void)e; confirm_dialog(tr(STR_CONFIRM_REBOOT),      reboot_yes); }
static void shutdown_cb(lv_event_t *e)    { (void)e; confirm_dialog(tr(STR_CONFIRM_SHUTDOWN),    shutdown_yes); }
static void restart_app_cb(lv_event_t *e) { (void)e; confirm_dialog(tr(STR_CONFIRM_RESTART_APP), restart_app_yes); }
/* Bascule FR <-> EN, persiste dans config.ini puis reconstruit la vue courante
 * pour rafraichir toutes les chaines a l'ecran (les chaines de status reglees
 * par les timers se mettront aussi a jour au prochain tick). */
static void lang_toggle_cb(lv_event_t *e) {
    (void)e;
    const char *cur = settings_language();
    settings_set_language((cur && cur[0] == 'e') ? "fr" : "en");
    settings_save();
    show_tab(cur_tab);
}
static void calib_cb(lv_event_t *e)    { (void)e; calib_start(NULL); }
static void ssh_toggle_cb(lv_event_t *e) { (void)e; sys_ssh_set(!sys_ssh_running()); }

static void hot_yes(void)        { sys_hotspot_set(!sys_hotspot_active()); }

static void wifi_radio_yes(void) { sys_wifi_radio_set(!sys_wifi_radio_on()); }
static void wifi_radio_toggle_cb(lv_event_t *e) {
    (void)e;
    if (sys_wifi_radio_on())
        confirm_dialog(tr(STR_CONFIRM_WIFI_OFF), wifi_radio_yes);
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
    confirm_dialog(tr(STR_CONFIRM_NET_SHARE), usb_net_share_yes);
}
static void usb_net_client_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_net_mode() == USB_NET_CLIENT) return;
    confirm_dialog(tr(STR_CONFIRM_NET_CLIENT), usb_net_client_yes);
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
        lv_label_set_text(upd_lbl_state, tr(STR_UPDATE_CHECKING));
        lv_obj_set_style_text_color(upd_lbl_state, lv_color_hex(CY_CYAN), 0);
    }
    sys_update_check_async(upd_check_done_cb, NULL);
}
/* Ferme l'overlay de progression et arrête le timer associé. */
static void upd_overlay_close(void) {
    if (upd_timer) { lv_timer_delete(upd_timer); upd_timer = NULL; }
    if (upd_ov)    { lv_obj_delete(upd_ov); upd_ov = NULL; }
    upd_bar = upd_pct = NULL;
}

/* En cas de succès, bq-lora-ui redémarre et tue ce process avant ce callback.
 * On n'arrive ici qu'en cas d'échec (pas d'internet, build cassé, ...). */
static void upd_apply_done_cb(bool ok, void *u) {
    (void)u;
    if (!upd_ov) return;          /* déjà traité par upd_tick (jalon -1) */
    upd_overlay_close();
    if (!ok)
        confirm_dialog(tr(STR_UPDATE_FAILED), NULL);
}

/* Polled ~5x/s : lit le jalon réel et anime la barre en douceur. */
static void upd_tick(lv_timer_t *t) {
    (void)t;
    if (!upd_bar) return;
    int p = sys_update_progress();
    if (p < 0) {                 /* le script a signalé un échec */
        upd_overlay_close();
        confirm_dialog(tr(STR_UPDATE_FAILED), NULL);
        return;
    }
    /* rattrape vite le jalon, sinon avance lentement pour rester vivant */
    if ((float)p > upd_disp) upd_disp += (p - upd_disp) * 0.3f + 0.4f;
    else if (upd_disp < 99.0f) upd_disp += 0.2f;
    if (upd_disp > 99.0f) upd_disp = 99.0f;
    int v = (int)(upd_disp + 0.5f);
    lv_bar_set_value(upd_bar, v, LV_ANIM_OFF);
    if (upd_pct) lv_label_set_text_fmt(upd_pct, "%d%%", v);
}

static void upd_apply_yes(void) {
    /* overlay plein écran avec barre de chargement */
    upd_disp = 0;
    upd_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(upd_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(upd_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(upd_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(upd_ov, 0, 0);
    lv_obj_set_style_radius(upd_ov, 0, 0);
    lv_obj_clear_flag(upd_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(upd_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(upd_ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(upd_ov, 14, 0);

    {
        char tt[40]; snprintf(tt, sizeof(tt), LV_SYMBOL_DOWNLOAD "%s", tr(STR_UPDATE_TITLE));
        label(upd_ov, tt, FONT_BIG, CY_MAGENTA);
    }
    label(upd_ov, tr(STR_UPDATE_INSTALLING), FONT_BODY, CY_DIM);

    upd_bar = lv_bar_create(upd_ov);
    lv_obj_set_size(upd_bar, LV_PCT(80), 16);
    lv_obj_set_style_radius(upd_bar, 2, 0);
    lv_obj_set_style_bg_color(upd_bar, lv_color_hex(CY_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_border_color(upd_bar, lv_color_hex(CY_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(upd_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(upd_bar, lv_color_hex(CY_MAGENTA), LV_PART_INDICATOR);
    lv_bar_set_range(upd_bar, 0, 100);
    lv_bar_set_value(upd_bar, 0, LV_ANIM_OFF);

    upd_pct = label(upd_ov, "0%", FONT_BODY, CY_TEXT);
    label(upd_ov, tr(STR_UPDATE_REBOOT_HINT), FONT_SMALL, CY_DIM);

    upd_timer = lv_timer_create(upd_tick, 200, NULL);
    sys_update_apply_async(upd_apply_done_cb, NULL);
}
static void upd_apply_cb(lv_event_t *e) {
    (void)e;
    confirm_dialog(tr(STR_CONFIRM_UPDATE), upd_apply_yes);
}

/* App BAD USB (bad_run_*, bap_*, usb_mode_btn_*) extraite dans ui_badusb.c. */

static void bl_slider_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = (int)lv_slider_get_value(s);
    if (sys_bl_lbl) {
        char b[8]; snprintf(b, sizeof(b), "%d%%", v);
        lv_label_set_text(sys_bl_lbl, b);
    }
    sys_backlight_set(v);
}

/* Veille ecran : cycle parmi quelques delais d'inactivite. */
static const int SLEEP_OPTS[] = { 0, 30, 60, 120, 300 };
#define SLEEP_NOPTS ((int)(sizeof(SLEEP_OPTS) / sizeof(SLEEP_OPTS[0])))

static void sleep_fmt(int s, char *out, size_t cap) {
    if (s <= 0)        snprintf(out, cap, LV_SYMBOL_POWER "  Jamais");
    else if (s < 60)   snprintf(out, cap, LV_SYMBOL_POWER "  %d s", s);
    else if (s % 60 == 0) snprintf(out, cap, LV_SYMBOL_POWER "  %d min", s / 60);
    else               snprintf(out, cap, LV_SYMBOL_POWER "  %d s", s);
}

static void sleep_cycle_cb(lv_event_t *e) {
    (void)e;
    int cur = settings_screen_timeout();
    int idx = 0;
    for (int i = 0; i < SLEEP_NOPTS; i++) if (SLEEP_OPTS[i] == cur) { idx = i; break; }
    idx = (idx + 1) % SLEEP_NOPTS;
    settings_set_screen_timeout(SLEEP_OPTS[idx]);
    settings_save();
    if (sys_sleep_lbl) {
        char b[32]; sleep_fmt(SLEEP_OPTS[idx], b, sizeof(b));
        lv_label_set_text(sys_sleep_lbl, b);
    }
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

/* Recopie src dans dst en retirant les espaces de début/fin. dst de taille cap. */
static void str_trim(const char *src, char *dst, size_t cap) {
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Valide le nom long du nœud : 1 à 39 caractères imprimables (la limite
 * firmware Meshtastic est ~40 octets pour long_name). Refuse vide/espaces. */
static bool node_name_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 39) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;   /* pas de caractères de contrôle */
    }
    return true;
}

/* Dérive un nom court (≤4 car.) à partir du nom long : initiales des mots si
 * plusieurs mots, sinon les 4 premiers caractères. Résultat en majuscules. */
static void derive_short_name(const char *longn, char *out, size_t cap) {
    char ini[8]; size_t k = 0;
    bool prev_sp = true;
    for (const char *p = longn; *p && k < 4; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) { prev_sp = true; continue; }
        if (prev_sp && isalnum(c)) ini[k++] = (char)toupper(c);
        prev_sp = false;
    }
    if (k >= 2) {                       /* plusieurs mots -> initiales */
        ini[k] = '\0';
    } else {                            /* un seul mot -> 4 premiers car. */
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
        confirm_dialog(tr(STR_INVALID_NODE_NAME), NULL);
        return;                          /* garde le modal ouvert */
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
        confirm_dialog(tr(STR_NAME_SAVED_LOCAL), NULL);
}
static void set_cancel_cb_e(lv_event_t *e) { (void)e; set_modal_close(); }

lv_obj_t *settings_field(lv_obj_t *parent, const char *key, const char *val, bool pwd) {
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
/* Le gestionnaire de canaux a ete extrait dans ui_chanmgr.c (ui_chanmgr_open_e). */

static void hot_toggle_cb(lv_event_t *e) {
    (void)e;
    bool on = sys_hotspot_active();
    confirm_dialog(on ? tr(STR_CONFIRM_HOTSPOT_OFF) : tr(STR_CONFIRM_HOTSPOT_ON), hot_yes);
}
static void wifi_modal_open(void);
static void wifi_btn_cb(lv_event_t *e) { (void)e; wifi_modal_open(); }

static void sys_refresh(lv_timer_t *t) {
    (void)t;
    /* La vue SYSTEME est decoupee en sous-onglets ; selon celui qui est
     * affiche, seule une partie des labels est vivante. On verifie chaque
     * groupe individuellement. sys_info_get() etant couteux (nmcli, lectures
     * /proc), on ne l'appelle que si au moins un consommateur est vivant. */
    bool have_info = sys_lbl_host != NULL;
    bool need_info = have_info || sys_lbl_wifi || sys_lbl_usb_state || sys_lbl_usb_ip;
    sys_info_t i;
    memset(&i, 0, sizeof(i));        /* defaut neutre si pas need_info (silence cppcheck) */
    if (need_info) sys_info_get(&i);
    char b[80];
    if (have_info) {
        lv_label_set_text(sys_lbl_host,   i.hostname);
        lv_label_set_text(sys_lbl_ipw,    i.ip_wlan);
        lv_label_set_text(sys_lbl_ipu,    i.ip_usb);
        lv_label_set_text(sys_lbl_uptime, i.uptime);
        snprintf(b, sizeof(b), "%.1f C", i.cpu_temp_c);  lv_label_set_text(sys_lbl_cpu, b);
        snprintf(b, sizeof(b), "%d / %d MB", i.mem_used_mb, i.mem_total_mb); lv_label_set_text(sys_lbl_mem, b);
        snprintf(b, sizeof(b), "%d %%", i.disk_used_pct); lv_label_set_text(sys_lbl_disk, b);
        lv_label_set_text(sys_lbl_thr, i.throttled_now ? tr(STR_POWER_LOW) : (i.throttled_ever ? tr(STR_POWER_PREV) : tr(STR_POWER_OK)));
        lv_obj_set_style_text_color(sys_lbl_thr,
            lv_color_hex(i.throttled_now ? CY_AMBER : (i.throttled_ever ? CY_DIM : CY_GREEN)), 0);
        lv_label_set_text(sys_lbl_kernel, i.kernel);
    }

    if (sys_lbl_ssh_state) {
        bool running = sys_ssh_running();
        lv_label_set_text(sys_lbl_ssh_state, running ? tr(STR_STATE_ACTIVE) : tr(STR_STATE_STOPPED));
        lv_obj_set_style_text_color(sys_lbl_ssh_state,
            lv_color_hex(running ? CY_GREEN : CY_DIM), 0);
        if (sys_lbl_ssh_btn)
            lv_label_set_text(sys_lbl_ssh_btn, running ? tr(STR_BTN_DISABLE) : tr(STR_BTN_ENABLE));
    }

    if (sys_lbl_wifi) {
        if (i.wifi_signal >= 0)
            snprintf(b, sizeof(b), "%s  (%d%%)", i.wifi_ssid, i.wifi_signal);
        else
            snprintf(b, sizeof(b), "%s", i.wifi_ssid);
        lv_label_set_text(sys_lbl_wifi, b);
    }

    if (sys_lbl_wifi_radio_state) {
        bool radio = sys_wifi_radio_on();
        lv_label_set_text(sys_lbl_wifi_radio_state, radio ? tr(STR_RADIO_ON) : tr(STR_RADIO_OFF));
        lv_obj_set_style_text_color(sys_lbl_wifi_radio_state,
            lv_color_hex(radio ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_wifi_radio_btn, radio ? tr(STR_BTN_DISABLE) : tr(STR_BTN_ENABLE));
    }
    if (sys_lbl_bt_state) {
        bool bt = sys_bt_on();
        lv_label_set_text(sys_lbl_bt_state, bt ? tr(STR_STATE_ACTIVE) : tr(STR_STATE_OFF));
        lv_obj_set_style_text_color(sys_lbl_bt_state,
            lv_color_hex(bt ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_bt_btn, bt ? tr(STR_BTN_DISABLE) : tr(STR_BTN_ENABLE));
    }
    if (sys_lbl_usb_mode_state) {
        usb_mode_t m = sys_usb_mode();
        const char *txt = (m == USB_MODE_HID) ? tr(STR_BTN_USB_MODE_KBD) :
                          (m == USB_MODE_NCM) ? tr(STR_BTN_USB_MODE_NET) : "?";
        lv_label_set_text(sys_lbl_usb_mode_state, txt);
        lv_obj_set_style_text_color(sys_lbl_usb_mode_state,
            lv_color_hex(m == USB_MODE_HID ? CY_MAGENTA : CY_CYAN), 0);
        lv_label_set_text(sys_lbl_usb_mode_btn,
            m == USB_MODE_HID ? tr(STR_BTN_MODE_NET) : tr(STR_BTN_MODE_KBD));
    }

    if (sys_lbl_hot_state) {
        bool hot = sys_hotspot_active();
        lv_label_set_text(sys_lbl_hot_state, hot ? tr(STR_STATE_ACTIVE) : tr(STR_STATE_INACTIVE));
        lv_obj_set_style_text_color(sys_lbl_hot_state,
            lv_color_hex(hot ? CY_GREEN : CY_DIM), 0);
        lv_label_set_text(sys_lbl_hot_btn, hot ? tr(STR_BTN_DISABLE) : tr(STR_BTN_ENABLE));
    }

    if (sys_lbl_usb_state) {
        bool usb_up = usb_client_connected();
        lv_label_set_text(sys_lbl_usb_state, usb_up ? tr(STR_STATE_CONNECTED) : tr(STR_STATE_DISCONNECTED));
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

/* ---- Sous-onglets SYSTEME (decoupe en 3 vues pour reduire le scroll) ----
 * Chaque sous-onglet ne contient que ses propres sections, donc beaucoup
 * moins de widgets a poser : le rendu est instantane et le scroll reste
 * fluide. sys_refresh continue de tourner globalement, les pointeurs des
 * vues non-actives sont remis a NULL et les callbacks deviennent no-op. */
static int       sys_subtab = 0;          /* 0 = systeme, 1 = reseau, 2 = reglages */
static lv_obj_t *sys_content;
static lv_obj_t *sys_chips[3];

static void sys_null_widgets(void) {
    sys_lbl_host = NULL; sys_lbl_ipw = NULL; sys_lbl_ipu = NULL;
    sys_lbl_uptime = NULL; sys_lbl_cpu = NULL; sys_lbl_mem = NULL;
    sys_lbl_disk = NULL; sys_lbl_thr = NULL; sys_lbl_kernel = NULL;
    sys_lbl_ssh_state = NULL; sys_lbl_ssh_btn = NULL; sys_btn_ssh = NULL;
    sys_lbl_bt_state = NULL; sys_lbl_bt_btn = NULL;
    sys_lbl_usb_state = NULL; sys_lbl_usb_ip = NULL;
    sys_btn_usb_share = NULL; sys_btn_usb_client = NULL;
    upd_lbl_state = NULL; upd_lbl_hash = NULL; upd_btn_install = NULL;
    sys_bl_lbl = NULL; sys_bl_slider = NULL; sys_sleep_lbl = NULL;
    sys_log_ta = NULL;
}

static void sys_build_system_tab(lv_obj_t *col)
{
    lv_obj_t *s = section(col, tr(STR_SEC_INFO));
    sys_lbl_host   = info_row(s, tr(STR_INFO_HOSTNAME));
    sys_lbl_ipw    = info_row(s, tr(STR_INFO_IP_WLAN));
    sys_lbl_ipu    = info_row(s, tr(STR_INFO_IP_USB));
    sys_lbl_uptime = info_row(s, tr(STR_INFO_UPTIME));
    sys_lbl_cpu    = info_row(s, tr(STR_INFO_CPU));
    sys_lbl_mem    = info_row(s, tr(STR_INFO_RAM));
    sys_lbl_disk   = info_row(s, tr(STR_INFO_DISK));
    sys_lbl_thr    = info_row(s, tr(STR_INFO_POWER));
    sys_lbl_kernel = info_row(s, tr(STR_INFO_KERNEL));

    s = section(col, tr(STR_SEC_POWER));
    lv_obj_t *row = lv_obj_create(s);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0); lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    {
        char b1[32], b2[32];
        snprintf(b1, sizeof(b1), LV_SYMBOL_POWER "%s",   tr(STR_BTN_SHUTDOWN));
        snprintf(b2, sizeof(b2), LV_SYMBOL_REFRESH "%s", tr(STR_BTN_REBOOT));
        small_button(row, b1, CY_MAGENTA, shutdown_cb);
        small_button(row, b2, CY_CYAN,    reboot_cb);
    }

    s = section(col, tr(STR_SEC_APP));
    {
        char br[40]; snprintf(br, sizeof(br), LV_SYMBOL_REFRESH "%s", tr(STR_BTN_RESTART_APP));
        small_button(s, br, CY_CYAN, restart_app_cb);
    }

    /* MISES A JOUR (git pull + rebuild + restart) */
    s = section(col, tr(STR_SEC_UPDATES));
    upd_lbl_state = label(s, "?", FONT_BODY, CY_DIM);
    upd_lbl_hash  = label(s, "-", FONT_SMALL, CY_DIM);
    lv_obj_t *urow = lv_obj_create(s);
    lv_obj_set_size(urow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(urow); lv_obj_set_flex_flow(urow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(urow, 6, 0);
    lv_obj_clear_flag(urow, LV_OBJ_FLAG_SCROLLABLE);
    {
        char bv[40], bi[40];
        snprintf(bv, sizeof(bv), LV_SYMBOL_REFRESH "%s",  tr(STR_BTN_CHECK_UPDATE));
        snprintf(bi, sizeof(bi), LV_SYMBOL_DOWNLOAD "%s", tr(STR_BTN_INSTALL_UPDATE));
        small_button(urow, bv, CY_CYAN, upd_check_cb);
        upd_btn_install = small_button(urow, bi, CY_MAGENTA, upd_apply_cb);
    }
    lv_obj_add_flag(upd_btn_install, LV_OBJ_FLAG_HIDDEN);
    upd_check_cb(NULL);

    /* LANGUE */
    s = section(col, tr(STR_SEC_LANG));
    lv_obj_t *lang_btn = lv_button_create(s);
    lv_obj_set_size(lang_btn, 130, 30);
    lv_obj_set_style_radius(lang_btn, 2, 0);
    lv_obj_set_style_bg_opa(lang_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(lang_btn, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(lang_btn, 1, 0);
    lv_obj_set_style_border_color(lang_btn, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_shadow_width(lang_btn, 0, 0);
    lv_obj_add_event_cb(lang_btn, lang_toggle_cb, LV_EVENT_CLICKED, NULL);
    {
        const char *cur = settings_language();
        char b[16]; snprintf(b, sizeof(b), "%s " LV_SYMBOL_RIGHT " %s",
                             cur[0] == 'e' ? tr(STR_LANG_EN) : tr(STR_LANG_FR),
                             cur[0] == 'e' ? tr(STR_LANG_FR) : tr(STR_LANG_EN));
        lv_obj_t *ll = label(lang_btn, b, FONT_SMALL, CY_TEXT);
        lv_obj_center(ll);
    }
}

static void sys_build_network_tab(lv_obj_t *col)
{
    lv_obj_t *s = section(col, tr(STR_SEC_SSH));
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
    s = section(col, tr(STR_SEC_BLUETOOTH));
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

    s = section(col, tr(STR_SEC_USB_NET));
    sys_lbl_usb_state = info_row(s, tr(STR_INFO_STATE));
    sys_lbl_usb_ip    = info_row(s, tr(STR_INFO_IP_PI));
    lv_obj_t *usbhint = label(s, tr(STR_USB_NET_HINT), FONT_SMALL, CY_DIM);
    (void)usbhint;

    /* USB > INTERNET : choisir qui sert le DHCP (Pi ou PC via ICS) */
    label(s, tr(STR_USB_INTERNET), FONT_SMALL, CY_DIM);
    lv_obj_t *unrow = lv_obj_create(s);
    lv_obj_set_size(unrow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(unrow); lv_obj_set_flex_flow(unrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(unrow, 6, 0);
    lv_obj_clear_flag(unrow, LV_OBJ_FLAG_SCROLLABLE);
    sys_btn_usb_share  = small_button(unrow, tr(STR_BTN_NET_SHARE),  CY_CYAN,  usb_net_share_cb);
    sys_btn_usb_client = small_button(unrow, tr(STR_BTN_NET_CLIENT), CY_AMBER, usb_net_client_cb);
}

static void sys_build_settings_tab(lv_obj_t *col)
{
    lv_obj_t *s = section(col, tr(STR_SEC_SETTINGS));
    {
        char bm[40]; snprintf(bm, sizeof(bm), LV_SYMBOL_SETTINGS "%s", tr(STR_BTN_MODIFY));
        small_button(s, bm, CY_CYAN, settings_modal_open_e);
    }
    label(s, tr(STR_SETTINGS_DETAILS), FONT_SMALL, CY_DIM);

    s = section(col, tr(STR_SEC_SCREEN));
    /* Luminosité — slider en % */
    lv_obj_t *brow = lv_obj_create(s);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(brow);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
    label(brow, tr(STR_LUMINOSITY), FONT_SMALL, CY_DIM);
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
    {
        char bb[32], bc[32];
        snprintf(bb, sizeof(bb), LV_SYMBOL_AUDIO "%s", tr(STR_BTN_BEEP));
        snprintf(bc, sizeof(bc), LV_SYMBOL_GPS "%s",   tr(STR_BTN_CALIBRATE));
        small_button(erow, bb, CY_MAGENTA, ui_audio_open_e);
        small_button(erow, bc, CY_CYAN,    calib_cb);
    }

    /* Veille ecran : delai d'extinction (reveil au toucher) */
    lv_obj_t *vrow = lv_obj_create(s);
    lv_obj_set_size(vrow, LV_PCT(100), LV_SIZE_CONTENT);
    flat(vrow);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vrow, 6, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
    label(vrow, tr(STR_SCREEN_TIMEOUT), FONT_SMALL, CY_DIM);
    char vb[32]; sleep_fmt(settings_screen_timeout(), vb, sizeof(vb));
    lv_obj_t *vbtn = small_button(vrow, vb, CY_AMBER, sleep_cycle_cb);
    sys_sleep_lbl = lv_obj_get_child(vbtn, 0);

    /* Section LOG */
    s = section(col, tr(STR_SEC_LOG));
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
    {
        char bl[40]; snprintf(bl, sizeof(bl), LV_SYMBOL_REFRESH "%s", tr(STR_BTN_REFRESH_LOG));
        small_button(s, bl, CY_CYAN, log_refresh_cb);
    }
}

/* Rebuild le contenu du sous-onglet courant + met a jour le style des chips. */
static void sys_render_subtab(void)
{
    for (int i = 0; i < 3; i++) {
        if (!sys_chips[i]) continue;
        bool active = (i == sys_subtab);
        lv_obj_set_style_bg_opa(sys_chips[i], active ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(sys_chips[i],
            lv_color_hex(active ? CY_CYAN : CY_BORDER), 0);
    }
    sys_null_widgets();
    if (sys_content) lv_obj_clean(sys_content);
    switch (sys_subtab) {
        case 0: sys_build_system_tab(sys_content);   break;
        case 1: sys_build_network_tab(sys_content);  break;
        case 2: sys_build_settings_tab(sys_content); break;
    }
    sys_refresh(NULL);
}

static void sys_chip_cb(lv_event_t *e)
{
    sys_subtab = (int)(intptr_t)lv_event_get_user_data(e);
    sys_render_subtab();
}

/* Vue SYSTEME : 3 chips de sous-onglet en tete + zone de contenu rebuildable.
 * Chaque sous-onglet contient ~3 sections, donc beaucoup moins de widgets a
 * dessiner que l'ancienne vue monobloc -> scroll fluide / tap instantane. */
static void build_sys(void) {
    sys_null_widgets();
    sys_content = NULL;
    for (int i = 0; i < 3; i++) sys_chips[i] = NULL;

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    flat(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 4, 0);
    lv_obj_set_style_pad_row(root, 4, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* barre de chips de sous-onglet (toujours visible) */
    lv_obj_t *strip = lv_obj_create(root);
    lv_obj_set_size(strip, LV_PCT(100), 30);
    flat(strip);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(strip, 4, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    static const str_id_t labels[3] = { STR_SYSTAB_SYSTEM, STR_SYSTAB_NETWORK, STR_SYSTAB_SETTINGS };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *chip = lv_button_create(strip);
        lv_obj_set_flex_grow(chip, 1);
        lv_obj_set_height(chip, 26);
        lv_obj_set_style_radius(chip, 2, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(CY_CYAN), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(CY_BORDER), 0);
        lv_obj_set_style_shadow_width(chip, 0, 0);
        lv_obj_add_event_cb(chip, sys_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *cl = label(chip, tr(labels[i]), FONT_SMALL, CY_TEXT);
        lv_obj_center(cl);
        sys_chips[i] = chip;
    }

    /* zone de contenu scrollable (mais chaque sous-onglet tient presque dans
     * la hauteur visible -> tres peu de scroll en pratique) */
    sys_content = lv_obj_create(root);
    lv_obj_set_width(sys_content, LV_PCT(100));
    lv_obj_set_flex_grow(sys_content, 1);
    flat(sys_content);
    lv_obj_set_flex_flow(sys_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sys_content, 3, 0);
    lv_obj_set_scroll_dir(sys_content, LV_DIR_VER);

    sys_render_subtab();
    sys_refresh_timer = lv_timer_create(sys_refresh, 5000, NULL);
}

/* ------------- confirmation modale ------------- */
static lv_obj_t *confirm_ov;
static void (*confirm_yes_cb)(void);
static void confirm_close(void) { if (confirm_ov) { lv_obj_delete(confirm_ov); confirm_ov = NULL; } }
static void confirm_yes_e(lv_event_t *e) { (void)e; void (*cb)(void) = confirm_yes_cb; confirm_close(); if (cb) cb(); }
static void confirm_no_e (lv_event_t *e) { (void)e; confirm_close(); }
void confirm_dialog(const char *msg, void (*on_yes)(void)) {
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
    small_button(row, tr(STR_CANCEL),  CY_DIM,     confirm_no_e);
    small_button(row, tr(STR_CONFIRM), CY_MAGENTA, confirm_yes_e);
}

/* ------------- modal WiFi ------------- */
static lv_obj_t *wifi_ov, *wifi_list_ov, *wifi_status, *wifi_pwd_panel, *wifi_pwd_ta;
static char wifi_pending_ssid[64];

/* ----- mode QR (scanner WiFi par code) ----- */
#if CFG_WIFI_QR
#define WIFI_QR_W 320
#define WIFI_QR_H 240
static uint8_t  wifi_qr_buf[WIFI_QR_W * WIFI_QR_H * 2] __attribute__((aligned(4)));
static lv_obj_t *wifi_qr_panel, *wifi_qr_canvas, *wifi_qr_status;
#endif

/* ----- mode WPS ----- */
#if CFG_WPS
static lv_obj_t *wifi_wps_panel, *wifi_wps_status;
static lv_timer_t *wifi_wps_timer;
static int       wifi_wps_remaining;       /* secondes restantes affichees */
#endif

static void wifi_modal_close_e(lv_event_t *e) {
    (void)e;
    /* arrete proprement le flux camera / timer WPS si actifs sous la modal */
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
    if (!wifi_status) return;
    lv_label_set_text(wifi_status, ok ? tr(STR_WIFI_CONNECTED) : msg);
    lv_obj_set_style_text_color(wifi_status, lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
}

static void wifi_pwd_ok_e(lv_event_t *e) {
    (void)e;
    const char *p = lv_textarea_get_text(wifi_pwd_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (wifi_pwd_panel) { lv_obj_delete(wifi_pwd_panel); wifi_pwd_panel = NULL; }
    lv_label_set_text(wifi_status, tr(STR_WIFI_CONNECTING));
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

/* ----- Parser standard WiFi-QR (NFC/Wi-Fi Alliance) -----
 * Format : WIFI:T:<type>;S:<ssid>;P:<pwd>;[H:<bool>;];;
 * Caracteres ';' ':' ',' '\\' '"' echappes par '\'. Le parsing s'arrete au
 * double ';;' final. Renvoie true si SSID present. */
static bool wifi_qr_parse(const char *src, char *ssid, size_t ss, char *pass, size_t ps,
                          char *auth, size_t as)
{
    ssid[0] = pass[0] = auth[0] = 0;
    if (!src || strncmp(src, "WIFI:", 5) != 0) return false;
    const char *p = src + 5;
    while (*p && *p != ';') {                       /* boucle clef:valeur */
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
        sys_wifi_connect_async(ssid, pass, wifi_connect_done, NULL);
    } else if (wifi_qr_status) {
        /* QR detecte mais pas au format WiFi : on continue a scanner */
        lv_label_set_text(wifi_qr_status, tr(STR_WIFI_QR_NOT_WIFI));
        lv_obj_set_style_text_color(wifi_qr_status, lv_color_hex(CY_AMBER), 0);
        /* on re-arme : sys_qr_stop+start est trop lourd, on s'attend a ce que le
         * decodeur retombe sur un autre QR ; ici on quitte au cas ou. */
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

/* ------------- modal Bluetooth (scan BLE + appairage + console serie) ------------- */
static lv_obj_t *bt_ov, *bt_list_ov, *bt_status, *bt_act_panel, *bt_serial_btn_lbl;
static bt_device_t bt_kept[48];
static int  bt_kept_n = 0;

static void bt_modal_close_e(lv_event_t *e) { (void)e; if (bt_ov) { lv_obj_delete(bt_ov); bt_ov = NULL; bt_list_ov = bt_status = bt_act_panel = bt_serial_btn_lbl = NULL; } }

static void bt_scan_done(const bt_device_t *list, int n, void *user);

static void bt_rescan_e(lv_event_t *e) {
    (void)e;
    if (bt_status) lv_label_set_text(bt_status, tr(STR_BT_SCANNING));
    sys_bt_scan_async(bt_scan_done, NULL);
}

static void bt_action_done(bool ok, const char *msg, void *user) {
    (void)user;
    if (bt_status) {
        lv_label_set_text(bt_status, ok ? tr(STR_BT_OK_RESCAN) : (msg && msg[0] ? msg : tr(STR_BT_FAILED)));
        lv_obj_set_style_text_color(bt_status, lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    }
    if (ok) sys_bt_scan_async(bt_scan_done, NULL);  /* rafraîchit l'état */
}

static void bt_act_close(void) {
    if (bt_act_panel) { lv_obj_delete(bt_act_panel); bt_act_panel = NULL; }
}
static void bt_act_close_e(lv_event_t *e) { (void)e; bt_act_close(); }

/* boutons du panneau d'action : user_data = index dans bt_kept */
static void bt_do_pair_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_act_close();
    if (bt_status) { lv_label_set_text(bt_status, tr(STR_BT_PAIRING)); lv_obj_set_style_text_color(bt_status, lv_color_hex(CY_CYAN), 0); }
    sys_bt_action_async("pair", bt_kept[i].addr, bt_action_done, NULL);
}
static void bt_do_connect_e(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_act_close();
    if (bt_status) { lv_label_set_text(bt_status, tr(STR_BT_CONNECTING)); lv_obj_set_style_text_color(bt_status, lv_color_hex(CY_CYAN), 0); }
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
        lv_obj_add_event_cb(small_button(row, tr(STR_DISCONNECT), CY_AMBER, NULL), bt_do_disconnect_e, LV_EVENT_CLICKED, ud);
        lv_obj_add_event_cb(small_button(row, tr(STR_FORGET), CY_MAGENTA, NULL), bt_do_remove_e, LV_EVENT_CLICKED, ud);
    } else if (d->paired) {
        lv_obj_add_event_cb(small_button(row, tr(STR_CONNECT), CY_GREEN, NULL), bt_do_connect_e, LV_EVENT_CLICKED, ud);
        lv_obj_add_event_cb(small_button(row, tr(STR_FORGET), CY_MAGENTA, NULL), bt_do_remove_e, LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_add_event_cb(small_button(row, tr(STR_PAIR), CY_CYAN, NULL), bt_do_pair_e, LV_EVENT_CLICKED, ud);
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

        const char *tag = d->connected ? LV_SYMBOL_OK " " : (d->paired ? LV_SYMBOL_BLUETOOTH " " : "");
        lv_obj_t *l = label(r, "", FONT_SMALL, col == CY_BORDER ? CY_TEXT : col);
        if (d->rssi != 0)
            lv_label_set_text_fmt(l, "%s%s  %ddBm", tag, d->name, d->rssi);
        else
            lv_label_set_text_fmt(l, "%s%s", tag, d->name);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    }
}

static void bt_serial_toggle_e(lv_event_t *e) {
    (void)e;
    bool on = !sys_bt_serial_active();
    sys_bt_serial_set(on);
    if (bt_serial_btn_lbl)
        {
            char cb[48]; snprintf(cb, sizeof(cb), LV_SYMBOL_USB "%s",
                                  on ? tr(STR_BT_CONSOLE_ON) : tr(STR_BT_CONSOLE_OFF));
            lv_label_set_text(bt_serial_btn_lbl, cb);
        }
    if (bt_status) {
        lv_label_set_text(bt_status, on ? tr(STR_BT_SERIAL_VISIBLE) : tr(STR_BT_SERIAL_HIDDEN));
        lv_obj_set_style_text_color(bt_status, lv_color_hex(on ? CY_GREEN : CY_DIM), 0);
    }
}

static void bt_modal_open(void) {
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
    bt_serial_btn_lbl = label(sbtn, sbb,
                              FONT_SMALL, ser ? CY_GREEN : CY_TEXT);
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

/* ---------------------------------------------------------------- routage */
static void build_home(void);
static void build_hotspot_app(void);
/* build_badusb_app supprime : remplace par ui_badusb_build (ui_badusb.h) */
static void build_about(void);
static void build_camera(void);
static void build_gallery(void);
static lv_obj_t *cam_canvas, *cam_status, *cam_btn;   /* fwd : nullifies au changement d'onglet */
static lv_obj_t *gal_canvas, *gal_status, *gal_del_btn, *gal_prev_btn, *gal_next_btn;
static lv_obj_t *gal_frame, *gal_hd_canvas, *gal_nav_btn;
static lv_obj_t *gal_browse_row, *gal_nav_row, *gal_zoom_lbl;

static void show_tab(int app) {
    if (sys_refresh_timer) { lv_timer_delete(sys_refresh_timer); sys_refresh_timer = NULL; }
    /* arrete le flux camera live et oublie ses widgets avant de nettoyer content */
    sys_cam_stream_stop();
    cam_canvas = NULL; cam_status = NULL; cam_btn = NULL;
    gal_canvas = NULL; gal_status = NULL;
    gal_del_btn = NULL; gal_prev_btn = NULL; gal_next_btn = NULL;
    gal_frame = NULL; gal_hd_canvas = NULL; gal_nav_btn = NULL;
    gal_browse_row = NULL; gal_nav_row = NULL; gal_zoom_lbl = NULL;
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
    ui_badusb_reset();
    upd_lbl_state = NULL; upd_lbl_hash = NULL; upd_btn_install = NULL;
    sys_btn_usb_share = NULL; sys_btn_usb_client = NULL;
    sys_log_ta = NULL;
    sys_bl_slider = NULL; sys_bl_lbl = NULL; sys_sleep_lbl = NULL;
    /* le clavier peut avoir remonté la barre de saisie sur la couche top :
     * la replacer sous content pour qu'elle soit bien libérée par le clean */
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (compose_bar) lv_obj_set_parent(compose_bar, content);
    compose_bar  = NULL;
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
#if CFG_BADUSB
        case APP_BADUSB:  ui_badusb_build();     break;
#endif
        case APP_ABOUT:   build_about();         break;
#if CFG_CAMERA
        case APP_CAMERA:  build_camera();        break;
        case APP_GALLERY: build_gallery();       break;
#endif
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
#if CFG_BLUETOOTH
        case APP_BT:
            /* Bluetooth = modal flottant ; on reste conceptuellement sur HOME */
            cur_tab = APP_HOME;
            build_home();
            if (tb_back) {
                lv_obj_add_flag(tb_back, LV_OBJ_FLAG_HIDDEN);
                if (tb_name) lv_obj_align(tb_name, LV_ALIGN_LEFT_MID, 0, 0);
            }
            bt_modal_open();
            break;
#endif
        default: build_home(); break;
    }
}

/* ---------------------------------------------------------------- HOME hub */
typedef struct {
    int          app_id;
    str_id_t     title_id;
    const char  *icon;
    uint32_t     color;
} app_card_t;

static const app_card_t HOME_APPS[] = {
    { APP_CHAT,    STR_TAB_CHAT,     LV_SYMBOL_ENVELOPE,  CY_CYAN    },
    { APP_NODES,   STR_TAB_NODES,    LV_SYMBOL_GPS,       CY_CYAN    },
    { APP_WIFI,    STR_SEC_WIFI,     LV_SYMBOL_WIFI,      CY_CYAN    },
#if CFG_BLUETOOTH
    { APP_BT,      STR_SEC_BLUETOOTH,LV_SYMBOL_BLUETOOTH, CY_CYAN    },
#endif
    { APP_HOTSPOT, STR_TAB_HOTSPOT,  LV_SYMBOL_IMAGE,     CY_MAGENTA },
#if CFG_BADUSB
    { APP_BADUSB,  STR_TAB_BADUSB,   LV_SYMBOL_USB,       CY_MAGENTA },
#endif
#if CFG_CAMERA
    { APP_CAMERA,  STR_TAB_CAMERA,   LV_SYMBOL_IMAGE,     CY_GREEN   },
    { APP_GALLERY, STR_TAB_GALLERY,  LV_SYMBOL_DIRECTORY, CY_GREEN   },
#endif
    { APP_SYS,     STR_TAB_SYS,      LV_SYMBOL_SETTINGS,  CY_AMBER   },
    { APP_ABOUT,   STR_TAB_ABOUT,    LV_SYMBOL_LIST,      CY_AMBER   },
};

/* Apps nécessitant la liaison meshtasticd active. */
static bool app_needs_mesh(int id) {
    return id == APP_CHAT || id == APP_NODES;
}

static void home_card_cb(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (app_needs_mesh(id) && !mesh_enabled()) {
        confirm_dialog(tr(STR_MESH_OFF_HINT), NULL);
        return;
    }
    show_tab(id);
}

/* Bascule l'usage de meshtasticd par l'UI (libère/reprend le port 4403). */
static void mesh_toggle_cb(lv_event_t *e) {
    (void)e;
    bool en = !mesh_enabled();
    mesh_set_enabled(en);
    settings_set_mesh_enabled(en);
    settings_save();
    show_tab(APP_HOME);   /* reconstruit le hub pour refléter l'état */
}

/* Cree/met a jour/efface la pastille de non-lus sur la carte MESSAGES. */
static void update_msg_badge(void) {
    if (!home_msg_card) return;
    unsigned unread = mesh_rx_msg_total() - msg_seen;
    if (home_msg_badge) { lv_obj_delete(home_msg_badge); home_msg_badge = NULL; }
    if (unread == 0) return;

    char buf[8];
    if (unread > 99) snprintf(buf, sizeof(buf), "99+");
    else             snprintf(buf, sizeof(buf), "%u", unread);

    lv_obj_t *b = lv_label_create(home_msg_card);
    home_msg_badge = b;
    lv_label_set_text(b, buf);
    lv_obj_set_style_text_font(b, FONT_SMALL, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 5, 0);
    lv_obj_set_style_pad_ver(b, 1, 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -2, 2);
}

static void build_home(void) {
    /* le contenu vient d'etre nettoye : les anciens objets sont invalides */
    home_msg_card = NULL; home_msg_badge = NULL;

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);

    /* grille 2 col x 3 lignes — occupe la hauteur restante */
    lv_obj_t *grid = lv_obj_create(col);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    flat(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    bool men = mesh_enabled();
    int n = (int)(sizeof(HOME_APPS) / sizeof(HOME_APPS[0]));
    for (int i = 0; i < n; i++) {
        const app_card_t *a = &HOME_APPS[i];
        bool locked = app_needs_mesh(a->app_id) && !men;
        uint32_t bcol = locked ? CY_DIM : a->color;
        uint32_t tcol = locked ? CY_DIM : CY_TEXT;
        lv_obj_t *c = lv_button_create(grid);
        lv_obj_set_size(c, LV_PCT(46), 66);
        lv_obj_set_style_radius(c, 2, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(CY_PANEL), 0);
        lv_obj_set_style_bg_opa(c, locked ? LV_OPA_50 : LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(bcol), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_shadow_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 4, 0);
        lv_obj_add_event_cb(c, home_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a->app_id);

        lv_obj_t *ic = label(c, locked ? LV_SYMBOL_EYE_CLOSE : a->icon, &lv_font_montserrat_16, bcol);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -8);
        lv_obj_t *lbl = label(c, tr(a->title_id), FONT_SMALL, tcol);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -3);

        if (a->app_id == APP_CHAT) home_msg_card = c;   /* support du badge non-lus */
    }
    update_msg_badge();

    /* bascule MESH : l'UI pilote le nœud, ou laisse la main au téléphone */
    uint32_t mcol = men ? CY_GREEN : CY_AMBER;
    lv_obj_t *mb = lv_button_create(col);
    lv_obj_set_size(mb, LV_PCT(100), 40);
    lv_obj_set_style_radius(mb, 2, 0);
    lv_obj_set_style_bg_opa(mb, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(mb, lv_color_hex(mcol), 0);
    lv_obj_set_style_border_width(mb, 1, 0);
    lv_obj_set_style_border_color(mb, lv_color_hex(mcol), 0);
    lv_obj_set_style_shadow_width(mb, 0, 0);
    lv_obj_add_event_cb(mb, mesh_toggle_cb, LV_EVENT_CLICKED, NULL);
    char mbb[64]; snprintf(mbb, sizeof(mbb), LV_SYMBOL_GPS "  %s",
                           men ? tr(STR_MESH_ACTIVE) : tr(STR_MESH_INACTIVE));
    lv_obj_t *ml = label(mb, mbb, FONT_SMALL, men ? CY_GREEN : CY_AMBER);
    lv_obj_center(ml);
}

/* ---------------------------------------------------------------- app HOTSPOT */
static void hap_qr_cb(lv_event_t *e) { qr_open_cb(e); }
static void hap_refresh(lv_timer_t *t) {
    (void)t;
    if (!hap_lbl_state) return;
    bool on = sys_hotspot_active();
    lv_label_set_text(hap_lbl_state, on ? tr(STR_STATE_ACTIVE) : tr(STR_STATE_INACTIVE));
    lv_obj_set_style_text_color(hap_lbl_state, lv_color_hex(on ? CY_GREEN : CY_DIM), 0);
    lv_label_set_text(hap_lbl_btn, on ? tr(STR_BTN_DISABLE) : tr(STR_BTN_ENABLE));
}
static void build_hotspot_app(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(col, tr(STR_HAP_TITLE), FONT_BIG, CY_MAGENTA);
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
    char hqr[40]; snprintf(hqr, sizeof(hqr), LV_SYMBOL_IMAGE "%s", tr(STR_HAP_QR_BTN));
    lv_obj_t *qrb = small_button(col, hqr, CY_MAGENTA, hap_qr_cb);
    lv_obj_set_flex_grow(qrb, 0);
    lv_obj_set_height(qrb, 38);
    lv_obj_set_width(qrb, LV_PCT(100));

    /* refresh */
    hap_refresh(NULL);
    sys_refresh_timer = lv_timer_create(hap_refresh, 3000, NULL);
}

/* ---------------------------------------------------------------- app BAD USB */
/* ---------------------------------------------------------------- app A PROPOS */
/* Petite ligne "cle : valeur" dans un panneau d'infos. */
static void about_kv(lv_obj_t *parent, const char *k, const char *v) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    label(row, k, FONT_SMALL, CY_DIM);
    lv_obj_t *val = label(row, v, FONT_SMALL, CY_TEXT);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_about(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    /* en-tete : titre cyberpunk + version */
    label(col, tr(STR_ABOUT_TITLE), FONT_BIG, CY_CYAN);
    label(col, "/ / L O R A", FONT_BODY, CY_MAGENTA);
    label(col, tr(STR_ABOUT_VERSION), FONT_SMALL, CY_DIM);

    /* materiel */
    lv_obj_t *hw = lv_obj_create(col);
    lv_obj_set_size(hw, LV_PCT(100), LV_SIZE_CONTENT);
    panel(hw, CY_BORDER);
    lv_obj_set_flex_flow(hw, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hw, 4, 0);
    lv_obj_clear_flag(hw, LV_OBJ_FLAG_SCROLLABLE);
    label(hw, tr(STR_ABOUT_HW), FONT_SMALL, CY_AMBER);
    about_kv(hw, "Carte",  "Pi Zero 2 W");
    about_kv(hw, "Ecran",  "MKS TS35-R / ILI9486");
    about_kv(hw, "Tactile","XPT2046");
    about_kv(hw, "Radio",  "SX1262 868 MHz");

    /* logiciel */
    lv_obj_t *sw = lv_obj_create(col);
    lv_obj_set_size(sw, LV_PCT(100), LV_SIZE_CONTENT);
    panel(sw, CY_BORDER);
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sw, 4, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    label(sw, tr(STR_ABOUT_SW), FONT_SMALL, CY_AMBER);
    about_kv(sw, "UI",    "LVGL 9.2 (fbdev)");
    about_kv(sw, "Mesh",  "meshtasticd");
    about_kv(sw, "Noeud", settings_node_name());

    /* projet */
    lv_obj_t *pr = lv_obj_create(col);
    lv_obj_set_size(pr, LV_PCT(100), LV_SIZE_CONTENT);
    panel(pr, CY_BORDER);
    lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pr, 4, 0);
    lv_obj_clear_flag(pr, LV_OBJ_FLAG_SCROLLABLE);
    label(pr, tr(STR_ABOUT_PROJECT), FONT_SMALL, CY_AMBER);
    about_kv(pr, "Auteur", "BugQuest");
    lv_obj_t *gh = label(pr, "github.com/BugQuest/bq-lora", FONT_SMALL, CY_CYAN);
    lv_obj_set_width(gh, LV_PCT(100));
    lv_label_set_long_mode(gh, LV_LABEL_LONG_DOT);
}

/* ---------------------------------------------------------------- app CAMERA */
/* Viewfinder live : rpicam-vid pousse des frames YUV420 que sys.c convertit en
 * RGB565 directement dans cam_pv_buf, puis invalide le canvas (~12 fps).
 * Le bouton CAPTURE coupe brievement le flux (camera mono-acces), prend une
 * photo pleine resolution sur le disque, puis le live reprend.
 * Largeur multiple de 64 (256) -> frame YUV420 compacte, sans padding de stride. */
#define CAM_PV_W 256
#define CAM_PV_H 192
static uint8_t   cam_pv_buf[CAM_PV_W * CAM_PV_H * 2] __attribute__((aligned(4)));
/* cam_canvas / cam_status / cam_btn : declares avant show_tab */
static bool      cam_busy;

/* thread UI : une frame live vient d'etre ecrite dans cam_pv_buf */
static void cam_frame_cb(void *user) {
    (void)user;
    if (cam_canvas) lv_obj_invalidate(cam_canvas);
}

static void cam_stream_resume(void) {
    if (cam_canvas && !sys_cam_stream_active())
        sys_cam_stream_start(cam_pv_buf, CAM_PV_W, CAM_PV_H, cam_frame_cb, NULL);
}

static void cam_capture_done(bool ok, const char *photo,
                             const char *preview, void *user) {
    (void)preview; (void)user;
    cam_busy = false;
    if (cam_btn) lv_obj_clear_state(cam_btn, LV_STATE_DISABLED);
    if (cam_status) {
        if (ok) {
            const char *base = strrchr(photo, '/');
            char b[96];
            snprintf(b, sizeof(b), LV_SYMBOL_OK "  %s", base ? base + 1 : photo);
            lv_label_set_text(cam_status, b);
            lv_obj_set_style_text_color(cam_status, lv_color_hex(CY_GREEN), 0);
        } else {
            {
                char ce[40]; snprintf(ce, sizeof(ce), LV_SYMBOL_WARNING "%s", tr(STR_CAM_CAPTURE_FAILED));
                lv_label_set_text(cam_status, ce);
            }
            lv_obj_set_style_text_color(cam_status, lv_color_hex(CY_MAGENTA), 0);
        }
    }
    cam_stream_resume();   /* le live reprend (si on est toujours sur la page) */
}

static void cam_gallery_cb(lv_event_t *e) {
    (void)e;
    if (cam_busy) return;          /* pas pendant une capture */
    show_tab(APP_GALLERY);
}

static void cam_capture_cb(lv_event_t *e) {
    (void)e;
    if (cam_busy) return;
    cam_busy = true;
    if (cam_btn) lv_obj_add_state(cam_btn, LV_STATE_DISABLED);
    if (cam_status) {
        {
            char ch[40]; snprintf(ch, sizeof(ch), LV_SYMBOL_REFRESH "%s", tr(STR_CAM_HD_CAPTURE));
            lv_label_set_text(cam_status, ch);
        }
        lv_obj_set_style_text_color(cam_status, lv_color_hex(CY_AMBER), 0);
    }
    /* camera mono-acces : on coupe le flux live pendant la photo pleine resolution */
    sys_cam_stream_stop();
    sys_cam_capture_async(CAM_PV_W, CAM_PV_H, cam_capture_done, NULL);
}

static void build_camera(void) {
    cam_canvas = NULL; cam_status = NULL; cam_btn = NULL;

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    label(col, tr(STR_CAMERA_TITLE), FONT_BIG, CY_GREEN);

    /* cadre + canvas de preview */
    lv_obj_t *frame = lv_obj_create(col);
    lv_obj_set_size(frame, CAM_PV_W + 6, CAM_PV_H + 6);
    panel(frame, CY_BORDER);
    lv_obj_set_style_pad_all(frame, 2, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    cam_canvas = lv_canvas_create(frame);
    lv_canvas_set_buffer(cam_canvas, cam_pv_buf, CAM_PV_W, CAM_PV_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_center(cam_canvas);
    lv_canvas_fill_bg(cam_canvas, lv_color_black(), LV_OPA_COVER);

    cam_status = label(col, tr(STR_CAM_LIVE), FONT_SMALL, CY_DIM);
    lv_obj_set_width(cam_status, LV_PCT(100));
    lv_label_set_long_mode(cam_status, LV_LABEL_LONG_DOT);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    {
        char b1[32], b2[32];
        snprintf(b1, sizeof(b1), LV_SYMBOL_IMAGE "%s", tr(STR_CAM_CAPTURE));
        snprintf(b2, sizeof(b2), LV_SYMBOL_LIST "%s",  tr(STR_CAM_GALLERY));
        cam_btn = small_button(row, b1, CY_GREEN, cam_capture_cb);
        small_button(row, b2, CY_CYAN, cam_gallery_cb);
    }

    cam_busy = false;

    /* demarre le viewfinder live */
    sys_cam_stream_start(cam_pv_buf, CAM_PV_W, CAM_PV_H, cam_frame_cb, NULL);
}

/* ---------------------------------------------------------------- app GALERIE */
/* Parcours des photos de ~/bq-lora-ui/photos/. Chaque photo selectionnee est
 * convertie en preview RGB565 (cam.py) puis affichee dans un canvas, comme la
 * capture. Navigation precedent/suivant (cyclique) + suppression confirmee. */
#define GAL_MAX 128
static char      gal_paths[GAL_MAX][256];
static int       gal_n, gal_idx;
static uint8_t   gal_buf[CAM_PV_W * CAM_PV_H * 2] __attribute__((aligned(4)));

/* ---------- Mode navigation (zoom + déplacement) ----------
 * Idée : pour zoomer on a besoin d'une source plus détaillée que la preview
 * 256×192. On rend une seconde preview HD 768×576 (~880 ko RGB565) à la
 * demande, dans un canvas plein-cadre clippé par le frame d'affichage.
 *  - transform_scale gère le facteur de zoom (256 = 1:1 HD = ~3× le « fit »)
 *  - lv_obj_set_pos translate ce canvas dans le frame (= panoramique)
 *  - le drag tactile (LV_EVENT_PRESSING + lv_indev_get_vect) modifie l'offset
 *  - les boutons +/-/FIT/EXIT remplacent prev/del/next pendant le mode. */
#define CAM_HD_W 768
#define CAM_HD_H 576
static uint8_t   gal_hd_buf[CAM_HD_W * CAM_HD_H * 2] __attribute__((aligned(4)));
/* Échelle LVGL : 256 = 1:1. FIT = échelle pour que CAM_HD_W tienne dans CAM_PV_W. */
#define GAL_FIT_SCALE   ((CAM_PV_W * 256) / CAM_HD_W)        /* ≈ 85 */
static bool      gal_nav_mode;
static int       gal_zoom;       /* GAL_FIT_SCALE .. 256 */
static int       gal_ox, gal_oy; /* position top-left du canvas HD dans le frame (px) */

static int gal_view_w(void) { return (CAM_HD_W * gal_zoom) / 256; }
static int gal_view_h(void) { return (CAM_HD_H * gal_zoom) / 256; }

static void gal_clamp_pan(void) {
    int vw = gal_view_w(), vh = gal_view_h();
    int min_x = CAM_PV_W - vw, min_y = CAM_PV_H - vh;
    if (vw <= CAM_PV_W) gal_ox = min_x / 2;
    else { if (gal_ox > 0) gal_ox = 0; if (gal_ox < min_x) gal_ox = min_x; }
    if (vh <= CAM_PV_H) gal_oy = min_y / 2;
    else { if (gal_oy > 0) gal_oy = 0; if (gal_oy < min_y) gal_oy = min_y; }
}

static void gal_apply_transform(void) {
    if (!gal_hd_canvas) return;
    lv_obj_set_style_transform_pivot_x(gal_hd_canvas, 0, 0);
    lv_obj_set_style_transform_pivot_y(gal_hd_canvas, 0, 0);
    lv_obj_set_style_transform_scale_x(gal_hd_canvas, gal_zoom, 0);
    lv_obj_set_style_transform_scale_y(gal_hd_canvas, gal_zoom, 0);
    lv_obj_set_pos(gal_hd_canvas, gal_ox, gal_oy);
    if (gal_zoom_lbl) {
        /* zoom affiché relatif au « fit » (×1.0 = pleine vue, ×3.0 = pixel HD natif) */
        int pct = (gal_zoom * 100 + GAL_FIT_SCALE / 2) / GAL_FIT_SCALE;
        char b[16]; snprintf(b, sizeof(b), "x%d.%d", pct / 100, (pct % 100) / 10);
        lv_label_set_text(gal_zoom_lbl, b);
    }
}

static void gal_hd_done(bool ok, const char *preview, void *user) {
    (void)user;
    if (!gal_hd_canvas) return;
    if (ok) {
        FILE *f = fopen(preview, "rb");
        if (f) {
            size_t got = fread(gal_hd_buf, 1, sizeof(gal_hd_buf), f);
            fclose(f);
            if (got == sizeof(gal_hd_buf)) { lv_obj_invalidate(gal_hd_canvas); return; }
        }
    }
    lv_canvas_fill_bg(gal_hd_canvas, lv_color_black(), LV_OPA_COVER);
}

static void gal_set_nav_mode(bool on) {
    gal_nav_mode = on;
    if (gal_browse_row) { if (on) lv_obj_add_flag(gal_browse_row, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_clear_flag(gal_browse_row, LV_OBJ_FLAG_HIDDEN); }
    if (gal_nav_row)    { if (on) lv_obj_clear_flag(gal_nav_row, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_add_flag(gal_nav_row, LV_OBJ_FLAG_HIDDEN); }
    if (gal_canvas)     { if (on) lv_obj_add_flag(gal_canvas, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_clear_flag(gal_canvas, LV_OBJ_FLAG_HIDDEN); }
    if (gal_hd_canvas)  { if (on) lv_obj_clear_flag(gal_hd_canvas, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_add_flag(gal_hd_canvas, LV_OBJ_FLAG_HIDDEN); }

    if (on && gal_n > 0 && gal_hd_canvas) {
        gal_zoom = GAL_FIT_SCALE;
        gal_ox = gal_oy = 0;
        gal_clamp_pan();
        gal_apply_transform();
        lv_canvas_fill_bg(gal_hd_canvas, lv_color_black(), LV_OPA_COVER);
        sys_cam_preview_async(gal_paths[gal_idx], CAM_HD_W, CAM_HD_H, gal_hd_done, NULL);
    }
}

static void gal_nav_enter_cb(lv_event_t *e) { (void)e; if (gal_n > 0) gal_set_nav_mode(true); }
static void gal_nav_exit_cb (lv_event_t *e) { (void)e; gal_set_nav_mode(false); }

/* Zoom centré sur le milieu du viewport : on conserve la position du pixel
 * central de l'image lors du changement d'échelle. */
static void gal_zoom_apply(int new_zoom) {
    if (new_zoom < GAL_FIT_SCALE) new_zoom = GAL_FIT_SCALE;
    if (new_zoom > 256)            new_zoom = 256;
    int cx = CAM_PV_W / 2, cy = CAM_PV_H / 2;
    int px = cx - gal_ox, py = cy - gal_oy;                     /* coord image actuelle */
    int npx = (px * new_zoom) / (gal_zoom ? gal_zoom : 1);
    int npy = (py * new_zoom) / (gal_zoom ? gal_zoom : 1);
    gal_ox = cx - npx;
    gal_oy = cy - npy;
    gal_zoom = new_zoom;
    gal_clamp_pan();
    gal_apply_transform();
}

static void gal_zoom_in_cb   (lv_event_t *e) { (void)e; gal_zoom_apply(gal_zoom * 3 / 2); }
static void gal_zoom_out_cb  (lv_event_t *e) { (void)e; gal_zoom_apply(gal_zoom * 2 / 3); }
static void gal_zoom_reset_cb(lv_event_t *e) {
    (void)e;
    gal_zoom = GAL_FIT_SCALE; gal_ox = gal_oy = 0;
    gal_clamp_pan(); gal_apply_transform();
}

/* Drag tactile -> pan (uniquement en mode nav et au-delà du fit). */
static void gal_frame_drag_cb(lv_event_t *e) {
    if (!gal_nav_mode) return;
    if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
    lv_indev_t *id = lv_indev_active();
    if (!id) return;
    lv_point_t v; lv_indev_get_vect(id, &v);
    if (v.x == 0 && v.y == 0) return;
    gal_ox += v.x; gal_oy += v.y;
    gal_clamp_pan();
    gal_apply_transform();
}

static void gal_set_nav(bool on) {
    if (gal_prev_btn) { if (on) lv_obj_clear_state(gal_prev_btn, LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_prev_btn, LV_STATE_DISABLED); }
    if (gal_next_btn) { if (on) lv_obj_clear_state(gal_next_btn, LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_next_btn, LV_STATE_DISABLED); }
    if (gal_del_btn)  { if (on) lv_obj_clear_state(gal_del_btn,  LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_del_btn,  LV_STATE_DISABLED); }
    if (gal_nav_btn)  { if (on) lv_obj_clear_state(gal_nav_btn,  LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_nav_btn,  LV_STATE_DISABLED); }
}

static void gal_preview_done(bool ok, const char *preview, void *user) {
    (void)user;
    if (!gal_canvas) return;
    if (ok) {
        FILE *f = fopen(preview, "rb");
        if (f) {
            size_t got = fread(gal_buf, 1, sizeof(gal_buf), f);
            fclose(f);
            if (got == sizeof(gal_buf)) { lv_obj_invalidate(gal_canvas); return; }
        }
    }
    lv_canvas_fill_bg(gal_canvas, lv_color_black(), LV_OPA_COVER);
}

static void gal_show(int idx) {
    if (gal_n <= 0) {
        if (gal_canvas) lv_canvas_fill_bg(gal_canvas, lv_color_black(), LV_OPA_COVER);
        if (gal_status) {
            lv_label_set_text(gal_status, tr(STR_GAL_EMPTY));
            lv_obj_set_style_text_color(gal_status, lv_color_hex(CY_DIM), 0);
        }
        gal_set_nav(false);
        return;
    }
    if (idx < 0)        idx = gal_n - 1;
    else if (idx >= gal_n) idx = 0;
    gal_idx = idx;

    const char *p = gal_paths[gal_idx];
    const char *base = strrchr(p, '/');
    char b[96];
    snprintf(b, sizeof(b), "%s  (%d/%d)", base ? base + 1 : p, gal_idx + 1, gal_n);
    if (gal_status) {
        lv_label_set_text(gal_status, b);
        lv_obj_set_style_text_color(gal_status, lv_color_hex(CY_TEXT), 0);
    }
    gal_set_nav(true);
    sys_cam_preview_async(p, CAM_PV_W, CAM_PV_H, gal_preview_done, NULL);
}

static void gal_prev_cb(lv_event_t *e) { (void)e; if (gal_nav_mode) gal_set_nav_mode(false); gal_show(gal_idx - 1); }
static void gal_next_cb(lv_event_t *e) { (void)e; if (gal_nav_mode) gal_set_nav_mode(false); gal_show(gal_idx + 1); }

static void gal_delete_yes(void) {
    if (gal_n <= 0 || !gal_canvas) return;
    sys_cam_photo_delete(gal_paths[gal_idx]);
    gal_n = sys_cam_photo_list(gal_paths, GAL_MAX);
    gal_show(gal_idx);          /* gal_show borne l'index */
}

static void gal_del_cb(lv_event_t *e) {
    (void)e;
    if (gal_n <= 0) return;
    confirm_dialog(tr(STR_GAL_DELETE_CONFIRM), gal_delete_yes);
}

static void build_gallery(void) {
    gal_canvas = NULL; gal_status = NULL;
    gal_del_btn = NULL; gal_prev_btn = NULL; gal_next_btn = NULL;

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    label(col, tr(STR_GAL_TITLE), FONT_BIG, CY_CYAN);

    gal_frame = lv_obj_create(col);
    lv_obj_set_size(gal_frame, CAM_PV_W + 6, CAM_PV_H + 6);
    panel(gal_frame, CY_BORDER);
    lv_obj_set_style_pad_all(gal_frame, 2, 0);
    lv_obj_clear_flag(gal_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gal_frame, LV_OBJ_FLAG_CLICKABLE);      /* reçoit les press */
    lv_obj_add_event_cb(gal_frame, gal_frame_drag_cb, LV_EVENT_PRESSING, NULL);

    gal_canvas = lv_canvas_create(gal_frame);
    lv_canvas_set_buffer(gal_canvas, gal_buf, CAM_PV_W, CAM_PV_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_center(gal_canvas);
    lv_canvas_fill_bg(gal_canvas, lv_color_black(), LV_OPA_COVER);

    /* canvas HD pour le mode navigation : caché par défaut, clippé par gal_frame */
    gal_hd_canvas = lv_canvas_create(gal_frame);
    lv_canvas_set_buffer(gal_hd_canvas, gal_hd_buf, CAM_HD_W, CAM_HD_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(gal_hd_canvas, 0, 0);
    lv_canvas_fill_bg(gal_hd_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(gal_hd_canvas, LV_OBJ_FLAG_HIDDEN);

    gal_status = label(col, "...", FONT_SMALL, CY_DIM);
    lv_obj_set_width(gal_status, LV_PCT(100));
    lv_label_set_long_mode(gal_status, LV_LABEL_LONG_DOT);

    /* ----- barre de boutons : navigation entre photos (mode browse) ----- */
    gal_browse_row = lv_obj_create(col);
    lv_obj_set_size(gal_browse_row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(gal_browse_row);
    lv_obj_set_flex_flow(gal_browse_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(gal_browse_row, 6, 0);
    lv_obj_clear_flag(gal_browse_row, LV_OBJ_FLAG_SCROLLABLE);
    gal_prev_btn = small_button(gal_browse_row, LV_SYMBOL_LEFT,      CY_CYAN,    gal_prev_cb);
    gal_del_btn  = small_button(gal_browse_row, LV_SYMBOL_TRASH,     CY_MAGENTA, gal_del_cb);
    gal_nav_btn  = small_button(gal_browse_row, LV_SYMBOL_EYE_OPEN,  CY_AMBER,   gal_nav_enter_cb);
    gal_next_btn = small_button(gal_browse_row, LV_SYMBOL_RIGHT,     CY_CYAN,    gal_next_cb);

    /* ----- barre de boutons : zoom/pan (mode nav) ----- */
    gal_nav_row = lv_obj_create(col);
    lv_obj_set_size(gal_nav_row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(gal_nav_row);
    lv_obj_set_flex_flow(gal_nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(gal_nav_row, 6, 0);
    lv_obj_clear_flag(gal_nav_row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(gal_nav_row, LV_SYMBOL_MINUS, CY_CYAN, gal_zoom_out_cb);
    gal_zoom_lbl = label(gal_nav_row, "x1.0", FONT_SMALL, CY_AMBER);
    small_button(gal_nav_row, LV_SYMBOL_PLUS,  CY_CYAN, gal_zoom_in_cb);
    small_button(gal_nav_row, LV_SYMBOL_LOOP,  CY_DIM,  gal_zoom_reset_cb);
    small_button(gal_nav_row, LV_SYMBOL_CLOSE, CY_MAGENTA, gal_nav_exit_cb);
    lv_obj_add_flag(gal_nav_row, LV_OBJ_FLAG_HIDDEN);

    gal_nav_mode = false;
    gal_zoom = GAL_FIT_SCALE; gal_ox = gal_oy = 0;

    gal_n = sys_cam_photo_list(gal_paths, GAL_MAX);
    gal_idx = 0;
    gal_show(0);
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

    /* rafraîchissement des vues chat/nodes sur données Meshtastic entrantes */
    lv_timer_create(mesh_refresh_cb, 700, NULL);

    /* corps = contenu (gauche, extensible) + barre d'état verticale (droite) */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    flat(body);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* barre d'état à gauche, puis contenu (extensible) à droite */
    build_statusbar(body);
    statusbar_refresh(NULL);
    lv_timer_create(statusbar_refresh, 1500, NULL);

    content = lv_obj_create(body);
    lv_obj_set_height(content, LV_PCT(100));
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
