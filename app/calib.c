#include "calib.h"
#include "touch.h"
#include "theme.h"
#include "lvgl/lvgl.h"
#include <math.h>

#define NPT 5
#define SCR_W 320
#define SCR_H 480
#define MARGIN 0

static const int targets[NPT][2] = {
    { MARGIN,         MARGIN         },
    { SCR_W - MARGIN, MARGIN         },
    { SCR_W - MARGIN, SCR_H - MARGIN },
    { MARGIN,         SCR_H - MARGIN },
    { SCR_W / 2,      SCR_H / 2      },
};

static lv_obj_t *overlay, *cross_h, *cross_v, *info;
static lv_timer_t *timer;
static void (*on_done)(void);

static int   idx;
static double sum_rx, sum_ry;
static int   samples;
static bool  was_pressed;
static double raw_pts[NPT][2];   /* moyennes brutes relevées */

/* résout A·p = b (3x3) par élimination de Gauss ; retourne false si singulier */
static bool solve3(double A[3][3], double b[3], double out[3])
{
    for (int i = 0; i < 3; i++) {
        int piv = i;
        for (int r = i + 1; r < 3; r++)
            if (fabs(A[r][i]) > fabs(A[piv][i])) piv = r;
        if (fabs(A[piv][i]) < 1e-12) return false;
        if (piv != i) {
            for (int c = 0; c < 3; c++) { double t = A[i][c]; A[i][c] = A[piv][c]; A[piv][c] = t; }
            double t = b[i]; b[i] = b[piv]; b[piv] = t;
        }
        for (int r = 0; r < 3; r++) if (r != i) {
            double f = A[r][i] / A[i][i];
            for (int c = 0; c < 3; c++) A[r][c] -= f * A[i][c];
            b[r] -= f * b[i];
        }
    }
    for (int i = 0; i < 3; i++) out[i] = b[i] / A[i][i];
    return true;
}

/* moindres carrés affine : (rx,ry) -> screen, sur les NPT points */
static void compute_affine(double cx[3], double cy[3])
{
    double Sxx = 0, Sxy = 0, Sx = 0, Syy = 0, Sy = 0;
    double bx[3] = {0, 0, 0}, by[3] = {0, 0, 0};
    for (int i = 0; i < NPT; i++) {
        double rx = raw_pts[i][0], ry = raw_pts[i][1];
        double sx = targets[i][0], sy = targets[i][1];
        Sxx += rx * rx; Sxy += rx * ry; Sx += rx;
        Syy += ry * ry; Sy += ry;
        bx[0] += rx * sx; bx[1] += ry * sx; bx[2] += sx;
        by[0] += rx * sy; by[1] += ry * sy; by[2] += sy;
    }
    double A[3][3] = { { Sxx, Sxy, Sx }, { Sxy, Syy, Sy }, { Sx, Sy, NPT } };
    double A2[3][3]; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) A2[r][c] = A[r][c];
    solve3(A, bx, cx);
    solve3(A2, by, cy);
}

static void place_cross(int i)
{
    lv_obj_set_pos(cross_h, targets[i][0] - 12, targets[i][1]);
    lv_obj_set_pos(cross_v, targets[i][0], targets[i][1] - 12);
    lv_label_set_text_fmt(info, "Touche la croix  (%d/%d)", i + 1, NPT);
}

static void finish(void)
{
    double cx[3], cy[3];
    compute_affine(cx, cy);
    touch_set_affine(cx, cy);
    touch_save();
    touch_set_calib_mode(false);
    lv_timer_delete(timer);
    lv_obj_delete(overlay);
    if (on_done) on_done();
}

static void tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    int rx, ry;
    bool p = touch_raw(&rx, &ry);

    if (p) {
        sum_rx += rx; sum_ry += ry; samples++;
        was_pressed = true;
    } else if (was_pressed) {            /* relâché -> on valide le point */
        was_pressed = false;
        if (samples >= 3) {
            raw_pts[idx][0] = sum_rx / samples;
            raw_pts[idx][1] = sum_ry / samples;
            idx++;
        }
        sum_rx = sum_ry = 0; samples = 0;
        if (idx >= NPT) finish();
        else place_cross(idx);
    }
}

void calib_start(void (*done)(void))
{
    on_done = done;
    idx = samples = 0;
    sum_rx = sum_ry = 0;
    was_pressed = false;
    touch_set_calib_mode(true);

    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(CY_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    info = lv_label_create(overlay);
    lv_obj_set_style_text_color(info, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_text_font(info, FONT_BODY, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint = lv_label_create(overlay);
    lv_label_set_text(hint, "Calibrage tactile");
    lv_obj_set_style_text_color(hint, lv_color_hex(CY_DIM), 0);
    lv_obj_set_style_text_font(hint, FONT_SMALL, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 64);

    cross_h = lv_obj_create(overlay);
    lv_obj_set_size(cross_h, 25, 2);
    lv_obj_set_style_bg_color(cross_h, lv_color_hex(0xFF0033), 0);
    lv_obj_set_style_border_width(cross_h, 0, 0);
    lv_obj_set_style_radius(cross_h, 0, 0);

    cross_v = lv_obj_create(overlay);
    lv_obj_set_size(cross_v, 2, 25);
    lv_obj_set_style_bg_color(cross_v, lv_color_hex(0xFF0033), 0);
    lv_obj_set_style_border_width(cross_v, 0, 0);
    lv_obj_set_style_radius(cross_v, 0, 0);

    place_cross(0);
    timer = lv_timer_create(tick, 30, NULL);
}
