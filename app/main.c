#include "lvgl/lvgl.h"
#include "ui.h"
#include "touch.h"
#include "calib.h"
#include "settings.h"
#include "mesh.h"
#include "sys.h"
#include <unistd.h>
#include <time.h>
#include <stdint.h>

static uint32_t tick_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* Mode économie : éteint le rétroéclairage après inactivité, réveil au toucher.
 * Inhibé pendant le calibrage et le flux caméra live (pas de toucher mais
 * l'écran doit rester allumé). */
static bool pm_asleep = false;
static int  pm_saved_bl = 80;

static void power_save_tick(void)
{
    int to = settings_screen_timeout();
    bool inhibit = (to <= 0) || touch_calib_mode() || sys_cam_stream_active();

    if (inhibit) {
        if (pm_asleep) {
            sys_backlight_set(pm_saved_bl);
            touch_set_sleep(false);
            pm_asleep = false;
        }
        if (to > 0) lv_display_trigger_activity(NULL);
        return;
    }

    if (!pm_asleep) {
        if (lv_display_get_inactive_time(NULL) > (uint32_t)to * 1000) {
            pm_saved_bl = sys_backlight_get();
            if (pm_saved_bl < 5) pm_saved_bl = 100;
            sys_backlight_set(0);
            touch_set_sleep(true);
            pm_asleep = true;
        }
    } else if (touch_woke()) {
        sys_backlight_set(pm_saved_bl);
        touch_set_sleep(false);
        pm_asleep = false;
        lv_display_trigger_activity(NULL);
    }
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

    /* Liaison vers le nœud Meshtastic local (meshtasticd, API TCP 4403).
     * Respecte le choix persistant : si l'utilisateur a passé la main au
     * téléphone, l'UI ne reprend pas le port 4403 au redémarrage. */
    mesh_set_enabled(settings_mesh_enabled());
    mesh_init();

    ui_init();
    ui_show_splash(after_splash);

    while (1) {
        mesh_poll();
        power_save_tick();
        uint32_t idle = lv_timer_handler();
        if (idle > 20) idle = 20;
        if (pm_asleep && idle < 80) idle = 80;  /* veille : on lève le pied */
        usleep(idle * 1000);
    }
    return 0;
}
