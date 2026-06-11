#include "ui_common.h"
#include "ui_chat.h"
#include "ui_chanmgr.h"
#include "ui_radio.h"
#include "ui_dialog.h"
#include "mesh.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Declare dans ui.c : changement d'onglet */
void show_tab(int app);

/* ============================================================ */
/* Vue CHAT a deux niveaux :                                      */
/*   1. LISTE des conversations (canaux) avec details + nb recus  */
/*   2. CHAT du canal selectionne (messages + compose)            */
/* Le bouton retour de la topbar revient de 2 -> 1, puis 1 -> HOME*/
/* (cf. tb_back_cb dans ui.c qui appelle ui_chat_back()).         */
/* Etat partage (cur_chan, msg_seen, msg_seen_ch) : declare dans  */
/* ui_common.h, defini ici (extern depuis HOME et autres modules).*/
/* ============================================================ */

uint8_t  cur_chan = 0;
unsigned msg_seen;
unsigned msg_seen_ch[UI_MAX_CHANS];

enum { CV_LIST = 0, CV_CHAT = 1 };
static int s_view = CV_LIST;

/* Fil ouvert au niveau 2 : si s_dm_peer != 0 c'est un DM avec ce noeud,
 * sinon c'est le canal cur_chan. */
static uint32_t s_dm_peer = 0;

static lv_obj_t *conv_list;     /* niveau 1 : liste des conversations */
static lv_obj_t *msg_list;      /* niveau 2 : bulles de messages */
static lv_obj_t *compose_ta;
static lv_obj_t *compose_bar;   /* barre de saisie : remontee au-dessus du clavier */

/* Entree de la liste des conversations : un canal OU un fil DM. */
typedef struct {
    bool     is_dm;
    uint8_t  ch;     /* si !is_dm : position de canal */
    uint32_t peer;   /* si is_dm  : num du correspondant */
    uint32_t epoch;  /* derniere activite (tri) */
} conv_entry_t;
static conv_entry_t s_convs[UI_MAX_CHANS + 16];
static int          s_conv_n;

/* Suivi "lu" des fils DM (analogue a msg_seen_ch pour les canaux) :
 * petite table peer -> nb de messages recus deja vus. */
static uint32_t dm_seen_peer[16];
static unsigned dm_seen_cnt[16];

static unsigned dm_seen_get(uint32_t peer) {
    for (int i = 0; i < 16; i++) if (dm_seen_peer[i] == peer) return dm_seen_cnt[i];
    return 0;
}
static void dm_seen_set(uint32_t peer, unsigned v) {
    for (int i = 0; i < 16; i++)
        if (dm_seen_peer[i] == peer) { dm_seen_cnt[i] = v; return; }
    for (int i = 0; i < 16; i++)
        if (dm_seen_peer[i] == 0) { dm_seen_peer[i] = peer; dm_seen_cnt[i] = v; return; }
}

/* ------------------------------------------------------------ niveau 2 : chat */

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
    if (!msg_list) return;
    lv_obj_clean(msg_list);
    int n = s_dm_peer ? mesh_dm_message_count(s_dm_peer) : mesh_message_count(cur_chan);
    for (int i = 0; i < n; i++) {
        const mesh_message_t *m = s_dm_peer ? mesh_dm_message(s_dm_peer, i)
                                            : mesh_message(cur_chan, i);
        if (m) add_bubble(msg_list, m);
    }
    lv_obj_t *last = lv_obj_get_child(msg_list, -1);
    if (last) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
}

static void send_cb(lv_event_t *e) {
    (void)e;
    const char *txt = lv_textarea_get_text(compose_ta);
    if (!txt || !txt[0]) return;
    /* Throttle TX : refuse l'envoi si l'air-time depasse 10% */
    const mesh_self_t *sf = mesh_self();
    if (sf && sf->air_tx > 10.0f) {
        ui_dialog_warning(tr(STR_TX_THROTTLED));
        return;
    }
    if (s_dm_peer) mesh_send_dm(s_dm_peer, txt);
    else           mesh_send(cur_chan, txt);
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

static void build_chat_detail(void) {
    bool is_dm = (s_dm_peer != 0);
    if (is_dm) {
        dm_seen_set(s_dm_peer, mesh_dm_rx_count(s_dm_peer));
    } else if (cur_chan < UI_MAX_CHANS) {
        msg_seen_ch[cur_chan] = mesh_rx_msg_count(cur_chan);
    }

    const mesh_channel_t *c = is_dm ? NULL : mesh_channel(cur_chan);
    bool enc = c ? c->enc : false;

    /* en-tete du canal / du fil DM */
    lv_obj_t *hdr = lv_obj_create(content);
    lv_obj_set_size(hdr, LV_PCT(100), 34);
    flat(hdr);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    if (is_dm) {
        const char *pn = mesh_dm_peer_name(s_dm_peer);
        char nm[48];
        if (pn) snprintf(nm, sizeof(nm), LV_SYMBOL_CALL " %s", pn);
        else    snprintf(nm, sizeof(nm), LV_SYMBOL_CALL " !%08x", (unsigned)s_dm_peer);
        label(hdr, nm, FONT_BIG, CY_GREEN);
    } else {
        char nm[40];
        snprintf(nm, sizeof(nm), "%s%s",
                 enc ? LV_SYMBOL_EYE_CLOSE " " : "# ", c ? c->name : "?");
        label(hdr, nm, FONT_BIG, enc ? CY_MAGENTA : CY_CYAN);

        lv_obj_t *mgr = lv_button_create(hdr);
        lv_obj_set_size(mgr, 44, 28);
        lv_obj_set_style_radius(mgr, 2, 0);
        lv_obj_set_style_shadow_width(mgr, 0, 0);
        lv_obj_set_style_bg_opa(mgr, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(mgr, lv_color_hex(CY_AMBER), 0);
        lv_obj_set_style_border_width(mgr, 1, 0);
        lv_obj_set_style_border_color(mgr, lv_color_hex(CY_AMBER), 0);
        lv_obj_add_event_cb(mgr, ui_chanmgr_open_e, LV_EVENT_CLICKED, NULL);
        lv_obj_center(label(mgr, LV_SYMBOL_SETTINGS, FONT_BODY, CY_AMBER));
    }

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
    lv_obj_set_height(compose_ta, 40);
    lv_obj_set_style_bg_color(compose_ta, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_border_color(compose_ta, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(compose_ta, 1, 0);
    lv_obj_set_style_radius(compose_ta, 3, 0);
    lv_obj_set_style_pad_all(compose_ta, 8, 0);
    lv_obj_set_style_text_color(compose_ta, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(compose_ta, FONT_BODY, 0);
    lv_obj_set_style_border_color(compose_ta, lv_color_hex(CY_CYAN), LV_PART_CURSOR);
    lv_obj_set_style_border_width(compose_ta, 2, LV_PART_CURSOR);
    lv_obj_add_event_cb(compose_ta, ta_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send = lv_button_create(bar);
    lv_obj_set_size(send, 50, 40);
    lv_obj_set_style_radius(send, 2, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_add_event_cb(send, send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(send, LV_SYMBOL_UPLOAD, FONT_BODY, CY_BG));

    rebuild_messages();
}

/* ------------------------------------------------------------ niveau 1 : liste */

/* Ouvre une conversation (passe au niveau 2). user_data = index dans s_convs. */
static void conv_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_conv_n) return;
    const conv_entry_t *cv = &s_convs[idx];
    if (cv->is_dm) {
        s_dm_peer = cv->peer;
        dm_seen_set(s_dm_peer, mesh_dm_rx_count(s_dm_peer));
    } else {
        s_dm_peer = 0;
        cur_chan = cv->ch;
        if (cur_chan < UI_MAX_CHANS)
            msg_seen_ch[cur_chan] = mesh_rx_msg_count(cur_chan);
    }
    s_view = CV_CHAT;
    show_tab(APP_CHAT);
}

/* Construit s_convs (canaux + fils DM) trie par activite recente (#3). */
static void build_conv_index(void) {
    s_conv_n = 0;
    for (int i = 0; i < mesh_channel_count() && s_conv_n < (int)(sizeof(s_convs)/sizeof(s_convs[0])); i++) {
        const mesh_channel_t *c = mesh_channel(i);
        if (!c) continue;
        conv_entry_t *e = &s_convs[s_conv_n++];
        e->is_dm = false; e->ch = (uint8_t)i; e->peer = 0;
        e->epoch = mesh_conv_last_epoch((uint8_t)i);
    }
    for (int i = 0; i < mesh_dm_count() && s_conv_n < (int)(sizeof(s_convs)/sizeof(s_convs[0])); i++) {
        uint32_t p = mesh_dm_peer(i);
        if (!p) continue;
        conv_entry_t *e = &s_convs[s_conv_n++];
        e->is_dm = true; e->ch = 0; e->peer = p;
        e->epoch = mesh_dm_last_epoch(p);
    }
    /* tri par epoch decroissant (les conversations sans message, epoch=0,
     * tombent en bas mais restent visibles). Tri stable a bulles : n petit. */
    for (int i = 0; i < s_conv_n - 1; i++)
        for (int j = 0; j < s_conv_n - 1 - i; j++)
            if (s_convs[j].epoch < s_convs[j + 1].epoch) {
                conv_entry_t t = s_convs[j]; s_convs[j] = s_convs[j + 1]; s_convs[j + 1] = t;
            }
}

/* Ajoute un petit badge "non-lus" rond a droite de la ligne titre. */
static void add_unread_badge(lv_obj_t *top, unsigned unread) {
    if (unread == 0) return;
    lv_obj_t *bdg = lv_label_create(top);
    char bb[8]; snprintf(bb, sizeof(bb), "%u%s",
                         (unsigned)(unread > 9 ? 9 : unread), unread > 9 ? "+" : "");
    lv_label_set_text(bdg, bb);
    lv_obj_set_style_text_font(bdg, FONT_SMALL, 0);
    lv_obj_set_style_text_color(bdg, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_bg_color(bdg, lv_color_hex(CY_MAGENTA), 0);
    lv_obj_set_style_bg_opa(bdg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bdg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(bdg, 6, 0);
    lv_obj_set_style_pad_ver(bdg, 1, 0);
}

/* Construit la ligne d'apercu du dernier message (#2) : "from: texte".
 * Tronque proprement, "(aucun message)" si vide. */
static void fmt_preview(char *out, size_t cap, const mesh_message_t *m) {
    if (!m) { snprintf(out, cap, "(aucun message)"); return; }
    const char *who = m->out ? "moi" : (m->from ? m->from : "?");
    snprintf(out, cap, "%s: %s", who, m->text);
}

/* (Re)remplit conv_list avec une carte par conversation. Separe du build pour
 * rafraichir les compteurs sans reconstruire tout l'onglet (preserve le scroll). */
static void populate_conv_cards(void) {
    if (!conv_list) return;
    lv_obj_clean(conv_list);
    build_conv_index();

    for (int k = 0; k < s_conv_n; k++) {
        const conv_entry_t *cv = &s_convs[k];
        bool is_dm = cv->is_dm;
        const mesh_channel_t *c = is_dm ? NULL : mesh_channel(cv->ch);
        if (!is_dm && !c) continue;
        bool enc = c ? c->enc : false;
        uint32_t col = is_dm ? CY_GREEN : (enc ? CY_MAGENTA : CY_CYAN);

        lv_obj_t *card = lv_obj_create(conv_list);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        panel(card, is_dm ? CY_GREEN : (enc ? CY_MAGENTA : CY_BORDER));
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_row(card, 3, 0);
        lv_obj_add_event_cb(card, conv_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);

        /* ligne titre : nom + badge non-lus a droite */
        lv_obj_t *top = lv_obj_create(card);
        lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
        flat(top);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

        char nm[48];
        unsigned tot = 0, unread = 0;
        const mesh_message_t *last = NULL;
        if (is_dm) {
            const char *pn = mesh_dm_peer_name(cv->peer);
            if (pn) snprintf(nm, sizeof(nm), LV_SYMBOL_CALL " %s", pn);
            else    snprintf(nm, sizeof(nm), LV_SYMBOL_CALL " !%08x", (unsigned)cv->peer);
            tot = mesh_dm_rx_count(cv->peer);
            unsigned seen = dm_seen_get(cv->peer);
            if (tot > seen) unread = tot - seen;
            int n = mesh_dm_message_count(cv->peer);
            if (n > 0) last = mesh_dm_message(cv->peer, n - 1);
        } else {
            snprintf(nm, sizeof(nm), "%s%s",
                     enc ? LV_SYMBOL_EYE_CLOSE " " : "# ", c->name);
            if (cv->ch < UI_MAX_CHANS) {
                tot = mesh_rx_msg_count(cv->ch);
                if (tot > msg_seen_ch[cv->ch]) unread = tot - msg_seen_ch[cv->ch];
            }
            int n = mesh_message_count(cv->ch);
            if (n > 0) last = mesh_message(cv->ch, n - 1);
        }
        label(top, nm, FONT_BODY, col);
        add_unread_badge(top, unread);

        /* ligne apercu : dernier message + heure (#2) */
        char prev[120];
        fmt_preview(prev, sizeof(prev), last);
        lv_obj_t *d = label(card, prev, FONT_SMALL, last ? CY_TEXT : CY_DIM);
        lv_obj_set_width(d, LV_PCT(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);

        /* ligne meta : type/role (canal) ou "prive", heure du dernier msg */
        char meta[80];
        const char *when = (last && last->time) ? last->time : "";
        if (is_dm) {
            snprintf(meta, sizeof(meta), "prive - %u recus%s%s",
                     tot, when[0] ? " - " : "", when);
        } else {
            const char *role = c->role == 1 ? "primaire"
                             : c->role == 2 ? "secondaire" : "inactif";
            snprintf(meta, sizeof(meta), "%s - %s - %u recus%s%s",
                     enc ? "chiffre" : "public", role, tot,
                     when[0] ? " - " : "", when);
        }
        lv_obj_t *md = label(card, meta, FONT_SMALL, CY_DIM);
        lv_obj_set_width(md, LV_PCT(100));
    }
}

static void build_conv_list(void) {
    /* Ouvrir la liste vaut acquittement du badge global HOME (mais pas des
     * badges par canal, qui restent jusqu'a ouverture de chaque conversation). */
    msg_seen = mesh_rx_msg_total();

    label(content, "Conversations", FONT_BIG, CY_CYAN);

    conv_list = lv_obj_create(content);
    lv_obj_set_width(conv_list, LV_PCT(100));
    lv_obj_set_flex_grow(conv_list, 1);
    flat(conv_list);
    lv_obj_set_flex_flow(conv_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(conv_list, 6, 0);
    lv_obj_set_style_pad_all(conv_list, 2, 0);
    lv_obj_set_scroll_dir(conv_list, LV_DIR_VER);
    populate_conv_cards();

    /* barre reseau LoRa : gestion des canaux + reglages radio cote a cote.
     * Les deux touchent le meme domaine (le reseau mesh), on les regroupe ici
     * plutot que d'eparpiller la radio sous Systeme. */
    lv_obj_t *mbar = lv_obj_create(content);
    lv_obj_set_size(mbar, LV_PCT(100), 38);
    flat(mbar);
    lv_obj_set_flex_flow(mbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mbar, 6, 0);
    lv_obj_clear_flag(mbar, LV_OBJ_FLAG_SCROLLABLE);
    {
        char mb[40];
        snprintf(mb, sizeof(mb), LV_SYMBOL_SETTINGS "  %s", tr(STR_CHANNELS_TITLE));
        lv_obj_t *bch = small_button(mbar, mb, CY_AMBER, ui_chanmgr_open_e);
        lv_obj_set_flex_grow(bch, 1);
        lv_obj_t *brd = small_button(mbar, LV_SYMBOL_WIFI "  Radio", CY_CYAN, ui_radio_open_e);
        lv_obj_set_flex_grow(brd, 1);
    }
}

/* ------------------------------------------------------------ dispatch / API */

void ui_chat_build(void) {
    msg_seen = mesh_rx_msg_total();
    if (s_view == CV_CHAT) build_chat_detail();
    else                   build_conv_list();
}

void ui_chat_rebuild_if_visible(void) {
    if (cur_tab != APP_CHAT) return;
    if (s_view == CV_CHAT) { if (msg_list)  rebuild_messages(); }
    else                   { if (conv_list) populate_conv_cards(); }
}

/* Appele par la topbar (tb_back_cb) : si on est dans un chat, on revient a la
 * liste des conversations et on signale qu'on a gere le retour. Sinon false ->
 * la topbar fait le retour HOME habituel. */
bool ui_chat_back(void) {
    if (cur_tab == APP_CHAT && s_view == CV_CHAT) {
        s_view = CV_LIST;
        s_dm_peer = 0;
        show_tab(APP_CHAT);
        return true;
    }
    return false;
}

/* Appele quand on entre dans l'onglet CHAT depuis une autre app : on demarre
 * toujours sur la liste des conversations. */
void ui_chat_enter_tab(void) { s_view = CV_LIST; s_dm_peer = 0; }

void ui_chat_reset(void) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (compose_bar) lv_obj_set_parent(compose_bar, content);
    compose_bar = NULL;
    compose_ta  = NULL;
    msg_list    = NULL;
    conv_list   = NULL;
}
