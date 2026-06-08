#include "ui_common.h"
#include "ui_radio.h"
#include "ui_dialog.h"
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ */
/* Modale Reglages radio + presets sauvegardables               */
/* ============================================================ */

#define RADIO_PRESETS_PATH  "/home/bq-lora/bq-lora-ui/radio_presets.tsv"
#define MAX_PRESETS         16
#define MESHTASTIC_CLI      "/home/bq-lora/.local/bin/meshtastic"

/* Tables de choix : indice -> chaine pour CLI / affichage UI. */
static const char *kRegions[] = {
    "EU_868", "EU_433", "US", "ANZ", "JP", "KR", "TW",
    "RU", "IN", "NZ_865", "TH", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923", NULL
};
static const char *kPresets[] = {
    "LONG_FAST", "LONG_MODERATE", "LONG_SLOW",
    "MEDIUM_FAST", "MEDIUM_SLOW",
    "SHORT_FAST", "SHORT_SLOW", "SHORT_TURBO", NULL
};

typedef struct {
    char  name[24];
    char  region[16];
    char  preset[20];
    int   hop;
    int   tx;
    float freq;     /* override_frequency MHz, 0.0 = utilise la freq preset */
    char  desc[80]; /* description courte (cas d'usage du preset) */
} radio_preset_t;

static radio_preset_t s_presets[MAX_PRESETS];
static int            s_preset_count;

/* Presets installes au premier lancement (si le fichier n'existe pas).
 * Sert de starter kit : l'utilisateur peut les charger, modifier, supprimer. */
static const radio_preset_t kDefaultPresets[] = {
    { "Gaulix FR",        "EU_868", "LONG_MODERATE", 3, 27, 869.4625f,
      "Reseau Gaulix France : portee+, freq 869.4625 separee du LongFast public" },
    { "LongFast public",  "EU_868", "LONG_FAST",     3, 27,   0.0f,
      "Defaut mondial Meshtastic : equilibre debit/portee, 869.525 MHz" },
    { "MediumFast",       "EU_868", "MEDIUM_FAST",   3, 27,   0.0f,
      "Plus de debit, portee reduite (~2x latence/2)" },
    { "ShortFast indoor", "EU_868", "SHORT_FAST",    1, 10,   0.0f,
      "Tests rapproches en interieur, TX faible 10 dBm, hop=1" },
    { "LongSlow DX",      "EU_868", "LONG_SLOW",     5, 27,   0.0f,
      "Portee maximale : LoRa SF12, debit minimal, lointain/relais" },
};
#define DEFAULT_PRESET_COUNT (int)(sizeof(kDefaultPresets) / sizeof(kDefaultPresets[0]))

/* Widgets de la modale (effaces avec l'overlay a la fermeture). */
static lv_obj_t *r_ov;
static lv_obj_t *r_dd_region, *r_dd_preset;
static lv_obj_t *r_sl_hop, *r_sl_hop_lbl;
static lv_obj_t *r_sl_tx,  *r_sl_tx_lbl;
static lv_obj_t *r_ta_name;
static lv_obj_t *r_ta_freq;
static lv_obj_t *r_ta_desc;
static lv_obj_t *r_dd_load;
static lv_obj_t *r_desc_lbl;
static lv_obj_t *r_status;

/* ------------------------------------------------------------ helpers I/O */

static void presets_save(void);   /* fwd */

/* Installe les presets par defaut + persiste. Idempotent : appele uniquement
 * si le fichier n'existe pas (premier lancement). */
static void presets_install_defaults(void)
{
    s_preset_count = 0;
    int n = DEFAULT_PRESET_COUNT;
    if (n > MAX_PRESETS) n = MAX_PRESETS;
    for (int i = 0; i < n; i++) s_presets[s_preset_count++] = kDefaultPresets[i];
    presets_save();
}

static void presets_load(void)
{
    s_preset_count = 0;
    FILE *f = fopen(RADIO_PRESETS_PATH, "r");
    if (!f) { presets_install_defaults(); return; }
    char line[256];
    while (s_preset_count < MAX_PRESETS && fgets(line, sizeof(line), f)) {
        radio_preset_t p;
        memset(&p, 0, sizeof(p));
        /* Format etendu : nom region preset hop tx [freq] [desc]
         * Compat 5/6 colonnes : freq/desc partent a 0/"" si absents. */
        int got = sscanf(line, "%23[^\t]\t%15[^\t]\t%19[^\t]\t%d\t%d\t%f\t%79[^\t\n]",
                         p.name, p.region, p.preset, &p.hop, &p.tx, &p.freq, p.desc);
        if (got >= 5) s_presets[s_preset_count++] = p;
    }
    fclose(f);
}

static void presets_save(void)
{
    char tmp[256]; snprintf(tmp, sizeof(tmp), "%s.tmp", RADIO_PRESETS_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < s_preset_count; i++) {
        const radio_preset_t *p = &s_presets[i];
        fprintf(f, "%s\t%s\t%s\t%d\t%d\t%.4f\t%s\n",
                p->name, p->region, p->preset, p->hop, p->tx, p->freq, p->desc);
    }
    fclose(f);
    rename(tmp, RADIO_PRESETS_PATH);
}

/* ------------------------------------------------------------ helpers UI */

static int str_index(const char *const *arr, const char *s)
{
    if (!s) return 0;
    for (int i = 0; arr[i]; i++)
        if (strcmp(arr[i], s) == 0) return i;
    return 0;
}

static void status_set(const char *txt, uint32_t color)
{
    if (!r_status) return;
    lv_label_set_text(r_status, txt);
    lv_obj_set_style_text_color(r_status, lv_color_hex(color), 0);
}

static void dd_options_from(lv_obj_t *dd, const char *const *arr)
{
    char buf[512]; buf[0] = '\0';
    size_t off = 0;
    for (int i = 0; arr[i] && off < sizeof(buf) - 32; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "%s%s", i ? "\n" : "", arr[i]);
    }
    lv_dropdown_set_options(dd, buf);
}

static void dd_options_presets(lv_obj_t *dd)
{
    char buf[512];
    int off = snprintf(buf, sizeof(buf), "%s", "-- charger --");
    for (int i = 0; i < s_preset_count && off < (int)sizeof(buf) - 32; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "\n%s", s_presets[i].name);
    }
    lv_dropdown_set_options(dd, buf);
}

/* ------------------------------------------------------------ apply / save */

static void apply_clicked(lv_event_t *e)
{
    (void)e;
    char region[16], preset[20];
    lv_dropdown_get_selected_str(r_dd_region, region, sizeof(region));
    lv_dropdown_get_selected_str(r_dd_preset, preset, sizeof(preset));
    int hop = (int)lv_slider_get_value(r_sl_hop);
    int tx  = (int)lv_slider_get_value(r_sl_tx);
    float freq = 0.0f;
    const char *ftxt = lv_textarea_get_text(r_ta_freq);
    if (ftxt && ftxt[0]) freq = strtof(ftxt, NULL);

    /* Une seule commande qui groupe tous les --set : la radio reboot une fois.
     * override_frequency : 0 = utilise la freq par defaut du preset/region. */
    char cmd[640];
    snprintf(cmd, sizeof(cmd),
             "%s --host 127.0.0.1 "
             "--set lora.region %s "
             "--set lora.modem_preset %s "
             "--set lora.use_preset true "
             "--set lora.hop_limit %d "
             "--set lora.tx_power %d "
             "--set lora.override_frequency %.4f "
             ">/tmp/radio_apply.log 2>&1",
             MESHTASTIC_CLI, region, preset, hop, tx, freq);
    /* Loading visible pendant que la CLI meshtastic pousse la conf et que la
     * radio reboot. Operation potentiellement bloquante (~1-3s). */
    ui_dialog_loading_show("Application de la config radio...");
    int rc = system(cmd);
    ui_dialog_loading_hide();
    if (rc == 0) {
        status_set("Applique. Re-sync...", CY_GREEN);
        mesh_refresh_config();
        ui_dialog_info("Configuration appliquee. La radio se reconfigure.");
    } else {
        status_set("Echec apply", CY_MAGENTA);
        ui_dialog_error("Echec de l'application. Voir /tmp/radio_apply.log");
    }
}

static void save_clicked(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(r_ta_name);
    if (!name || !name[0]) {
        status_set("Nom requis", CY_AMBER);
        ui_dialog_warning("Le nom du preset est obligatoire pour sauvegarder.");
        return;
    }
    ui_dialog_loading_show("Sauvegarde du preset...");

    radio_preset_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "%s", name);
    lv_dropdown_get_selected_str(r_dd_region, p.region, sizeof(p.region));
    lv_dropdown_get_selected_str(r_dd_preset, p.preset, sizeof(p.preset));
    p.hop = (int)lv_slider_get_value(r_sl_hop);
    p.tx  = (int)lv_slider_get_value(r_sl_tx);
    {
        const char *ftxt = lv_textarea_get_text(r_ta_freq);
        p.freq = (ftxt && ftxt[0]) ? strtof(ftxt, NULL) : 0.0f;
        const char *dtxt = lv_textarea_get_text(r_ta_desc);
        if (dtxt) snprintf(p.desc, sizeof(p.desc), "%s", dtxt);
    }

    /* upsert par nom */
    int idx = -1;
    for (int i = 0; i < s_preset_count; i++)
        if (strcmp(s_presets[i].name, p.name) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_preset_count >= MAX_PRESETS) {
            ui_dialog_loading_hide();
            status_set("Limite atteinte", CY_AMBER);
            ui_dialog_error("Nombre maximum de presets atteint (16).");
            return;
        }
        idx = s_preset_count++;
    }
    s_presets[idx] = p;
    presets_save();
    dd_options_presets(r_dd_load);
    ui_dialog_loading_hide();
    status_set("Preset sauvegarde", CY_GREEN);
    {
        char m[96]; snprintf(m, sizeof(m), "Preset \"%s\" sauvegarde.", p.name);
        ui_dialog_info(m);
    }
}

static void load_changed(lv_event_t *e)
{
    (void)e;
    int sel = (int)lv_dropdown_get_selected(r_dd_load);
    if (sel <= 0) return;                     /* 0 = placeholder */
    int idx = sel - 1;
    if (idx < 0 || idx >= s_preset_count) return;
    ui_dialog_loading_show("Chargement du preset...");
    const radio_preset_t *p = &s_presets[idx];
    lv_dropdown_set_selected(r_dd_region, str_index(kRegions, p->region));
    lv_dropdown_set_selected(r_dd_preset, str_index(kPresets, p->preset));
    lv_slider_set_value(r_sl_hop, p->hop, LV_ANIM_OFF);
    lv_slider_set_value(r_sl_tx,  p->tx,  LV_ANIM_OFF);
    char b[16];
    snprintf(b, sizeof(b), "%d", p->hop); lv_label_set_text(r_sl_hop_lbl, b);
    snprintf(b, sizeof(b), "%d", p->tx);  lv_label_set_text(r_sl_tx_lbl,  b);
    lv_textarea_set_text(r_ta_name, p->name);
    if (p->freq > 0.0001f) { snprintf(b, sizeof(b), "%.4f", p->freq); lv_textarea_set_text(r_ta_freq, b); }
    else                   { lv_textarea_set_text(r_ta_freq, ""); }
    if (r_ta_desc)  lv_textarea_set_text(r_ta_desc, p->desc);
    if (r_desc_lbl) lv_label_set_text(r_desc_lbl, p->desc[0] ? p->desc : " ");
    ui_dialog_loading_hide();
    status_set("Preset charge (non applique)", CY_CYAN);
    {
        char m[128];
        snprintf(m, sizeof(m), "Preset \"%s\" charge.\nTape Apply pour activer.", p->name);
        ui_dialog_info(m);
    }
}

static void do_delete(void)
{
    const char *name = r_ta_name ? lv_textarea_get_text(r_ta_name) : NULL;
    if (!name || !name[0]) return;
    for (int i = 0; i < s_preset_count; i++) {
        if (strcmp(s_presets[i].name, name) == 0) {
            memmove(&s_presets[i], &s_presets[i + 1],
                    sizeof(s_presets[0]) * (s_preset_count - i - 1));
            s_preset_count--;
            presets_save();
            dd_options_presets(r_dd_load);
            status_set("Supprime", CY_AMBER);
            ui_dialog_info("Preset supprime.");
            return;
        }
    }
    status_set("Nom inconnu", CY_AMBER);
    ui_dialog_warning("Aucun preset de ce nom dans la liste.");
}

static void delete_clicked(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(r_ta_name);
    if (!name || !name[0]) {
        ui_dialog_warning("Saisis le nom du preset a supprimer.");
        return;
    }
    char m[128];
    snprintf(m, sizeof(m), "Supprimer definitivement le preset \"%s\" ?", name);
    ui_dialog_confirm(m, do_delete);
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    if (r_ov) { lv_obj_delete(r_ov); r_ov = NULL; }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ scroll-into-view
 * Le clavier overlay prend ~LV_PCT(55) en bas de l'ecran. Quand une TA recoit
 * le focus, on s'assure qu'elle n'est pas sous le bord superieur du clavier
 * en scrollant la modale (r_ov) de la difference. */
static void radio_ta_focus_scroll(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (!r_ov || !ta) return;
    /* le clavier couvre les 55% du bas -> top utilisable = 45% */
    lv_coord_t scr_h  = lv_display_get_vertical_resolution(NULL);
    lv_coord_t kb_top = scr_h * 45 / 100;
    lv_area_t a; lv_obj_get_coords(ta, &a);
    lv_coord_t margin = 16;
    if (a.y2 > kb_top - margin) {
        lv_coord_t delta = (a.y2 - (kb_top - margin));
        lv_obj_scroll_by(r_ov, 0, -delta, LV_ANIM_ON);
    }
}

/* ------------------------------------------------------------ callbacks UI */

static void hop_changed(lv_event_t *e)
{
    (void)e;
    char b[8]; snprintf(b, sizeof(b), "%d", (int)lv_slider_get_value(r_sl_hop));
    lv_label_set_text(r_sl_hop_lbl, b);
}
static void tx_changed(lv_event_t *e)
{
    (void)e;
    int v = (int)lv_slider_get_value(r_sl_tx);
    char b[12];
    if (v == 0) snprintf(b, sizeof(b), "auto");
    else        snprintf(b, sizeof(b), "%d dBm", v);
    lv_label_set_text(r_sl_tx_lbl, b);
}

/* ------------------------------------------------------------ build */

static lv_obj_t *form_row(lv_obj_t *parent, const char *key)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    label(row, key, FONT_BODY, CY_DIM);
    return row;
}

static void style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_size(dd, 180, 36);
    lv_obj_set_style_bg_color(dd, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(dd, FONT_BODY, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, 3, 0);
}

static void style_slider(lv_obj_t *s)
{
    lv_obj_set_size(s, 180, 14);
    lv_obj_set_style_bg_color(s, lv_color_hex(CY_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(CY_CYAN),   LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(CY_CYAN),   LV_PART_KNOB);
    lv_obj_set_style_radius(s, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 3, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 3, LV_PART_KNOB);
}

void ui_radio_open_e(lv_event_t *e)
{
    (void)e;
    presets_load();
    const mesh_self_t *sf = mesh_self();

    r_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(r_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(r_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(r_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r_ov, 0, 0);
    lv_obj_set_style_radius(r_ov, 0, 0);
    lv_obj_set_style_pad_all(r_ov, 10, 0);
    lv_obj_set_flex_flow(r_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r_ov, 8, 0);

    label(r_ov, "Reglages radio", FONT_BIG, CY_CYAN);

    /* Region */
    lv_obj_t *row = form_row(r_ov, "Region");
    r_dd_region = lv_dropdown_create(row);
    style_dropdown(r_dd_region);
    dd_options_from(r_dd_region, kRegions);
    lv_dropdown_set_selected(r_dd_region, str_index(kRegions, sf->region));

    /* Preset */
    row = form_row(r_ov, "Preset");
    r_dd_preset = lv_dropdown_create(row);
    style_dropdown(r_dd_preset);
    dd_options_from(r_dd_preset, kPresets);
    /* Le preset stocke dans mesh_self est cosmetique (string libre), on essaie
     * une correspondance approximative ; sinon default = LONG_FAST. */
    {
        const char *p = sf->preset ? sf->preset : "LONG_FAST";
        int found = 0;
        for (int i = 0; kPresets[i]; i++) {
            if (strstr(p, kPresets[i]) || strstr(kPresets[i], p)) {
                lv_dropdown_set_selected(r_dd_preset, i); found = 1; break;
            }
        }
        if (!found) lv_dropdown_set_selected(r_dd_preset, 0);
    }

    /* Hop limit */
    row = form_row(r_ov, "Hop limit");
    r_sl_hop_lbl = label(row, "3", FONT_BODY, CY_CYAN);
    r_sl_hop = lv_slider_create(r_ov);
    style_slider(r_sl_hop);
    lv_obj_set_width(r_sl_hop, LV_PCT(100));
    lv_slider_set_range(r_sl_hop, 1, 7);
    lv_slider_set_value(r_sl_hop, sf->hop_limit ? sf->hop_limit : 3, LV_ANIM_OFF);
    lv_obj_add_event_cb(r_sl_hop, hop_changed, LV_EVENT_VALUE_CHANGED, NULL);
    { char b[8]; snprintf(b, sizeof(b), "%d", sf->hop_limit); lv_label_set_text(r_sl_hop_lbl, b); }

    /* TX power */
    row = form_row(r_ov, "TX power");
    r_sl_tx_lbl = label(row, "auto", FONT_BODY, CY_CYAN);
    r_sl_tx = lv_slider_create(r_ov);
    style_slider(r_sl_tx);
    lv_obj_set_width(r_sl_tx, LV_PCT(100));
    lv_slider_set_range(r_sl_tx, 0, 30);
    lv_slider_set_value(r_sl_tx, sf->tx_power, LV_ANIM_OFF);
    lv_obj_add_event_cb(r_sl_tx, tx_changed, LV_EVENT_VALUE_CHANGED, NULL);
    {
        int v = sf->tx_power; char b[12];
        if (v == 0) snprintf(b, sizeof(b), "auto"); else snprintf(b, sizeof(b), "%d dBm", v);
        lv_label_set_text(r_sl_tx_lbl, b);
    }

    /* Override frequency : MHz, 0 = laisse la freq par defaut du preset.
     * Ex Gaulix : 869.4625. Vide ou 0 -> CLI met 0.0 = pas d'override. */
    r_ta_freq = settings_field(r_ov, "Override freq (MHz, 0=auto)", "", false);
    if (sf->override_freq > 0.0001f) {
        char fb[16]; snprintf(fb, sizeof(fb), "%.4f", sf->override_freq);
        lv_textarea_set_text(r_ta_freq, fb);
    }

    /* Separator */
    lv_obj_t *sep = lv_obj_create(r_ov);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(CY_BORDER), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* Presets */
    label(r_ov, "Presets", FONT_BODY, CY_AMBER);
    r_ta_name = settings_field(r_ov, "Nom du preset", "", false);

    /* Description multi-ligne : textarea propre, 3 lignes visibles + wrap. */
    label(r_ov, "Description (optionnelle)", FONT_BODY, CY_DIM);
    r_ta_desc = lv_textarea_create(r_ov);
    lv_textarea_set_one_line(r_ta_desc, false);
    lv_obj_set_size(r_ta_desc, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(r_ta_desc, lv_color_hex(CY_PANEL2), 0);
    lv_obj_set_style_text_color(r_ta_desc, lv_color_hex(CY_TEXT), 0);
    lv_obj_set_style_text_font(r_ta_desc, FONT_BODY, 0);
    lv_obj_set_style_border_color(r_ta_desc, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(r_ta_desc, 1, 0);
    lv_obj_set_style_radius(r_ta_desc, 3, 0);
    lv_obj_set_style_pad_all(r_ta_desc, 8, 0);
    lv_obj_set_style_border_color(r_ta_desc, lv_color_hex(CY_CYAN), LV_PART_CURSOR);
    lv_obj_set_style_border_width(r_ta_desc, 2, LV_PART_CURSOR);
    lv_obj_add_event_cb(r_ta_desc, set_ta_focus_e, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(r_ta_desc, set_ta_focus_e, LV_EVENT_FOCUSED, NULL);

    row = form_row(r_ov, "Charger");
    r_dd_load = lv_dropdown_create(row);
    style_dropdown(r_dd_load);
    dd_options_presets(r_dd_load);
    lv_obj_add_event_cb(r_dd_load, load_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Description du preset charge : ligne wrap sous le dropdown */
    r_desc_lbl = label(r_ov, " ", FONT_SMALL, CY_DIM);
    lv_obj_set_width(r_desc_lbl, LV_PCT(100));
    lv_label_set_long_mode(r_desc_lbl, LV_LABEL_LONG_WRAP);

    /* Scroll-into-view sur chaque textarea : evite que la TA reste cachee
     * sous le clavier lorsqu'elle prend le focus. */
    lv_obj_add_event_cb(r_ta_name, radio_ta_focus_scroll, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(r_ta_desc, radio_ta_focus_scroll, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(r_ta_freq, radio_ta_focus_scroll, LV_EVENT_FOCUSED, NULL);

    /* Boutons d'action */
    lv_obj_t *bar = lv_obj_create(r_ov);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    flat(bar);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    small_button(bar, LV_SYMBOL_SAVE   " Save",   CY_AMBER,   save_clicked);
    small_button(bar, LV_SYMBOL_TRASH  " Del",    CY_MAGENTA, delete_clicked);
    small_button(bar, LV_SYMBOL_OK     " Apply",  CY_GREEN,   apply_clicked);
    small_button(bar, LV_SYMBOL_CLOSE  " Close",  CY_DIM,     close_clicked);

    r_status = label(r_ov, "", FONT_BODY, CY_DIM);

    /* Spacer fantome en bas : reserve la hauteur du clavier (~LV_PCT(55) de
     * l'ecran) pour que le scroll-into-view puisse remonter le contenu
     * meme quand le focus est sur la TA la plus basse. */
    lv_obj_t *spacer = lv_obj_create(r_ov);
    lv_obj_set_size(spacer, LV_PCT(100), 220);
    flat(spacer);
}
