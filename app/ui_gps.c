#include "ui_common.h"
#include "ui_gps.h"
#include "gps.h"
#include <stdio.h>
#include <string.h>

/* ============================================================ */
/* Vue GPS debug : statut fix + position + barres SNR par sat.   */
/* Lignes satellites pre-creees (slots fixes) -> pas de flicker. */
/* ============================================================ */

static lv_obj_t *g_status_lbl;   /* NO FIX / FIX 2D / FIX 3D */
static lv_obj_t *g_sat_lbl;      /* sats used/view + HDOP */
static lv_obj_t *g_pos_lbl;      /* lat/lon/alt */
static lv_obj_t *g_kin_lbl;      /* vitesse/cap/heure */
static lv_obj_t *g_dev_lbl;      /* device + trames */
static lv_obj_t *g_list;         /* conteneur barres satellites */

static lv_obj_t *g_row[GPS_MAX_SATS];
static lv_obj_t *g_row_lbl[GPS_MAX_SATS];
static lv_obj_t *g_row_bar[GPS_MAX_SATS];
static lv_obj_t *g_row_snr[GPS_MAX_SATS];

static void gps_sync(void)
{
    if (!g_list) return;
    const gps_state_t *g = gps_state();

    /* --- statut fix --- */
    if (g_status_lbl) {
        const char *txt; uint32_t col;
        if (!gps_enabled())        { txt = "GPS OFF";  col = CY_BORDER;}
        else if (!gps_connected()) { txt = "NO LINK";  col = CY_DIM;   }
        else if (!g->valid)        { txt = "NO FIX";   col = CY_AMBER; }
        else if (g->fix_dim >= 3)  { txt = "FIX 3D";   col = CY_GREEN; }
        else                       { txt = "FIX 2D";   col = CY_GREEN; }
        lv_label_set_text(g_status_lbl, txt);
        lv_obj_set_style_text_color(g_status_lbl, lv_color_hex(col), 0);
    }

    if (g_sat_lbl) {
        char b[64];
        snprintf(b, sizeof(b), LV_SYMBOL_GPS " %d/%d sats   HDOP %.1f",
                 g->sats_used, g->sats_view, g->hdop);
        lv_label_set_text(g_sat_lbl, b);
    }

    if (g_pos_lbl) {
        char b[96];
        if (g->valid)
            snprintf(b, sizeof(b), "lat %.6f\nlon %.6f\nalt %.0f m",
                     g->lat, g->lon, g->alt);
        else
            snprintf(b, sizeof(b), "lat --.------\nlon --.------\nalt -- m");
        lv_label_set_text(g_pos_lbl, b);
    }

    if (g_kin_lbl) {
        char b[80];
        snprintf(b, sizeof(b), "%.1f km/h  cap %.0f\nUTC %s %s",
                 g->valid ? g->speed_kmh : 0.0f,
                 g->valid ? g->course : 0.0f,
                 g->time_utc[0] ? g->time_utc : "--:--:--",
                 g->date_utc[0] ? g->date_utc : "");
        lv_label_set_text(g_kin_lbl, b);
    }

    if (g_dev_lbl) {
        char b[128];
        const char *nav5 = g->ubx_nav5 < 0 ? "?" : (g->ubx_nav5 ? "OK" : "NAK");
        const char *sbas = g->ubx_sbas < 0 ? "?" : (g->ubx_sbas ? "OK" : "NAK");
        snprintf(b, sizeof(b),
                 "/dev/serial0  trames:%u%s\n"
                 "UBX NAV5:%s SBAS:%s  ack:%u nak:%u",
                 g->sentences, gps_connected() ? "" : "  (rien recu)",
                 nav5, sbas, g->ubx_ack, g->ubx_nak);
        lv_label_set_text(g_dev_lbl, b);
    }

    /* --- barres SNR par satellite --- */
    int n = g->sat_n;
    if (n > GPS_MAX_SATS) n = GPS_MAX_SATS;
    for (int i = 0; i < GPS_MAX_SATS; i++) {
        if (!g_row[i]) continue;
        if (i >= n) { lv_obj_add_flag(g_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(g_row[i], LV_OBJ_FLAG_HIDDEN);
        const gps_sat_t *s = &g->sat[i];

        char id[8]; snprintf(id, sizeof(id), "%02d", s->prn);
        lv_label_set_text(g_row_lbl[i], id);

        lv_bar_set_value(g_row_bar[i], s->snr, LV_ANIM_OFF);
        uint32_t bc = s->used ? CY_GREEN : (s->snr > 0 ? CY_CYAN : CY_DIM);
        lv_obj_set_style_bg_color(g_row_bar[i], lv_color_hex(bc), LV_PART_INDICATOR);

        char sn[8]; snprintf(sn, sizeof(sn), "%d", s->snr);
        lv_label_set_text(g_row_snr[i], sn);
        lv_obj_set_style_text_color(g_row_snr[i], lv_color_hex(bc), 0);
    }
}

void ui_gps_sync_if_visible(void) { if (g_list) gps_sync(); }

void ui_gps_reset(void)
{
    g_status_lbl = g_sat_lbl = g_pos_lbl = g_kin_lbl = g_dev_lbl = NULL;
    g_list = NULL;
    for (int i = 0; i < GPS_MAX_SATS; i++) {
        g_row[i] = g_row_lbl[i] = g_row_bar[i] = g_row_snr[i] = NULL;
    }
}

void ui_gps_build(void)
{
    ui_gps_reset();

    /* En-tete : statut + compteurs. */
    lv_obj_t *hdr = lv_obj_create(content);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    flat(hdr);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    g_status_lbl = label(hdr, "NO LINK", &lv_font_montserrat_16, CY_DIM);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    g_sat_lbl = label(hdr, "", FONT_SMALL, CY_TEXT);
    lv_obj_align(g_sat_lbl, LV_ALIGN_TOP_RIGHT, 0, 2);

    g_pos_lbl = label(hdr, "", FONT_BODY, CY_TEXT);
    lv_obj_align(g_pos_lbl, LV_ALIGN_TOP_LEFT, 0, 26);

    g_kin_lbl = label(hdr, "", FONT_SMALL, CY_DIM);
    lv_obj_align(g_kin_lbl, LV_ALIGN_TOP_RIGHT, 0, 30);

    g_dev_lbl = label(hdr, "", FONT_SMALL, CY_DIM);
    lv_obj_align(g_dev_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Liste des satellites (PRN | barre SNR | dB). */
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flat(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_style_pad_all(list, 5, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    g_list = list;

    for (int i = 0; i < GPS_MAX_SATS; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), 18);
        flat(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *id = label(row, "", FONT_SMALL, CY_DIM);
        lv_obj_align(id, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *bar = lv_bar_create(row);
        lv_obj_set_size(bar, LV_PCT(70), 8);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 28, 0);
        lv_bar_set_range(bar, 0, 50);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CY_PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CY_CYAN), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 1, LV_PART_INDICATOR);

        lv_obj_t *snr = label(row, "", FONT_SMALL, CY_CYAN);
        lv_obj_align(snr, LV_ALIGN_RIGHT_MID, 0, 0);

        g_row[i] = row; g_row_lbl[i] = id; g_row_bar[i] = bar; g_row_snr[i] = snr;
    }

    gps_sync();
}
