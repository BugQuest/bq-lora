#include "touch.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#define EVDEV    "/dev/input/event0"
#define CAL_FILE "/home/bq-lora/meshui/touch.cal"
#define SCR_W 320
#define SCR_H 480

static int  fd = -1;
static int  raw_x = 0, raw_y = 0;
static bool pressed = false;
static bool calib_mode = false;
static bool have_cal = false;

/* Lissage : moyenne glissante des derniers échantillons bruts (doigt = bruité). */
#define AVG_N 6
static int  hist_x[AVG_N], hist_y[AVG_N];
static int  hist_n = 0, hist_pos = 0;

/* sx = cx[0]*rx + cx[1]*ry + cx[2] ;  sy = cy[0]*rx + cy[1]*ry + cy[2] */
static double cx[3] = {0, 0, 0};
static double cy[3] = {0, 0, 0};

static void poll_dev(void)
{
    if (fd < 0) return;
    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) raw_x = ev.value;
            else if (ev.code == ABS_Y) raw_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            pressed = ev.value != 0;
        }
    }

    if (pressed) {
        hist_x[hist_pos] = raw_x;
        hist_y[hist_pos] = raw_y;
        hist_pos = (hist_pos + 1) % AVG_N;
        if (hist_n < AVG_N) hist_n++;
    } else {
        hist_n = hist_pos = 0;
    }
}

static void smoothed_raw(int *ax, int *ay)
{
    if (hist_n <= 0) { *ax = raw_x; *ay = raw_y; return; }
    long sx = 0, sy = 0;
    for (int i = 0; i < hist_n; i++) { sx += hist_x[i]; sy += hist_y[i]; }
    *ax = (int)(sx / hist_n);
    *ay = (int)(sy / hist_n);
}

bool touch_raw(int *rx, int *ry)
{
    poll_dev();
    if (rx) *rx = raw_x;
    if (ry) *ry = raw_y;
    return pressed;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    poll_dev();

    int ax, ay;
    smoothed_raw(&ax, &ay);
    double x = cx[0] * ax + cx[1] * ay + cx[2];
    double y = cy[0] * ax + cy[1] * ay + cy[2];
    int sx = (int)(x + 0.5), sy = (int)(y + 0.5);
    if (sx < 0) sx = 0; else if (sx >= SCR_W) sx = SCR_W - 1;
    if (sy < 0) sy = 0; else if (sy >= SCR_H) sy = SCR_H - 1;

    data->point.x = sx;
    data->point.y = sy;
    data->state = (pressed && !calib_mode) ? LV_INDEV_STATE_PRESSED
                                           : LV_INDEV_STATE_RELEASED;
}

void touch_set_affine(const double ncx[3], const double ncy[3])
{
    memcpy(cx, ncx, sizeof(cx));
    memcpy(cy, ncy, sizeof(cy));
    have_cal = true;
}

bool touch_have_cal(void) { return have_cal; }
void touch_set_calib_mode(bool on) { calib_mode = on; }

void touch_save(void)
{
    FILE *f = fopen(CAL_FILE, "w");
    if (!f) return;
    fprintf(f, "%.10f %.10f %.10f %.10f %.10f %.10f\n",
            cx[0], cx[1], cx[2], cy[0], cy[1], cy[2]);
    fclose(f);
}

void touch_load(void)
{
    FILE *f = fopen(CAL_FILE, "r");
    if (!f) { have_cal = false; return; }
    if (fscanf(f, "%lf %lf %lf %lf %lf %lf",
               &cx[0], &cx[1], &cx[2], &cy[0], &cy[1], &cy[2]) == 6)
        have_cal = true;
    fclose(f);
}

lv_indev_t *touch_init(void)
{
    fd = open(EVDEV, O_RDONLY | O_NONBLOCK);

    touch_load();
    if (!have_cal) {
        /* Repli linéaire approximatif (plage ADC relevée), en attendant le
         * calibrage 5 points. Suppose les axes alignés sur le portrait. */
        cx[0] = (double)SCR_W / (3965 - 198); cx[1] = 0; cx[2] = -198 * cx[0];
        cy[0] = 0; cy[1] = (double)SCR_H / (3896 - 206); cy[2] = -206 * cy[1];
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    return indev;
}
