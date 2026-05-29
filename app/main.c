#include "lvgl/lvgl.h"
#include "ui.h"
#include <unistd.h>
#include <time.h>
#include <stdint.h>

static uint32_t tick_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");

    /* Tactile ADS7846 via evdev. Les coords sont des valeurs ADC brutes :
     * calibration approximative à affiner (voir docs). */
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
    if (touch) {
        lv_evdev_set_calibration(touch, 300, 300, 3800, 3800);
    }

    ui_init();

    while (1) {
        uint32_t idle = lv_timer_handler();
        if (idle > 20) idle = 20;
        usleep(idle * 1000);
    }
    return 0;
}
