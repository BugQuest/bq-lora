#include "ui_common.h"
#include "ui_chat.h"
#include "ui_chanmgr.h"
#include "mesh.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Declare dans ui.c : changement d'onglet */
void show_tab(int app);

/* ============================================================ */
/* Vue CHAT : strip canaux + messages + compose                   */
/* Etat partage (cur_chan, msg_seen, msg_seen_ch) : declare dans  */
/* ui_common.h, defini ici (extern depuis HOME et autres modules).*/
/* ============================================================ */

uint8_t  cur_chan = 0;
unsigned msg_seen;
unsigned msg_seen_ch[UI_MAX_CHANS];

static lv_obj_t *msg_list;
static lv_obj_t *compose_ta;
static lv_obj_t *compose_bar;   /* barre de saisie : remontee au-dessus du clavier */

static void add_bubble(lv_obj_t *parent, const mesh_message_t *m) {
    const mesh_channel_t *c = mesh_channel(m->ch);
    bool ch_enc = c ? c->enc : false;

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

void ui_chat_rebuild_if_visible(void) {
    if (cur_tab == APP_CHAT && msg_list) rebuild_messages();
}

static void chan_cb(lv_event_t *e) {
    cur_chan = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (cur_chan < UI_MAX_CHANS)
        msg_seen_ch[cur_chan] = mesh_rx_msg_count(cur_chan);
    show_tab(APP_CHAT);
}

static void send_cb(lv_event_t *e) {
    (void)e;
    const char *txt = lv_textarea_get_text(compose_ta);
    if (!txt || !txt[0]) return;
    /* Throttle TX : refuse l'envoi si l'air-time depasse 10% */
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

void ui_chat_kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY && cur_tab == APP_CHAT && compose_ta &&
        lv_keyboard_get_textarea(kb) == compose_ta) {
        send_cb(e);
    }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    compose_bar_restore();
}

void ui_chat_reset(void) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (compose_bar) lv_obj_set_parent(compose_bar, content);
    compose_bar = NULL;
    compose_ta  = NULL;
    msg_list    = NULL;
}

void ui_chat_build(void) {
    msg_seen = mesh_rx_msg_total();
    if (cur_chan < UI_MAX_CHANS)
        msg_seen_ch[cur_chan] = mesh_rx_msg_count(cur_chan);

    /* rangee des canaux */
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
        /* badge non-lus : petit cercle magenta avec le compte (9+) */
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

    /* bouton gestion des canaux */
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

    /* liste des messages */
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
