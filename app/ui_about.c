#include "ui_common.h"
#include "ui_about.h"
#include "settings.h"

/* ---- App "A propos" : identite + HW + SW + projet ---- */

static void about_kv(lv_obj_t *parent, const char *k, const char *v) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    label(row, k, FONT_SMALL, CY_DIM);
    lv_obj_t *val = label(row, v, FONT_SMALL, CY_TEXT);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
}

void ui_about_build(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    label(col, tr(STR_ABOUT_TITLE), FONT_BIG, CY_CYAN);
    label(col, "/ / L O R A", FONT_BODY, CY_MAGENTA);
    label(col, tr(STR_ABOUT_VERSION), FONT_SMALL, CY_DIM);

    /* materiel */
    lv_obj_t *hw = lv_obj_create(col);
    lv_obj_set_size(hw, LV_PCT(100), LV_SIZE_CONTENT);
    panel(hw, CY_BORDER);
    lv_obj_set_flex_flow(hw, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hw, 4, 0);
    lv_obj_clear_flag(hw, LV_OBJ_FLAG_SCROLLABLE);
    label(hw, tr(STR_ABOUT_HW), FONT_SMALL, CY_AMBER);
    about_kv(hw, "Carte",  "Pi Zero 2 W");
    about_kv(hw, "Ecran",  "MKS TS35-R / ILI9486");
    about_kv(hw, "Tactile","XPT2046");
    about_kv(hw, "Radio",  "SX1262 868 MHz");

    /* logiciel */
    lv_obj_t *sw = lv_obj_create(col);
    lv_obj_set_size(sw, LV_PCT(100), LV_SIZE_CONTENT);
    panel(sw, CY_BORDER);
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sw, 4, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    label(sw, tr(STR_ABOUT_SW), FONT_SMALL, CY_AMBER);
    about_kv(sw, "UI",    "LVGL 9.2 (fbdev)");
    about_kv(sw, "Mesh",  "meshtasticd");
    about_kv(sw, "Noeud", settings_node_name());

    /* projet */
    lv_obj_t *pr = lv_obj_create(col);
    lv_obj_set_size(pr, LV_PCT(100), LV_SIZE_CONTENT);
    panel(pr, CY_BORDER);
    lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pr, 4, 0);
    lv_obj_clear_flag(pr, LV_OBJ_FLAG_SCROLLABLE);
    label(pr, tr(STR_ABOUT_PROJECT), FONT_SMALL, CY_AMBER);
    about_kv(pr, "Auteur", "BugQuest");
    lv_obj_t *gh = label(pr, "github.com/BugQuest/bq-lora", FONT_SMALL, CY_CYAN);
    lv_obj_set_width(gh, LV_PCT(100));
    lv_label_set_long_mode(gh, LV_LABEL_LONG_DOT);
}
