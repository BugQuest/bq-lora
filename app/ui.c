#include "lvgl/lvgl.h"
#include "ui.h"
#include "theme.h"
#include "mesh.h"
#include "calib.h"
#include <stdio.h>
#include <stdint.h>

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
static void build_topbar(lv_obj_t *parent) {
    const mesh_self_t *s = mesh_self();
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

    char buf[64];
    snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "  " LV_SYMBOL_WIFI "  " LV_SYMBOL_CHARGE " %d%%", s->batt);
    lv_obj_t *info = label(bar, buf, FONT_SMALL, CY_TEXT);
    lv_obj_align(info, LV_ALIGN_RIGHT_MID, 0, 0);
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
static void stat_card(lv_obj_t *parent, const char *k, const char *v, uint32_t col) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, LV_PCT(48), LV_SIZE_CONTENT);
    panel(c, CY_BORDER);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *kl = label(c, k, FONT_SMALL, CY_DIM);
    lv_obj_align(kl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *vl = label(c, v, FONT_BIG, col);
    lv_obj_align(vl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_pad_top(vl, 16, 0);
}

static void calib_cb(lv_event_t *e) {
    (void)e;
    calib_start(NULL);
}

static void build_sys(void) {
    const mesh_self_t *s = mesh_self();
    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    flat(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(grid, 5, 0);
    lv_obj_set_style_pad_row(grid, 5, 0);
    lv_obj_set_style_pad_column(grid, 5, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);

    char b[32];
    snprintf(b, sizeof(b), "%d%%", s->batt);       stat_card(grid, "BATTERIE", b, s->batt < 25 ? CY_AMBER : CY_GREEN);
    snprintf(b, sizeof(b), "%.2fV", s->volt);       stat_card(grid, "TENSION", b, CY_CYAN);
    snprintf(b, sizeof(b), "%.1f%%", s->chan_util); stat_card(grid, "CANAL", b, CY_CYAN);
    snprintf(b, sizeof(b), "%.1f%%", s->air_tx);    stat_card(grid, "AIR TX", b, CY_CYAN);
    stat_card(grid, "REGION", s->region, CY_MAGENTA);
    stat_card(grid, "PRESET", s->preset, CY_MAGENTA);
    snprintf(b, sizeof(b), "%d", s->nodes);         stat_card(grid, "NOEUDS", b, CY_CYAN);
    stat_card(grid, "UPTIME", s->uptime, CY_CYAN);

    lv_obj_t *cal = lv_button_create(grid);
    lv_obj_set_width(cal, LV_PCT(100));
    lv_obj_set_height(cal, 34);
    lv_obj_set_style_radius(cal, 2, 0);
    lv_obj_set_style_bg_opa(cal, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(cal, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(cal, 1, 0);
    lv_obj_set_style_border_color(cal, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_shadow_width(cal, 0, 0);
    lv_obj_add_event_cb(cal, calib_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = label(cal, LV_SYMBOL_GPS "  CALIBRER L'ECRAN", FONT_BODY, CY_TEXT);
    lv_obj_center(cl);
}

/* ---------------------------------------------------------------- routage */
static void show_tab(int tab) {
    cur_tab = tab;
    lv_obj_clean(content);
    if (tab == 0)      build_chat();
    else if (tab == 1) build_nodes();
    else               build_sys();
    refresh_nav();
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

    content = lv_obj_create(scr);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    flat(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    build_nav(scr);

    /* clavier overlay, masqué par défaut */
    kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(55));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);

    show_tab(0);
}
