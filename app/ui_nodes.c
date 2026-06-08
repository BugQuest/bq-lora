#include "ui_common.h"
#include "ui_nodes.h"
#include "ui_node_views.h"
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================ */
/* Vue NODES : liste incrementale (cle = num), tri commutable    */
/* Tap sur une ligne -> ui_node_views.c (node_detail_open)       */
/* ============================================================ */

#define NODE_ROW_MAX 200
typedef struct {
    uint32_t  num;
    lv_obj_t *row;
    lv_obj_t *name_lbl;
    lv_obj_t *right_lbl;
    lv_obj_t *meta_lbl;
    bool      self;
} node_row_t;

static node_row_t s_nrows[NODE_ROW_MAX];
static int        s_nrow_count;
static lv_obj_t  *nodes_list;
static int        nodes_sort;          /* 0 = vu recemment, 1 = meilleur SNR */
static lv_obj_t  *nodes_sort_lbl;
static lv_obj_t  *nodes_radio_lbl;

static node_row_t *node_row_find(uint32_t num) {
    for (int i = 0; i < s_nrow_count; i++)
        if (s_nrows[i].num == num) return &s_nrows[i];
    return NULL;
}

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
        char yb[24]; snprintf(yb, sizeof(yb), LV_SYMBOL_HOME "%s", tr(STR_YOU_BADGE));
        lv_label_set_text(r->right_lbl, yb);
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
    if (na->self != nb->self) return na->self ? -1 : 1;
    if (nodes_sort == 1) {
        if (na->snr != nb->snr) return nb->snr - na->snr;
    }
    if (na->last_heard != nb->last_heard)
        return (nb->last_heard > na->last_heard) ? 1 : -1;
    return 0;
}

static void nodes_sync(void) {
    if (!nodes_list) return;

    /* ligne radio : conditionne directement la portee */
    if (nodes_radio_lbl) {
        const mesh_self_t *sf = mesh_self();
        char tx[16];
        if (sf->tx_power > 0) snprintf(tx, sizeof(tx), "%ddBm", sf->tx_power);
        else                  snprintf(tx, sizeof(tx), "%s", tr(STR_TX_AUTO));
        char rb[160];
        snprintf(rb, sizeof(rb), LV_SYMBOL_GPS "%s", "");
        int off = (int)strlen(rb);
        snprintf(rb + off, sizeof(rb) - off, tr(STR_FMT_RADIO_LINE),
                 sf->region, sf->preset, tx, sf->hop_limit);
        off = (int)strlen(rb);
        const mesh_stats_t *ms = mesh_stats();
        snprintf(rb + off, sizeof(rb) - off, "  RX:%u TX:%u NI:%u",
                 ms->packets_rx, ms->packets_tx, ms->packets_nodeinfo);
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
        lv_obj_move_to_index(r->row, i);
    }
}

void ui_nodes_sync_if_visible(void) {
    if (nodes_list) nodes_sync();
}

void ui_nodes_reset(void) {
    /* Les widgets ont ete detruits par lv_obj_clean(content) -- on null
     * les pointeurs pour que ui_nodes_sync_if_visible devienne no-op. */
    nodes_list = NULL;
    nodes_sort_lbl = NULL;
    nodes_radio_lbl = NULL;
    s_nrow_count = 0;
    /* s_nrows reste alloue (static) mais les pointeurs sont stale ; on
     * remet le count a 0 pour que node_row_find/_create reparte propre
     * au prochain build. */
}

/* Force le re-pull de la config depuis meshtasticd (canaux, preset, region).
 * Necessaire apres une modif externe (CLI --seturl, app mobile) car
 * meshtasticd ne pousse pas spontanement aux clients deja connectes. */
static void nodes_refresh_cb(lv_event_t *e) {
    (void)e;
    mesh_refresh_config();
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

void ui_nodes_build(void) {
    s_nrow_count = 0;
    nodes_list = NULL;
    nodes_radio_lbl = NULL;

    /* Header agrandi : pad confortable + ligne radio bien visible.
     * Boutons en flex_grow=1 -> ils se partagent toute la largeur disponible,
     * pas de scroll horizontal possible quel que soit l'ecran. */
    lv_obj_t *hdr = lv_obj_create(content);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    flat(hdr);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr, 8, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brow = lv_obj_create(hdr);
    lv_obj_set_size(brow, LV_PCT(100), 36);
    flat(brow);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brow, 6, 0);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    /* Helper inline : un bouton flex-grow=1, hauteur 32, fond panel2. */
    struct { const char *txt; uint32_t col; lv_event_cb_t cb; } btns[3] = {
        { NULL,                                CY_CYAN,  nodes_sort_cb     },
        { LV_SYMBOL_LIST " " ,                 CY_AMBER, ui_nodes_tree_open_e },
        { LV_SYMBOL_LOOP " Sync",              CY_GREEN, nodes_refresh_cb  },
    };
    /* Le 1er bouton a un texte dynamique (Recent/SNR) -> on construit a part. */
    char sortb[40];
    snprintf(sortb, sizeof(sortb), LV_SYMBOL_GPS " %s",
             tr(nodes_sort ? STR_SORT_BTN_SNR : STR_SORT_BTN_RECENT));
    btns[0].txt = sortb;
    char treeb[32];
    snprintf(treeb, sizeof(treeb), LV_SYMBOL_LIST " %s", tr(STR_TREE_BTN));
    btns[1].txt = treeb;

    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(brow);
        lv_obj_set_height(b, 32);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(CY_PANEL2), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(btns[i].col), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = label(b, btns[i].txt, FONT_SMALL, btns[i].col);
        lv_obj_center(l);
        if (i == 0) nodes_sort_lbl = l;   /* on retient le label du tri pour le toggle */
    }

    nodes_radio_lbl = label(hdr, "", FONT_SMALL, CY_DIM);
    lv_obj_set_width(nodes_radio_lbl, LV_PCT(100));
    lv_label_set_long_mode(nodes_radio_lbl, LV_LABEL_LONG_WRAP);

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
