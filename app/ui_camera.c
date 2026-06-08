#include "ui_common.h"
#include "ui_camera.h"
#include "ui_dialog.h"
#include "sys.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================ */
/* App CAMERA (viewfinder live + capture HD)                    */
/* App GALERIE (parcours + suppression + mode navigation zoom)   */
/* Detache de ui.c -- statics file-local                         */
/* ============================================================ */

/* ----- Camera (viewfinder + capture) ----- */
#define CAM_PV_W 256
#define CAM_PV_H 192
static uint8_t   cam_pv_buf[CAM_PV_W * CAM_PV_H * 2] __attribute__((aligned(4)));
static lv_obj_t *cam_canvas, *cam_status, *cam_btn;
static bool      cam_busy;

/* avancement vers show_tab : pour basculer vers la galerie */
extern void show_tab(int app);
enum { APP_GALLERY_ID = 10 };   /* doit matcher ui.c -- duplique localement */

static void cam_frame_cb(void *user) {
    (void)user;
    if (cam_canvas) lv_obj_invalidate(cam_canvas);
}

static void cam_stream_resume(void) {
    if (cam_canvas && !sys_cam_stream_active())
        sys_cam_stream_start(cam_pv_buf, CAM_PV_W, CAM_PV_H, cam_frame_cb, NULL);
}

static void cam_capture_done(bool ok, const char *photo, const char *preview, void *user) {
    (void)preview; (void)user;
    cam_busy = false;
    ui_dialog_loading_hide();
    if (cam_btn) lv_obj_clear_state(cam_btn, LV_STATE_DISABLED);
    if (cam_status) {
        if (ok) {
            const char *base = strrchr(photo, '/');
            char b[96]; snprintf(b, sizeof(b), LV_SYMBOL_OK "  %s", base ? base + 1 : photo);
            lv_label_set_text(cam_status, b);
            lv_obj_set_style_text_color(cam_status, lv_color_hex(CY_GREEN), 0);
        } else {
            char ce[40]; snprintf(ce, sizeof(ce), LV_SYMBOL_WARNING "%s", tr(STR_CAM_CAPTURE_FAILED));
            lv_label_set_text(cam_status, ce);
            lv_obj_set_style_text_color(cam_status, lv_color_hex(CY_MAGENTA), 0);
        }
    }
    if (ok) {
        const char *base = photo ? strrchr(photo, '/') : NULL;
        char m[128]; snprintf(m, sizeof(m), "Photo enregistree : %s", base ? base + 1 : (photo ? photo : "?"));
        ui_dialog_info(m);
    } else {
        ui_dialog_error(tr(STR_CAM_CAPTURE_FAILED));
    }
    cam_stream_resume();
}

static void cam_gallery_cb(lv_event_t *e) {
    (void)e;
    if (cam_busy) return;
    show_tab(APP_GALLERY_ID);
}

static void cam_capture_cb(lv_event_t *e) {
    (void)e;
    if (cam_busy) return;
    cam_busy = true;
    if (cam_btn) lv_obj_add_state(cam_btn, LV_STATE_DISABLED);
    if (cam_status) {
        char ch[40]; snprintf(ch, sizeof(ch), LV_SYMBOL_REFRESH "%s", tr(STR_CAM_HD_CAPTURE));
        lv_label_set_text(cam_status, ch);
        lv_obj_set_style_text_color(cam_status, lv_color_hex(CY_AMBER), 0);
    }
    sys_cam_stream_stop();
    ui_dialog_loading_show(tr(STR_CAM_HD_CAPTURE));
    sys_cam_capture_async(CAM_PV_W, CAM_PV_H, cam_capture_done, NULL);
}

void ui_camera_reset(void) {
    cam_canvas = NULL;
    cam_status = NULL;
    cam_btn = NULL;
}
void ui_camera_stream_stop(void) {
    sys_cam_stream_stop();
}

void ui_camera_build(void) {
    cam_canvas = NULL; cam_status = NULL; cam_btn = NULL;

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    label(col, tr(STR_CAMERA_TITLE), FONT_BIG, CY_GREEN);

    lv_obj_t *frame = lv_obj_create(col);
    lv_obj_set_size(frame, CAM_PV_W + 6, CAM_PV_H + 6);
    panel(frame, CY_BORDER);
    lv_obj_set_style_pad_all(frame, 2, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    cam_canvas = lv_canvas_create(frame);
    lv_canvas_set_buffer(cam_canvas, cam_pv_buf, CAM_PV_W, CAM_PV_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(cam_canvas);
    lv_canvas_fill_bg(cam_canvas, lv_color_black(), LV_OPA_COVER);

    cam_status = label(col, tr(STR_CAM_LIVE), FONT_SMALL, CY_DIM);
    lv_obj_set_width(cam_status, LV_PCT(100));
    lv_label_set_long_mode(cam_status, LV_LABEL_LONG_DOT);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    {
        char b1[32], b2[32];
        snprintf(b1, sizeof(b1), LV_SYMBOL_IMAGE "%s", tr(STR_CAM_CAPTURE));
        snprintf(b2, sizeof(b2), LV_SYMBOL_LIST  "%s", tr(STR_CAM_GALLERY));
        cam_btn = small_button(row, b1, CY_GREEN, cam_capture_cb);
        small_button(row, b2, CY_CYAN, cam_gallery_cb);
    }

    cam_busy = false;
    sys_cam_stream_start(cam_pv_buf, CAM_PV_W, CAM_PV_H, cam_frame_cb, NULL);
}

/* ============================================================ */
/* App GALERIE                                                  */
/* ============================================================ */

#define GAL_MAX 128
static char      gal_paths[GAL_MAX][256];
static int       gal_n, gal_idx;
static uint8_t   gal_buf[CAM_PV_W * CAM_PV_H * 2] __attribute__((aligned(4)));
static lv_obj_t *gal_canvas, *gal_status, *gal_del_btn, *gal_prev_btn, *gal_next_btn;
static lv_obj_t *gal_frame, *gal_hd_canvas, *gal_nav_btn;
static lv_obj_t *gal_browse_row, *gal_nav_row, *gal_zoom_lbl;

/* ---- Mode navigation (zoom + deplacement) ---- */
#define CAM_HD_W 768
#define CAM_HD_H 576
static uint8_t   gal_hd_buf[CAM_HD_W * CAM_HD_H * 2] __attribute__((aligned(4)));
#define GAL_FIT_SCALE   ((CAM_PV_W * 256) / CAM_HD_W)
static bool      gal_nav_mode;
static int       gal_zoom;
static int       gal_ox, gal_oy;

static int gal_view_w(void) { return (CAM_HD_W * gal_zoom) / 256; }
static int gal_view_h(void) { return (CAM_HD_H * gal_zoom) / 256; }

static void gal_clamp_pan(void) {
    int vw = gal_view_w(), vh = gal_view_h();
    int min_x = CAM_PV_W - vw, min_y = CAM_PV_H - vh;
    if (vw <= CAM_PV_W) gal_ox = min_x / 2;
    else { if (gal_ox > 0) gal_ox = 0; if (gal_ox < min_x) gal_ox = min_x; }
    if (vh <= CAM_PV_H) gal_oy = min_y / 2;
    else { if (gal_oy > 0) gal_oy = 0; if (gal_oy < min_y) gal_oy = min_y; }
}

static void gal_apply_transform(void) {
    if (!gal_hd_canvas) return;
    lv_obj_set_style_transform_pivot_x(gal_hd_canvas, 0, 0);
    lv_obj_set_style_transform_pivot_y(gal_hd_canvas, 0, 0);
    lv_obj_set_style_transform_scale_x(gal_hd_canvas, gal_zoom, 0);
    lv_obj_set_style_transform_scale_y(gal_hd_canvas, gal_zoom, 0);
    lv_obj_set_pos(gal_hd_canvas, gal_ox, gal_oy);
    if (gal_zoom_lbl) {
        int pct = (gal_zoom * 100 + GAL_FIT_SCALE / 2) / GAL_FIT_SCALE;
        char b[16]; snprintf(b, sizeof(b), "x%d.%d", pct / 100, (pct % 100) / 10);
        lv_label_set_text(gal_zoom_lbl, b);
    }
}

static void gal_hd_done(bool ok, const char *preview, void *user) {
    (void)user;
    if (!gal_hd_canvas) return;
    if (ok) {
        FILE *f = fopen(preview, "rb");
        if (f) {
            size_t got = fread(gal_hd_buf, 1, sizeof(gal_hd_buf), f);
            fclose(f);
            if (got == sizeof(gal_hd_buf)) { lv_obj_invalidate(gal_hd_canvas); return; }
        }
    }
    lv_canvas_fill_bg(gal_hd_canvas, lv_color_black(), LV_OPA_COVER);
}

static void gal_set_nav_mode(bool on) {
    gal_nav_mode = on;
    if (gal_browse_row) { if (on) lv_obj_add_flag(gal_browse_row, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_clear_flag(gal_browse_row, LV_OBJ_FLAG_HIDDEN); }
    if (gal_nav_row)    { if (on) lv_obj_clear_flag(gal_nav_row, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_add_flag(gal_nav_row, LV_OBJ_FLAG_HIDDEN); }
    if (gal_canvas)     { if (on) lv_obj_add_flag(gal_canvas, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_clear_flag(gal_canvas, LV_OBJ_FLAG_HIDDEN); }
    if (gal_hd_canvas)  { if (on) lv_obj_clear_flag(gal_hd_canvas, LV_OBJ_FLAG_HIDDEN);
                          else    lv_obj_add_flag(gal_hd_canvas, LV_OBJ_FLAG_HIDDEN); }
    if (on && gal_n > 0 && gal_hd_canvas) {
        gal_zoom = GAL_FIT_SCALE;
        gal_ox = gal_oy = 0;
        gal_clamp_pan();
        gal_apply_transform();
        lv_canvas_fill_bg(gal_hd_canvas, lv_color_black(), LV_OPA_COVER);
        sys_cam_preview_async(gal_paths[gal_idx], CAM_HD_W, CAM_HD_H, gal_hd_done, NULL);
    }
}

static void gal_nav_enter_cb(lv_event_t *e) { (void)e; if (gal_n > 0) gal_set_nav_mode(true); }
static void gal_nav_exit_cb (lv_event_t *e) { (void)e; gal_set_nav_mode(false); }

static void gal_zoom_apply(int new_zoom) {
    if (new_zoom < GAL_FIT_SCALE) new_zoom = GAL_FIT_SCALE;
    if (new_zoom > 256)            new_zoom = 256;
    int cx = CAM_PV_W / 2, cy = CAM_PV_H / 2;
    int px = cx - gal_ox, py = cy - gal_oy;
    int npx = (px * new_zoom) / (gal_zoom ? gal_zoom : 1);
    int npy = (py * new_zoom) / (gal_zoom ? gal_zoom : 1);
    gal_ox = cx - npx;
    gal_oy = cy - npy;
    gal_zoom = new_zoom;
    gal_clamp_pan();
    gal_apply_transform();
}
static void gal_zoom_in_cb   (lv_event_t *e) { (void)e; gal_zoom_apply(gal_zoom * 3 / 2); }
static void gal_zoom_out_cb  (lv_event_t *e) { (void)e; gal_zoom_apply(gal_zoom * 2 / 3); }
static void gal_zoom_reset_cb(lv_event_t *e) {
    (void)e;
    gal_zoom = GAL_FIT_SCALE; gal_ox = gal_oy = 0;
    gal_clamp_pan(); gal_apply_transform();
}

static void gal_frame_drag_cb(lv_event_t *e) {
    if (!gal_nav_mode) return;
    if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
    lv_indev_t *id = lv_indev_active();
    if (!id) return;
    lv_point_t v; lv_indev_get_vect(id, &v);
    if (v.x == 0 && v.y == 0) return;
    gal_ox += v.x; gal_oy += v.y;
    gal_clamp_pan();
    gal_apply_transform();
}

static void gal_set_nav(bool on) {
    if (gal_prev_btn) { if (on) lv_obj_clear_state(gal_prev_btn, LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_prev_btn, LV_STATE_DISABLED); }
    if (gal_next_btn) { if (on) lv_obj_clear_state(gal_next_btn, LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_next_btn, LV_STATE_DISABLED); }
    if (gal_del_btn)  { if (on) lv_obj_clear_state(gal_del_btn,  LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_del_btn,  LV_STATE_DISABLED); }
    if (gal_nav_btn)  { if (on) lv_obj_clear_state(gal_nav_btn,  LV_STATE_DISABLED);
                        else    lv_obj_add_state(gal_nav_btn,  LV_STATE_DISABLED); }
}

static void gal_preview_done(bool ok, const char *preview, void *user) {
    (void)user;
    if (!gal_canvas) return;
    if (ok) {
        FILE *f = fopen(preview, "rb");
        if (f) {
            size_t got = fread(gal_buf, 1, sizeof(gal_buf), f);
            fclose(f);
            if (got == sizeof(gal_buf)) { lv_obj_invalidate(gal_canvas); return; }
        }
    }
    lv_canvas_fill_bg(gal_canvas, lv_color_black(), LV_OPA_COVER);
}

static void gal_show(int idx) {
    if (gal_n <= 0) {
        if (gal_canvas) lv_canvas_fill_bg(gal_canvas, lv_color_black(), LV_OPA_COVER);
        if (gal_status) {
            lv_label_set_text(gal_status, tr(STR_GAL_EMPTY));
            lv_obj_set_style_text_color(gal_status, lv_color_hex(CY_DIM), 0);
        }
        gal_set_nav(false);
        return;
    }
    if (idx < 0)        idx = gal_n - 1;
    else if (idx >= gal_n) idx = 0;
    gal_idx = idx;

    const char *p = gal_paths[gal_idx];
    const char *base = strrchr(p, '/');
    char b[96];
    snprintf(b, sizeof(b), "%s  (%d/%d)", base ? base + 1 : p, gal_idx + 1, gal_n);
    if (gal_status) {
        lv_label_set_text(gal_status, b);
        lv_obj_set_style_text_color(gal_status, lv_color_hex(CY_TEXT), 0);
    }
    gal_set_nav(true);
    sys_cam_preview_async(p, CAM_PV_W, CAM_PV_H, gal_preview_done, NULL);
}

static void gal_prev_cb(lv_event_t *e) { (void)e; if (gal_nav_mode) gal_set_nav_mode(false); gal_show(gal_idx - 1); }
static void gal_next_cb(lv_event_t *e) { (void)e; if (gal_nav_mode) gal_set_nav_mode(false); gal_show(gal_idx + 1); }

static void gal_delete_yes(void) {
    if (gal_n <= 0 || !gal_canvas) return;
    sys_cam_photo_delete(gal_paths[gal_idx]);
    gal_n = sys_cam_photo_list(gal_paths, GAL_MAX);
    gal_show(gal_idx);
}
static void gal_del_cb(lv_event_t *e) {
    (void)e;
    if (gal_n <= 0) return;
    confirm_dialog(tr(STR_GAL_DELETE_CONFIRM), gal_delete_yes);
}

void ui_gallery_reset(void) {
    gal_canvas = NULL; gal_status = NULL;
    gal_del_btn = NULL; gal_prev_btn = NULL; gal_next_btn = NULL;
    gal_frame = NULL; gal_hd_canvas = NULL; gal_nav_btn = NULL;
    gal_browse_row = NULL; gal_nav_row = NULL; gal_zoom_lbl = NULL;
}

void ui_gallery_build(void) {
    ui_gallery_reset();

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    flat(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    label(col, tr(STR_GAL_TITLE), FONT_BIG, CY_CYAN);

    gal_frame = lv_obj_create(col);
    lv_obj_set_size(gal_frame, CAM_PV_W + 6, CAM_PV_H + 6);
    panel(gal_frame, CY_BORDER);
    lv_obj_set_style_pad_all(gal_frame, 2, 0);
    lv_obj_clear_flag(gal_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gal_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gal_frame, gal_frame_drag_cb, LV_EVENT_PRESSING, NULL);

    gal_canvas = lv_canvas_create(gal_frame);
    lv_canvas_set_buffer(gal_canvas, gal_buf, CAM_PV_W, CAM_PV_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(gal_canvas);
    lv_canvas_fill_bg(gal_canvas, lv_color_black(), LV_OPA_COVER);

    gal_hd_canvas = lv_canvas_create(gal_frame);
    lv_canvas_set_buffer(gal_hd_canvas, gal_hd_buf, CAM_HD_W, CAM_HD_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(gal_hd_canvas, 0, 0);
    lv_canvas_fill_bg(gal_hd_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(gal_hd_canvas, LV_OBJ_FLAG_HIDDEN);

    gal_status = label(col, "...", FONT_SMALL, CY_DIM);
    lv_obj_set_width(gal_status, LV_PCT(100));
    lv_label_set_long_mode(gal_status, LV_LABEL_LONG_DOT);

    /* barre browse */
    gal_browse_row = lv_obj_create(col);
    lv_obj_set_size(gal_browse_row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(gal_browse_row);
    lv_obj_set_flex_flow(gal_browse_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(gal_browse_row, 6, 0);
    lv_obj_clear_flag(gal_browse_row, LV_OBJ_FLAG_SCROLLABLE);
    gal_prev_btn = small_button(gal_browse_row, LV_SYMBOL_LEFT,     CY_CYAN,    gal_prev_cb);
    gal_del_btn  = small_button(gal_browse_row, LV_SYMBOL_TRASH,    CY_MAGENTA, gal_del_cb);
    gal_nav_btn  = small_button(gal_browse_row, LV_SYMBOL_EYE_OPEN, CY_AMBER,   gal_nav_enter_cb);
    gal_next_btn = small_button(gal_browse_row, LV_SYMBOL_RIGHT,    CY_CYAN,    gal_next_cb);

    /* barre nav */
    gal_nav_row = lv_obj_create(col);
    lv_obj_set_size(gal_nav_row, LV_PCT(100), LV_SIZE_CONTENT);
    flat(gal_nav_row);
    lv_obj_set_flex_flow(gal_nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(gal_nav_row, 6, 0);
    lv_obj_clear_flag(gal_nav_row, LV_OBJ_FLAG_SCROLLABLE);
    small_button(gal_nav_row, LV_SYMBOL_MINUS, CY_CYAN,    gal_zoom_out_cb);
    gal_zoom_lbl = label(gal_nav_row, "x1.0", FONT_SMALL, CY_AMBER);
    small_button(gal_nav_row, LV_SYMBOL_PLUS,  CY_CYAN,    gal_zoom_in_cb);
    small_button(gal_nav_row, LV_SYMBOL_LOOP,  CY_DIM,     gal_zoom_reset_cb);
    small_button(gal_nav_row, LV_SYMBOL_CLOSE, CY_MAGENTA, gal_nav_exit_cb);
    lv_obj_add_flag(gal_nav_row, LV_OBJ_FLAG_HIDDEN);

    gal_nav_mode = false;
    gal_zoom = GAL_FIT_SCALE; gal_ox = gal_oy = 0;

    gal_n = sys_cam_photo_list(gal_paths, GAL_MAX);
    gal_idx = 0;
    gal_show(0);
}
