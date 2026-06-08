#include "ui_common.h"
#include "ui_dialog.h"
#include <stdio.h>

/* ============================================================ */
/* Helpers de dialogue : loading / info / warning / error / confirm */
/* ============================================================ */

static lv_obj_t *s_overlay;          /* backdrop plein ecran (lv_layer_top) */
static lv_obj_t *s_loading;          /* loading garde a part : ne se ferme que via _hide */
static void   (*s_on_yes)(void);     /* callback Confirm (NULL si autre type) */

/* ------------------------------------------------------------ helpers */

static void overlay_close(void)
{
    if (s_overlay) { lv_obj_delete(s_overlay); s_overlay = NULL; }
    s_on_yes = NULL;
}

static void btn_close_e(lv_event_t *e) { (void)e; overlay_close(); }

static void btn_yes_e(lv_event_t *e)
{
    (void)e;
    void (*cb)(void) = s_on_yes;
    overlay_close();
    if (cb) cb();
}

/* Construit le backdrop semi-opaque + bloque les taps derriere.
 * Detruit la boite precedente si elle est encore presente. */
static lv_obj_t *overlay_new(void)
{
    if (s_overlay) overlay_close();
    s_loading = NULL;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE); /* mange les taps */
    lv_obj_move_foreground(s_overlay);
    return s_overlay;
}

/* Carte centrale avec bordure de la couleur passee.
 * Note : LV_FLEX_ALIGN_CENTER sur l'axe principal + LV_SIZE_CONTENT pour la
 * hauteur sont incompatibles (lvgl ne peut pas centrer dans un conteneur qui
 * s'adapte au contenu) et provoquent une cascade vers la taille de l'overlay
 * plein ecran. On utilise START sur l'axe principal et on cap la hauteur. */
static lv_obj_t *card_new(uint32_t border_color)
{
    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_set_width(card, LV_PCT(85));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(80), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(border_color), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    /* START sur axe principal (vertical) : empile depuis le haut, hauteur
     * = sum(children). CENTER sur axe cross (horizontal) : centre les enfants. */
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/* Titre avec icone + couleur. */
static void title_add(lv_obj_t *card, const char *icon, const char *txt, uint32_t color)
{
    char b[64];
    snprintf(b, sizeof(b), "%s %s", icon, txt);
    lv_obj_t *l = label(card, b, FONT_BIG, color);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

/* Message wrap, centre. */
static void message_add(lv_obj_t *card, const char *msg)
{
    lv_obj_t *l = label(card, msg ? msg : "", FONT_BODY, CY_TEXT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

/* ------------------------------------------------------------ Loading */

void ui_dialog_loading_show(const char *msg)
{
    overlay_new();
    lv_obj_t *card = card_new(CY_CYAN);
    /* Spinner LVGL v9 : arc anime, on impose la couleur de l'indicateur. */
    lv_obj_t *sp = lv_spinner_create(card);
    lv_obj_set_size(sp, 56, 56);
    lv_obj_set_style_arc_color(sp, lv_color_hex(CY_BORDER),  LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_color_hex(CY_CYAN),    LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
    message_add(card, msg ? msg : "Chargement...");
    s_loading = card;
    /* PAS de lv_refr_now() ici : appele depuis un event handler, ca re-entre
     * dans la pipeline de rendu LVGL et provoque un leak progressif (VIRT
     * monte de 12 MB a 80+ MB en quelques minutes, CPU finit a 100% -> freeze).
     * Pour les ops longues blocantes, defere via lv_timer (one-shot) au lieu
     * de forcer le rendu inline. */
}

void ui_dialog_loading_hide(void)
{
    /* Ne ferme que si c'est bien un loading actif (evite de fermer une boite
     * info/error qui aurait ete posee par-dessus). */
    if (s_loading && s_overlay) overlay_close();
}

/* ------------------------------------------------------------ Info / Warning / Error */

/* Row horizontale pour boutons : small_button utilise flex_grow=1 qui suppose
 * un parent flex ROW. Dans la card (flex COLUMN) ca le faisait grandir
 * verticalement avec largeur naturelle -> texte vertical. */
static lv_obj_t *button_row(lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void single_button_dialog(const char *icon, const char *title,
                                 uint32_t color, const char *msg)
{
    overlay_new();
    lv_obj_t *card = card_new(color);
    title_add(card, icon, title, color);
    message_add(card, msg);
    lv_obj_t *row = button_row(card);
    small_button(row, "OK", color, btn_close_e);
}

void ui_dialog_info(const char *msg)
{
    single_button_dialog(LV_SYMBOL_OK,      "Info",     CY_CYAN,    msg);
}

void ui_dialog_warning(const char *msg)
{
    single_button_dialog(LV_SYMBOL_WARNING, "Attention", CY_AMBER,  msg);
}

void ui_dialog_error(const char *msg)
{
    single_button_dialog(LV_SYMBOL_CLOSE,   "Erreur",    CY_MAGENTA, msg);
}

/* ------------------------------------------------------------ Confirm */

void ui_dialog_confirm(const char *msg, void (*on_yes)(void))
{
    s_on_yes = on_yes;
    overlay_new();
    /* set apres overlay_new car overlay_new() reset s_on_yes. */
    s_on_yes = on_yes;
    lv_obj_t *card = card_new(CY_AMBER);
    title_add(card, LV_SYMBOL_WARNING, "Confirmer", CY_AMBER);
    message_add(card, msg);

    lv_obj_t *bar = button_row(card);
    small_button(bar, LV_SYMBOL_OK    " Oui", CY_GREEN,   btn_yes_e);
    small_button(bar, LV_SYMBOL_CLOSE " Non", CY_MAGENTA, btn_close_e);
}

/* ------------------------------------------------------------ Choice (2 actions + cancel) */

static void (*s_on_cb1)(void);
static void (*s_on_cb2)(void);

static void btn_cb1_e(lv_event_t *e)
{
    (void)e;
    void (*cb)(void) = s_on_cb1;
    overlay_close();
    s_on_cb1 = NULL; s_on_cb2 = NULL;
    if (cb) cb();
}
static void btn_cb2_e(lv_event_t *e)
{
    (void)e;
    void (*cb)(void) = s_on_cb2;
    overlay_close();
    s_on_cb1 = NULL; s_on_cb2 = NULL;
    if (cb) cb();
}
static void btn_cancel_e(lv_event_t *e)
{
    (void)e;
    overlay_close();
    s_on_cb1 = NULL; s_on_cb2 = NULL;
}

void ui_dialog_choice(const char *msg,
                      const char *btn1_txt, void (*cb1)(void),
                      const char *btn2_txt, void (*cb2)(void))
{
    s_on_cb1 = cb1;
    s_on_cb2 = cb2;
    overlay_new();
    s_on_cb1 = cb1; s_on_cb2 = cb2; /* overlay_close() dans overlay_new a reset */
    lv_obj_t *card = card_new(CY_AMBER);
    title_add(card, LV_SYMBOL_POWER, "Action", CY_AMBER);
    message_add(card, msg);

    lv_obj_t *bar = button_row(card);
    small_button(bar, btn1_txt ? btn1_txt : "1", CY_MAGENTA, btn_cb1_e);
    small_button(bar, btn2_txt ? btn2_txt : "2", CY_AMBER,   btn_cb2_e);
    small_button(bar, LV_SYMBOL_CLOSE,           CY_DIM,     btn_cancel_e);
}
