#include "ui.h"
#include "ui_common.h"
#include "ui_audio.h"
#include "ui_node_views.h"
#include "ui_chanmgr.h"
#include "ui_badusb.h"
#include "ui_bt.h"
#include "ui_hotspot.h"
#include "ui_settings.h"
#include "ui_wifi.h"
#include "ui_camera.h"
#include "ui_about.h"
#include "ui_nodes.h"
#include "ui_chat.h"
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

/* ---------------------------------------------------------------- etat
 * Enum APP_* et cur_tab : partages via ui_common.h */
lv_obj_t *content;                 /* zone centrale, reconstruite par app -- partage */
lv_obj_t *kb;                      /* clavier virtuel (overlay, masqué) -- partage */
int       cur_tab  = APP_HOME;     /* defini ici, declare extern dans ui_common.h */
/* cur_tab definition deplacee au-dessus avant le re-include de ui_common.h */

void show_tab(int app);            /* non-static : utilise depuis ui_camera.c */
/* Les helpers UI partages (kb, flat, panel, label, small_button, confirm_dialog)
 * sont declares dans ui_common.h pour etre utilises par les modules separes
 * (ui_audio.c, etc.) -- pas besoin de forward decls locales ici. */
/* bt_modal_open : extrait dans ui_bt.c (ui_bt_modal_open). */
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
/* msg_seen + msg_seen_ch + UI_MAX_CHANS + cur_chan : deplaces dans
 * ui_chat.c, exposes via ui_common.h (HOME lit msg_seen pour le badge). */
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



/* Rafraîchit la vue courante quand le backend Meshtastic signale du neuf
   (nouveau message reçu, ACK, nœud découvert…). */
static void mesh_refresh_cb(lv_timer_t *t) {
    (void)t;
    /* Vue NODES : maj incrémentale à chaque tick (nouveaux nœuds, signaux ET
     * rafraîchissement du « vu il y a X ») sans recréer la liste -> scroll gardé. */
    ui_nodes_sync_if_visible();   /* maj NODES si visible */

    if (!mesh_take_dirty()) return;
    ui_chanmgr_refresh_if_open();   /* canaux changés -> rafraîchit le gestionnaire */
    ui_chat_rebuild_if_visible();    /* maj liste messages si vue CHAT */
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
/* App HOTSPOT (hap_*, qr_open_cb, hot_toggle_cb) extraite dans ui_hotspot.c. */
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

/* Le gestionnaire de canaux a ete extrait dans ui_chanmgr.c (ui_chanmgr_open_e). */

/* hot_toggle_cb : extrait dans ui_hotspot.c */
/* wifi_modal_open : extrait dans ui_wifi.c (ui_wifi_modal_open) */
static void wifi_btn_cb(lv_event_t *e) { (void)e; ui_wifi_modal_open(); }

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
        small_button(s, bm, CY_CYAN, ui_settings_open_e);
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


/* ---------------------------------------------------------------- routage */
static void build_home(void);
/* build_hotspot_app : remplace par ui_hotspot_build (ui_hotspot.h) */
/* build_badusb_app : remplace par ui_badusb_build (ui_badusb.h) */
/* build_about : extrait dans ui_about.c (ui_about_build) */
/* build_camera + build_gallery : extraits dans ui_camera.c
 * (ui_camera_build, ui_gallery_build). Les statics cam_* et gal_* y vivent. */

void show_tab(int app) {
    if (sys_refresh_timer) { lv_timer_delete(sys_refresh_timer); sys_refresh_timer = NULL; }
    /* arrete le flux camera live et oublie ses widgets avant de nettoyer content */
    ui_camera_stream_stop();
    ui_camera_reset();
    ui_gallery_reset();
    /* null tous les pointeurs vers des labels qu'on s'apprete a liberer */
    sys_lbl_host = NULL;
    sys_lbl_wifi = NULL;
    sys_lbl_wifi_radio_state = NULL; sys_lbl_wifi_radio_btn = NULL;
    sys_lbl_hot_state = NULL; sys_lbl_hot_btn = NULL;
    sys_lbl_usb_mode_state = NULL; sys_lbl_usb_mode_btn = NULL;
    sys_lbl_usb_state = NULL; sys_lbl_usb_ip = NULL;
    sys_lbl_bt_state = NULL; sys_lbl_bt_btn = NULL;
    sys_lbl_ssh_state = NULL; sys_lbl_ssh_btn = NULL;
    ui_hotspot_reset();
    ui_badusb_reset();
    upd_lbl_state = NULL; upd_lbl_hash = NULL; upd_btn_install = NULL;
    sys_btn_usb_share = NULL; sys_btn_usb_client = NULL;
    sys_log_ta = NULL;
    sys_bl_slider = NULL; sys_bl_lbl = NULL; sys_sleep_lbl = NULL;
    /* le clavier peut avoir remonté la barre de saisie sur la couche top :
     * la replacer sous content pour qu'elle soit bien libérée par le clean */
    ui_chat_reset();          /* nullifie compose / msg_list + cache clavier */
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
        case APP_CHAT:    ui_chat_build();       break;
        case APP_NODES:   ui_nodes_build();         break;
        case APP_SYS:     build_sys();           break;
        case APP_HOTSPOT: ui_hotspot_build();     break;
#if CFG_BADUSB
        case APP_BADUSB:  ui_badusb_build();     break;
#endif
        case APP_ABOUT:   ui_about_build();         break;
#if CFG_CAMERA
        case APP_CAMERA:  ui_camera_build();      break;
        case APP_GALLERY: ui_gallery_build();     break;
#endif
        case APP_WIFI:
            /* WIFI = modal flottant ; on reste conceptuellement sur HOME */
            cur_tab = APP_HOME;
            build_home();
            if (tb_back) {
                lv_obj_add_flag(tb_back, LV_OBJ_FLAG_HIDDEN);
                if (tb_name) lv_obj_align(tb_name, LV_ALIGN_LEFT_MID, 0, 0);
            }
            ui_wifi_modal_open();
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
            ui_bt_modal_open();
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
    lv_obj_add_event_cb(kb, ui_chat_kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, ui_chat_kb_event_cb, LV_EVENT_CANCEL, NULL);

    show_tab(APP_HOME);
}
