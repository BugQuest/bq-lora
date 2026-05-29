/*
 * meshtastic-screen — UI de test LVGL
 *
 * Affiche la démo widgets LVGL sur le framebuffer SPI (/dev/fb0) de l'écran
 * MKS TS35-R V2.0 (ILI9488 piloté par le driver fbtft ili9486).
 *
 * Étape suivante : remplacer lv_demo_widgets() par l'UI de commande Meshtastic
 * et brancher l'entrée tactile evdev (ADS7846) avec calibration.
 */
#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include <unistd.h>

int main(void)
{
    lv_init();

    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");

    /* Démo de test — sera remplacée par l'UI Meshtastic */
    lv_demo_widgets();

    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
