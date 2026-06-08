#include "ui_common.h"
#include "ui_node_views.h"
#include "ui_dialog.h"
#include "mesh.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- Modal "details du node" : stats + sparkline SNR + compose DM ----
 *
 * S'ouvre en tapant sur une ligne de la vue NODES (cf. ui.c, click handler
 * de node_row_t->row qui appelle node_detail_open(num)). Reutilise
 * mesh_node_history pour le ring buffer SNR. Le bouton ENVOYER DM appelle
 * mesh_send_dm avec le num du node selectionne. */

static lv_obj_t *nd_ov, *nd_dm_ta;
static uint32_t  nd_target_num;

static const mesh_node_t *nd_find(uint32_t num) {
    int n = mesh_node_count();
    for (int i = 0; i < n; i++) {
        const mesh_node_t *m = mesh_node(i);
        if (m && m->num == num) return m;
    }
    return NULL;
}
static void nd_close(void) {
    if (nd_ov) { lv_obj_delete(nd_ov); nd_ov = NULL; }
    nd_dm_ta = NULL;
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}
static void nd_close_e(lv_event_t *e) { (void)e; nd_close(); }
static void nd_dm_ta_e(lv_event_t *e) {
    (void)e;
    lv_keyboard_set_textarea(kb, nd_dm_ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}
static void nd_send_e(lv_event_t *e) {
    (void)e;
    if (!nd_dm_ta) return;
    const char *txt = lv_textarea_get_text(nd_dm_ta);
    if (!txt || !txt[0]) return;
    const mesh_self_t *sf = mesh_self();
    if (sf && sf->air_tx > 10.0f) {
        ui_dialog_warning(tr(STR_TX_THROTTLED));
        return;
    }
    mesh_send_dm(nd_target_num, txt);
    nd_close();
}

void node_detail_open(uint32_t num)
{
    const mesh_node_t *n = nd_find(num);
    if (!n) return;
    nd_target_num = num;

    nd_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(nd_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(nd_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(nd_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nd_ov, 0, 0);
    lv_obj_set_style_radius(nd_ov, 0, 0);
    lv_obj_set_style_pad_all(nd_ov, 8, 0);
    lv_obj_clear_flag(nd_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nd_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nd_ov, 4, 0);

    /* en-tete : nom + id */
    char hdr[80]; snprintf(hdr, sizeof(hdr), "%s  %s%s",
                          n->name, n->id, n->self ? "  (VOUS)" : "");
    label(nd_ov, hdr, FONT_BIG, n->self ? CY_CYAN : CY_TEXT);

    /* stats */
    char best[12]; if (n->best_snr == -128) snprintf(best, sizeof(best), "-");
                   else snprintf(best, sizeof(best), "%d", n->best_snr);
    char stat[160];
    snprintf(stat, sizeof(stat),
             "SNR %d dB (max %s)  RSSI %d dBm\n%s%d%%  %dhop  vu %s",
             n->snr, best, n->rssi, LV_SYMBOL_CHARGE, n->batt, n->hops, n->last);
    label(nd_ov, stat, FONT_SMALL, CY_DIM);

    /* sparkline SNR (LVGL chart) */
    label(nd_ov, tr(STR_NODE_HIST_TITLE), FONT_SMALL, CY_AMBER);
    int8_t hs[MESH_HIST_LEN];
    int hlen = mesh_node_history(num, hs, NULL);
    if (hlen <= 0) {
        label(nd_ov, tr(STR_NODE_NO_HIST), FONT_SMALL, CY_DIM);
    } else {
        lv_obj_t *chart = lv_chart_create(nd_ov);
        lv_obj_set_size(chart, LV_PCT(100), 90);
        lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(chart, hlen);
        int mn = hs[0], mx = hs[0];
        for (int i = 1; i < hlen; i++) {
            if (hs[i] < mn) mn = hs[i];
            if (hs[i] > mx) mx = hs[i];
        }
        if (mn == mx) { mn -= 2; mx += 2; }
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, mn - 1, mx + 1);
        lv_obj_set_style_bg_color(chart, lv_color_hex(CY_PANEL2), 0);
        lv_obj_set_style_border_color(chart, lv_color_hex(CY_BORDER), 0);
        lv_obj_set_style_size(chart, 2, 2, LV_PART_INDICATOR);
        lv_chart_series_t *ser = lv_chart_add_series(chart,
            lv_color_hex(CY_CYAN), LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < hlen; i++)
            lv_chart_set_next_value(chart, ser, hs[i]);
    }

    /* compose DM */
    if (!n->self) {
        nd_dm_ta = lv_textarea_create(nd_ov);
        lv_textarea_set_one_line(nd_dm_ta, true);
        lv_textarea_set_placeholder_text(nd_dm_ta, tr(STR_NODE_DM_HINT));
        lv_obj_set_size(nd_dm_ta, LV_PCT(100), 34);
        lv_obj_set_style_bg_color(nd_dm_ta, lv_color_hex(CY_PANEL2), 0);
        lv_obj_set_style_text_color(nd_dm_ta, lv_color_hex(CY_TEXT), 0);
        lv_obj_set_style_text_font(nd_dm_ta, FONT_BODY, 0);
        lv_obj_set_style_border_color(nd_dm_ta, lv_color_hex(CY_BORDER), 0);
        lv_obj_set_style_border_width(nd_dm_ta, 1, 0);
        lv_obj_set_style_radius(nd_dm_ta, 2, 0);
        lv_obj_add_event_cb(nd_dm_ta, nd_dm_ta_e, LV_EVENT_CLICKED, NULL);
    }

    /* barre boutons */
    lv_obj_t *row = lv_obj_create(nd_ov);
    lv_obj_set_size(row, LV_PCT(100), 38);
    flat(row); lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(row, tr(STR_CLOSE), CY_DIM, nd_close_e);
    if (!n->self)
        small_button(row, tr(STR_NODE_DM_SEND), CY_CYAN, nd_send_e);
}

/* ---- Modal "Arbre" : visualisation simple par nombre de sauts ----
 * Liste tous les nodes regroupes par hop count (0=direct, 1, 2, ...). Donne
 * une vue rapide de la topologie sans avoir besoin de tracer le routage
 * complet (les paquets Meshtastic ne portent que hop_start - hop_limit, pas
 * la liste des nodes intermediaires). */
static lv_obj_t *tree_ov;
static void tree_close_e(lv_event_t *e) {
    (void)e;
    if (tree_ov) { lv_obj_delete(tree_ov); tree_ov = NULL; }
}

#define TREE_MAX_HOPS 6
void ui_nodes_tree_open_e(lv_event_t *e) {
    (void)e;
    tree_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(tree_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tree_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(tree_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tree_ov, 0, 0);
    lv_obj_set_style_radius(tree_ov, 0, 0);
    lv_obj_set_style_pad_all(tree_ov, 8, 0);
    lv_obj_clear_flag(tree_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tree_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tree_ov, 5, 0);

    label(tree_ov, tr(STR_TREE_TITLE), FONT_BIG, CY_AMBER);

    int by_hop[TREE_MAX_HOPS + 1] = {0};
    int n = mesh_node_count();
    for (int i = 0; i < n; i++) {
        const mesh_node_t *m = mesh_node(i);
        if (!m || m->self) continue;
        int h = m->hops;
        if (h < 0) h = 0;
        if (h > TREE_MAX_HOPS) h = TREE_MAX_HOPS;
        by_hop[h]++;
    }

    lv_obj_t *list = lv_obj_create(tree_ov);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flat(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int h = 0; h <= TREE_MAX_HOPS; h++) {
        if (by_hop[h] == 0) continue;
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        panel(card, h == 0 ? CY_GREEN : (h <= 2 ? CY_CYAN : CY_AMBER));
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 2, 0);

        char hdr[40];
        if (h == 0) snprintf(hdr, sizeof(hdr), tr(STR_TREE_DIRECT), by_hop[h]);
        else        snprintf(hdr, sizeof(hdr), tr(STR_TREE_HOPS_FMT), h, by_hop[h]);
        label(card, hdr, FONT_BODY, h == 0 ? CY_GREEN : (h <= 2 ? CY_CYAN : CY_AMBER));

        char names[256]; size_t pos = 0; names[0] = 0;
        for (int i = 0; i < n && pos < sizeof(names) - 16; i++) {
            const mesh_node_t *m = mesh_node(i);
            if (!m || m->self) continue;
            int mh = m->hops; if (mh < 0) mh = 0; if (mh > TREE_MAX_HOPS) mh = TREE_MAX_HOPS;
            if (mh != h) continue;
            pos += (size_t)snprintf(names + pos, sizeof(names) - pos,
                                    "%s%s", pos ? ", " : "", m->name);
        }
        lv_obj_t *l = label(card, names, FONT_SMALL, CY_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }

    lv_obj_t *bar = lv_obj_create(tree_ov);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    flat(bar);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    small_button(bar, tr(STR_CLOSE), CY_DIM, tree_close_e);
}
