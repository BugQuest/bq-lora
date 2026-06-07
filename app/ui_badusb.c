#include "ui_common.h"
#include "ui_badusb.h"
#include "sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

/* ============================================================ */
/* App BAD USB (explorateur + selecteur mode USB + run dialog)  */
/* ============================================================ */

#define BADUSB_DIR "/home/bq-lora/bq-lora-ui/badusb"

/* ---- dialog d'execution (skull anime + statut) ---- */
static lv_obj_t *bad_run_ov, *bad_run_skull, *bad_run_status;
static lv_timer_t *bad_run_timer;
static int bad_run_frame;
static bool bad_run_active;

static const char *SKULL_FRAME_A =
"    .-=-.\n"
"   /     \\\n"
"  | O   O |\n"
"  |   v   |\n"
"   \\ === /\n"
"    '---'\n"
"    /   \\\n"
"   /  X  \\\n"
"  /   X   \\";
static const char *SKULL_FRAME_B =
"    .-=-.\n"
"   /     \\\n"
"  | -   - |\n"
"  |   v   |\n"
"   \\ === /\n"
"    '---'\n"
"    /   \\\n"
"   /  X  \\\n"
"  /   X   \\";

static void bad_run_tick(lv_timer_t *t) {
    (void)t;
    if (!bad_run_skull) return;
    bad_run_frame ^= 1;
    lv_label_set_text(bad_run_skull, bad_run_frame ? SKULL_FRAME_A : SKULL_FRAME_B);
    lv_obj_set_style_text_color(bad_run_skull,
        lv_color_hex(bad_run_frame ? CY_MAGENTA : CY_CYAN), 0);
}
static void bad_run_close_e(lv_event_t *e) {
    (void)e;
    if (bad_run_timer) { lv_timer_delete(bad_run_timer); bad_run_timer = NULL; }
    if (bad_run_ov)    { lv_obj_delete(bad_run_ov);     bad_run_ov = NULL; }
    bad_run_active = false;
}
static void bad_run_auto_close(lv_timer_t *t) { lv_timer_delete(t); bad_run_close_e(NULL); }
static void bad_run_done_cb(bool ok, void *user) {
    (void)user;
    if (!bad_run_status) return;
    lv_label_set_text(bad_run_status, ok ? tr(STR_BADUSB_DONE) : tr(STR_BADUSB_FAILED));
    lv_obj_set_style_text_color(bad_run_status,
        lv_color_hex(ok ? CY_GREEN : CY_MAGENTA), 0);
    if (bad_run_timer) { lv_timer_delete(bad_run_timer); bad_run_timer = NULL; }
    lv_timer_create(bad_run_auto_close, 1500, NULL);
}
static void bad_run_open(const char *path, const char *name) {
    if (bad_run_active) return;
    bad_run_active = true;
    bad_run_frame = 0;
    bad_run_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(bad_run_ov, 0, 0);
    lv_obj_set_size(bad_run_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bad_run_ov, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(bad_run_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bad_run_ov, 0, 0);
    lv_obj_set_style_radius(bad_run_ov, 0, 0);
    lv_obj_set_style_pad_all(bad_run_ov, 0, 0);
    lv_obj_clear_flag(bad_run_ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = label(bad_run_ov, tr(STR_BADUSB_HEADER), FONT_MONO, CY_MAGENTA);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 10);
    bad_run_skull = label(bad_run_ov, SKULL_FRAME_A, FONT_MONO, CY_MAGENTA);
    lv_obj_set_style_text_line_space(bad_run_skull, 2, 0);
    lv_obj_align(bad_run_skull, LV_ALIGN_CENTER, 0, -20);

    char nb[80]; snprintf(nb, sizeof(nb), tr(STR_FMT_EXEC), name);
    lv_obj_t *nm = label(bad_run_ov, nb, FONT_SMALL, CY_DIM);
    lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -70);

    bad_run_status = label(bad_run_ov, tr(STR_BADUSB_RUNNING), FONT_BODY, CY_CYAN);
    lv_obj_align(bad_run_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t *bar = lv_obj_create(bad_run_ov);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    flat(bar); lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    {
        char fb[24]; snprintf(fb, sizeof(fb), LV_SYMBOL_CLOSE "  %s", tr(STR_CLOSE));
        small_button(bar, fb, CY_DIM, bad_run_close_e);
    }
    bad_run_timer = lv_timer_create(bad_run_tick, 350, NULL);
    sys_badusb_run_async(path, bad_run_done_cb, NULL);
}

/* ---- explorateur de fichiers ---- */
typedef struct { char path[256]; char name[64]; bool is_dir; } badusb_entry_t;
static badusb_entry_t bad_entries[32];
static int bad_entries_n = 0;
static char bap_cwd[256] = BADUSB_DIR;
static lv_obj_t *bap_list_obj, *bap_lbl_path;
static lv_obj_t *bap_lbl_state;
static lv_obj_t *bap_btn_ncm, *bap_btn_hid, *bap_btn_storage;

static int bap_entry_cmp(const void *a, const void *b) {
    const badusb_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcmp(ea->name, eb->name);
}
static void bap_refresh_list(void);
static void bap_parent_cb(lv_event_t *e) {
    (void)e;
    if (strcmp(bap_cwd, BADUSB_DIR) == 0) return;
    char *p = strrchr(bap_cwd, '/');
    if (p && p > bap_cwd) *p = 0;
    if (strncmp(bap_cwd, BADUSB_DIR, strlen(BADUSB_DIR)) != 0)
        strcpy(bap_cwd, BADUSB_DIR);
    bap_refresh_list();
}
static void bap_entry_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= bad_entries_n) return;
    badusb_entry_t *en = &bad_entries[idx];
    if (en->is_dir) {
        strncpy(bap_cwd, en->path, sizeof(bap_cwd) - 1);
        bap_cwd[sizeof(bap_cwd) - 1] = 0;
        bap_refresh_list();
        return;
    }
    if (sys_usb_mode() != USB_MODE_HID) {
        confirm_dialog(tr(STR_CONFIRM_KBD_FIRST), NULL);
        return;
    }
    bad_run_open(en->path, en->name);
}
static void bap_refresh_list(void) {
    if (!bap_list_obj) return;
    lv_obj_clean(bap_list_obj);
    bad_entries_n = 0;
    DIR *d = opendir(bap_cwd);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && bad_entries_n < (int)(sizeof(bad_entries)/sizeof(bad_entries[0]))) {
            if (de->d_name[0] == '.') continue;
            if (strcasecmp(de->d_name, "System Volume Information") == 0 ||
                strcasecmp(de->d_name, "$RECYCLE.BIN")               == 0 ||
                strcasecmp(de->d_name, "RECYCLER")                   == 0 ||
                strcasecmp(de->d_name, "desktop.ini")                == 0 ||
                strcasecmp(de->d_name, "Thumbs.db")                  == 0 ||
                strcasecmp(de->d_name, ".Trashes")                   == 0 ||
                strcasecmp(de->d_name, ".Spotlight-V100")            == 0 ||
                strcasecmp(de->d_name, ".fseventsd")                 == 0)
                continue;
            badusb_entry_t *e = &bad_entries[bad_entries_n];
            snprintf(e->path, sizeof(e->path), "%s/%s", bap_cwd, de->d_name);
            strncpy(e->name, de->d_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = 0;
            struct stat st;
            e->is_dir = (stat(e->path, &st) == 0 && S_ISDIR(st.st_mode));
            bad_entries_n++;
        }
        closedir(d);
    }
    qsort(bad_entries, bad_entries_n, sizeof(badusb_entry_t), bap_entry_cmp);

    if (bap_lbl_path) {
        const char *rel = bap_cwd + strlen(BADUSB_DIR);
        if (!*rel) rel = "/";
        char buf[80]; snprintf(buf, sizeof(buf), "%s", rel);
        lv_label_set_text(bap_lbl_path, buf);
    }

    if (strcmp(bap_cwd, BADUSB_DIR) != 0) {
        lv_obj_t *up = lv_button_create(bap_list_obj);
        lv_obj_set_size(up, LV_PCT(100), 32);
        lv_obj_set_style_radius(up, 2, 0);
        lv_obj_set_style_bg_opa(up, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(up, lv_color_hex(CY_DIM), 0);
        lv_obj_set_style_border_width(up, 1, 0);
        lv_obj_set_style_border_color(up, lv_color_hex(CY_DIM), 0);
        lv_obj_set_style_shadow_width(up, 0, 0);
        lv_obj_add_event_cb(up, bap_parent_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ul = label(up, LV_SYMBOL_LEFT "  ..", FONT_BODY, CY_TEXT);
        lv_obj_align(ul, LV_ALIGN_LEFT_MID, 8, 0);
    }

    if (bad_entries_n == 0 && strcmp(bap_cwd, BADUSB_DIR) == 0) {
        label(bap_list_obj, tr(STR_BADUSB_DIR_EMPTY), FONT_SMALL, CY_DIM);
        return;
    }
    for (int i = 0; i < bad_entries_n; i++) {
        badusb_entry_t *en = &bad_entries[i];
        lv_obj_t *r = lv_button_create(bap_list_obj);
        lv_obj_set_size(r, LV_PCT(100), 32);
        lv_obj_set_style_radius(r, 2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_20, 0);
        uint32_t col = en->is_dir ? CY_AMBER : CY_MAGENTA;
        lv_obj_set_style_bg_color(r, lv_color_hex(col), 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(col), 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_add_event_cb(r, bap_entry_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char txt[96];
        snprintf(txt, sizeof(txt), "%s  %s",
                 en->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                 en->name);
        lv_obj_t *rl = label(r, txt, FONT_BODY, CY_TEXT);
        lv_obj_align(rl, LV_ALIGN_LEFT_MID, 8, 0);
        if (!en->is_dir) {
            lv_obj_t *rr = label(r, tr(STR_BADUSB_RUN), FONT_SMALL, CY_TEXT);
            lv_obj_align(rr, LV_ALIGN_RIGHT_MID, -8, 0);
        }
    }
}

/* ---- selecteur 3 modes USB ---- */
static int usb_mode_target;
static void usb_mode_switch_done(bool ok, void *user) { (void)ok; (void)user; }
static void usb_mode_yes(void) {
    sys_usb_mode_set_async((usb_mode_t)usb_mode_target, usb_mode_switch_done, NULL);
}
static void usb_mode_btn_ncm_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_mode() == USB_MODE_NCM) return;
    usb_mode_target = USB_MODE_NCM;
    confirm_dialog(tr(STR_CONFIRM_MODE_NET), usb_mode_yes);
}
static void usb_mode_btn_hid_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_mode() == USB_MODE_HID) return;
    usb_mode_target = USB_MODE_HID;
    confirm_dialog(tr(STR_CONFIRM_MODE_KBD), usb_mode_yes);
}
static void usb_mode_btn_storage_cb(lv_event_t *e) {
    (void)e;
    if (sys_usb_mode() == USB_MODE_STORAGE) return;
    usb_mode_target = USB_MODE_STORAGE;
    confirm_dialog(tr(STR_CONFIRM_MODE_STORAGE), usb_mode_yes);
}

/* ---- refresh periodique (statut + highlight du mode actif) ---- */
static void bap_refresh(lv_timer_t *t) {
    (void)t;
    if (!bap_lbl_state) return;
    usb_mode_t m = sys_usb_mode();
    const char *s = (m == USB_MODE_HID)     ? tr(STR_USB_MODE_KBD_ACTIVE)
                  : (m == USB_MODE_STORAGE) ? tr(STR_USB_MODE_STORAGE_ACTIVE)
                  : (m == USB_MODE_NCM)     ? tr(STR_USB_MODE_NET_ACTIVE) : "?";
    uint32_t col = (m == USB_MODE_HID)     ? CY_MAGENTA
                 : (m == USB_MODE_STORAGE) ? CY_AMBER
                 : (m == USB_MODE_NCM)     ? CY_CYAN  : CY_DIM;
    lv_label_set_text(bap_lbl_state, s);
    lv_obj_set_style_text_color(bap_lbl_state, lv_color_hex(col), 0);

    if (bap_btn_ncm)
        lv_obj_set_style_bg_opa(bap_btn_ncm,     m == USB_MODE_NCM     ? LV_OPA_80 : LV_OPA_20, 0);
    if (bap_btn_hid)
        lv_obj_set_style_bg_opa(bap_btn_hid,     m == USB_MODE_HID     ? LV_OPA_80 : LV_OPA_20, 0);
    if (bap_btn_storage)
        lv_obj_set_style_bg_opa(bap_btn_storage, m == USB_MODE_STORAGE ? LV_OPA_80 : LV_OPA_20, 0);
}

void ui_badusb_reset(void) {
    bap_lbl_state = NULL;
    bap_btn_ncm = bap_btn_hid = bap_btn_storage = NULL;
    bap_list_obj = bap_lbl_path = NULL;
    /* le dialog d'exec est sur lv_layer_top : independant du content, on
     * ne le touche pas ici (il a son propre lifecycle). */
}

void ui_badusb_build(void) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    label(col, tr(STR_BADUSB_TITLE), FONT_BIG, CY_MAGENTA);
    bap_lbl_state = label(col, "?", FONT_BODY, CY_DIM);

    /* selecteur 3 modes USB */
    lv_obj_t *mr = lv_obj_create(col);
    lv_obj_set_size(mr, LV_PCT(100), LV_SIZE_CONTENT);
    flat(mr); lv_obj_set_flex_flow(mr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mr, 4, 0);
    lv_obj_clear_flag(mr, LV_OBJ_FLAG_SCROLLABLE);
    bap_btn_ncm     = small_button(mr, tr(STR_BTN_USB_MODE_NET),     CY_CYAN,    usb_mode_btn_ncm_cb);
    bap_btn_hid     = small_button(mr, tr(STR_BTN_USB_MODE_KBD),     CY_MAGENTA, usb_mode_btn_hid_cb);
    bap_btn_storage = small_button(mr, tr(STR_BTN_USB_MODE_STORAGE), CY_AMBER,   usb_mode_btn_storage_cb);

    /* explorateur de l'arbo BADUSB */
    label(col, tr(STR_BADUSB_SCRIPTS), FONT_SMALL, CY_DIM);
    bap_lbl_path = label(col, "/", FONT_MONO, CY_AMBER);
    lv_obj_set_width(bap_lbl_path, LV_PCT(100));
    lv_label_set_long_mode(bap_lbl_path, LV_LABEL_LONG_DOT);

    bap_list_obj = lv_obj_create(col);
    lv_obj_set_size(bap_list_obj, LV_PCT(100), LV_PCT(100));
    flat(bap_list_obj);
    lv_obj_set_flex_flow(bap_list_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bap_list_obj, 4, 0);
    lv_obj_set_scroll_dir(bap_list_obj, LV_DIR_VER);

    strcpy(bap_cwd, BADUSB_DIR);
    bap_refresh_list();

    bap_refresh(NULL);
    sys_refresh_timer = lv_timer_create(bap_refresh, 3000, NULL);
}
