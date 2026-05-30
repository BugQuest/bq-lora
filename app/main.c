#include "lvgl/lvgl.h"
#include "ui.h"
#include "touch.h"
#include "calib.h"
#include "settings.h"
#include <unistd.h>
#include <time.h>
#include <stdint.h>

static uint32_t tick_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* Après le splash : calibrage 5 points si jamais calibré. */
static void after_splash(void)
{
    if (!touch_have_cal())
        calib_start(NULL);
}

int main(void)
{
    settings_load();

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");

    /* Pilote tactile maison (lecture brute + affine). */
    touch_init();

    ui_init();
    ui_show_splash(after_splash);

    while (1) {
        uint32_t idle = lv_timer_handler();
        if (idle > 20) idle = 20;
        usleep(idle * 1000);
    }
    return 0;
}
