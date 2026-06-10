#include "ui_common.h"
#include "ui_diag.h"
#include "mesh.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ============================================================ */
/* Vue DIAG RF : journal des paquets radio + compteurs par type  */
/* Les lignes de la liste sont pre-creees une fois (slot fixe) : */
/* la sync ne fait que mettre a jour les textes -> pas de flicker */
/* ni de perte de scroll. mesh_pktlog(0) = le plus recent.       */
/* ============================================================ */

#define DIAG_ROW_MAX MESH_PKTLOG_LEN
#define BCAST_ADDR   0xFFFFFFFFu

static lv_obj_t *diag_stat_lbl;    /* ligne RX/TX/util/air */
static lv_obj_t *diag_port_lbl;    /* compteurs par type (multi-lignes) */
static lv_obj_t *diag_list;        /* conteneur scrollable */
static lv_obj_t *diag_rows[DIAG_ROW_MAX];
static lv_obj_t *diag_row_lbl[DIAG_ROW_MAX];
static int       diag_row_built;

static const char *port_name(uint8_t p, bool decoded)
{
    if (!decoded) return "CHIFFRE";
    switch (p) {
        case 1:  return "TEXTE";
        case 3:  return "POSITION";
        case 4:  return "NODEINFO";
        case 5:  return "ROUTAGE";
        case 6:  return "ADMIN";
        case 67: return "TELEM";
        case 70: return "TRACERT";
        default: return "AUTRE";
    }
}

static uint32_t port_color(uint8_t p, bool decoded)
{
    if (!decoded)      return CY_AMBER;   /* recu mais pas decodable */
    switch (p) {
        case 1:  return CY_GREEN;         /* texte = le plus interessant */
        case 4:  return CY_CYAN;          /* nodeinfo */
        default: return CY_DIM;
    }
}

static void diag_sync(void)
{
    if (!diag_list) return;

    /* --- ligne stats globales --- */
    if (diag_stat_lbl) {
        const mesh_stats_t *ms = mesh_stats();
        const mesh_self_t  *sf = mesh_self();
        char b[120];
        snprintf(b, sizeof(b),
                 LV_SYMBOL_DOWNLOAD "%u  " LV_SYMBOL_UPLOAD "%u   util %.1f%%  air %.1f%%",
                 ms->packets_rx, ms->packets_tx, sf->chan_util, sf->air_tx);
        lv_label_set_text(diag_stat_lbl, b);
    }

    /* --- compteurs par type --- */
    if (diag_port_lbl) {
        const mesh_port_stats_t *ps = mesh_port_stats();
        char b[200];
        snprintf(b, sizeof(b),
                 "TXT:%u  POS:%u  NODE:%u  ROUT:%u\n"
                 "ADMIN:%u  TELEM:%u  TRACE:%u  AUTRE:%u\n"
                 LV_SYMBOL_WARNING " CHIFFRE (non decode) : %u",
                 ps->text, ps->position, ps->nodeinfo, ps->routing,
                 ps->admin, ps->telemetry, ps->traceroute, ps->other,
                 ps->encrypted);
        lv_label_set_text(diag_port_lbl, b);
    }

    /* --- liste des paquets (slot fixe, le plus recent en haut) --- */
    int n = mesh_pktlog_count();
    if (n > DIAG_ROW_MAX) n = DIAG_ROW_MAX;
    for (int i = 0; i < DIAG_ROW_MAX; i++) {
        if (!diag_rows[i]) continue;
        if (i >= n) {
            lv_obj_add_flag(diag_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const mesh_pktlog_t *p = mesh_pktlog(i);
        if (!p) { lv_obj_add_flag(diag_rows[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(diag_rows[i], LV_OBJ_FLAG_HIDDEN);

        char tbuf[8];
        time_t t = (time_t)p->epoch;
        struct tm *tm = localtime(&t);
        if (tm) strftime(tbuf, sizeof(tbuf), "%H:%M", tm);
        else    snprintf(tbuf, sizeof(tbuf), "--:--");

        const char *nm = mesh_dm_peer_name(p->from);
        char who[16];
        if (nm && nm[0]) snprintf(who, sizeof(who), "%.10s", nm);
        else             snprintf(who, sizeof(who), "!%08x", p->from);

        char sig[20];
        if (p->have_snr) snprintf(sig, sizeof(sig), "%d/%d", p->snr, p->rssi);
        else             snprintf(sig, sizeof(sig), "-/%d", p->rssi);

        const char *dm = (p->to == BCAST_ADDR) ? "" : " " LV_SYMBOL_CALL;

        char ln[140];
        snprintf(ln, sizeof(ln), "%s  %-10s  %-8s  %s  #%u%s",
                 tbuf, who, port_name(p->portnum, p->decoded), sig, p->chan, dm);
        lv_label_set_text(diag_row_lbl[i], ln);
        lv_obj_set_style_text_color(diag_row_lbl[i],
                lv_color_hex(port_color(p->portnum, p->decoded)), 0);
    }
}

void ui_diag_sync_if_visible(void)
{
    if (diag_list) diag_sync();
}

void ui_diag_reset(void)
{
    diag_stat_lbl = NULL;
    diag_port_lbl = NULL;
    diag_list = NULL;
    diag_row_built = 0;
    for (int i = 0; i < DIAG_ROW_MAX; i++) { diag_rows[i] = NULL; diag_row_lbl[i] = NULL; }
}

void ui_diag_build(void)
{
    ui_diag_reset();

    /* En-tete : stats globales + compteurs par type. */
    lv_obj_t *hdr = lv_obj_create(content);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    flat(hdr);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr, 6, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    diag_stat_lbl = label(hdr, "", FONT_BODY, CY_TEXT);
    lv_obj_set_width(diag_stat_lbl, LV_PCT(100));

    diag_port_lbl = label(hdr, "", FONT_SMALL, CY_DIM);
    lv_obj_set_width(diag_port_lbl, LV_PCT(100));
    lv_label_set_long_mode(diag_port_lbl, LV_LABEL_LONG_WRAP);

    /* Liste live des paquets. */
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flat(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_style_pad_all(list, 5, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    diag_list = list;

    /* Pre-creation des lignes (slots fixes, caches au depart). */
    for (int i = 0; i < DIAG_ROW_MAX; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        flat(row);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *l = label(row, "", FONT_SMALL, CY_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        diag_rows[i] = row;
        diag_row_lbl[i] = l;
    }
    diag_row_built = 1;

    diag_sync();
}
