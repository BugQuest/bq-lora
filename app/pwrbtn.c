#include "config.h"
#include "pwrbtn.h"

#if CFG_PWRBTN

#include "ui_dialog.h"
#include "lvgl/lvgl.h"
#include "mesh.h"
#include "sys.h"
#include <gpiod.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================ */
/* Bouton power (GPIO 17) + LED activite RX (GPIO 4)            */
/* ============================================================ */

#define GPIO_CHIP        "/dev/gpiochip0"
#define BTN_OFFSET       27        /* pin 13 (GPIO17 deja pris par le buzzer) */
#define LED_OFFSET       4         /* pin 7 */
#define POLL_PERIOD_MS   30      /* assez fin pour debounce + reactif au tap */
#define DEBOUNCE_MS      40      /* press doit durer 40ms minimum */
#define LONG_PRESS_MS    3000    /* >= 3s = ouvre dialogue power */
#define LED_PULSE_MS     150     /* duree d'allumage LED par paquet RX */

/* Etats du bouton (machine a etats simple). */
typedef enum {
    BTN_IDLE,            /* relache, attente d'un nouvel appui */
    BTN_PRESSED,         /* enfonce, on chronometre */
    BTN_LONG_FIRED,      /* long press deja declenche, on attend le relachement */
} btn_state_t;

static struct gpiod_chip          *s_chip;
static struct gpiod_line_request  *s_btn_req;
static struct gpiod_line_request  *s_led_req;

static btn_state_t s_state = BTN_IDLE;
static uint32_t    s_press_start_ms;       /* tick LVGL du debut d'appui */
static uint32_t    s_led_off_at_ms;        /* tick d'extinction LED */
static lv_timer_t *s_timer;

/* Acces aux fonctions PM exposees par main.c (pas de header dedie). */
extern void pm_enter_sleep(void);
extern void pm_wake(void);
extern bool pm_is_asleep(void);

/* ------------------------------------------------------------ libgpiod helpers */

static bool request_input_pullup(unsigned int offset, struct gpiod_line_request **out)
{
    struct gpiod_line_settings *ls = gpiod_line_settings_new();
    if (!ls) return false;
    gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_active_low(ls, true);  /* presse = ACTIVE */

    struct gpiod_line_config *lc = gpiod_line_config_new();
    if (!lc) { gpiod_line_settings_free(ls); return false; }
    gpiod_line_config_add_line_settings(lc, &offset, 1, ls);

    struct gpiod_request_config *rc = gpiod_request_config_new();
    if (rc) gpiod_request_config_set_consumer(rc, "bq-lora-ui-btn");

    *out = gpiod_chip_request_lines(s_chip, rc, lc);

    if (rc) gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(ls);
    return *out != NULL;
}

static bool request_output(unsigned int offset, struct gpiod_line_request **out)
{
    struct gpiod_line_settings *ls = gpiod_line_settings_new();
    if (!ls) return false;
    gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(ls, GPIOD_LINE_VALUE_INACTIVE);

    struct gpiod_line_config *lc = gpiod_line_config_new();
    if (!lc) { gpiod_line_settings_free(ls); return false; }
    gpiod_line_config_add_line_settings(lc, &offset, 1, ls);

    struct gpiod_request_config *rc = gpiod_request_config_new();
    if (rc) gpiod_request_config_set_consumer(rc, "bq-lora-ui-led");

    *out = gpiod_chip_request_lines(s_chip, rc, lc);

    if (rc) gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(ls);
    return *out != NULL;
}

static bool btn_is_pressed(void)
{
    if (!s_btn_req) return false;
    enum gpiod_line_value v = gpiod_line_request_get_value(s_btn_req, BTN_OFFSET);
    return v == GPIOD_LINE_VALUE_ACTIVE;
}

static void led_set(bool on)
{
    if (!s_led_req) return;
    gpiod_line_request_set_value(s_led_req, LED_OFFSET,
                                 on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

/* ------------------------------------------------------------ actions */

static void do_reboot(void)     { sys_reboot();   }
static void do_shutdown(void)   { sys_shutdown(); }

/* Ouverture du dialogue power (long press). Reveille l'ecran si endormi
 * pour qu'on voie le dialogue. */
static void open_power_dialog(void)
{
    if (pm_is_asleep()) pm_wake();
    ui_dialog_choice(
        "Que veux-tu faire ?",
        LV_SYMBOL_REFRESH " Redemarrer", do_reboot,
        LV_SYMBOL_POWER   " Eteindre",   do_shutdown);
}

/* Toggle veille sur appui court. */
static void short_press_action(void)
{
    if (pm_is_asleep()) pm_wake();
    else                pm_enter_sleep();
}

/* ------------------------------------------------------------ polling loop */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();

    /* --- LED RX activity --- */
    if (mesh_take_rx_pulse()) {
        s_led_off_at_ms = now + LED_PULSE_MS;
        led_set(true);
    } else if (s_led_off_at_ms && now >= s_led_off_at_ms) {
        s_led_off_at_ms = 0;
        led_set(false);
    }

    /* --- Bouton power : machine a etats --- */
    bool pressed = btn_is_pressed();

    switch (s_state) {
    case BTN_IDLE:
        if (pressed) {
            s_press_start_ms = now;
            s_state = BTN_PRESSED;
        }
        break;

    case BTN_PRESSED:
        if (!pressed) {
            uint32_t held = now - s_press_start_ms;
            s_state = BTN_IDLE;
            if (held >= DEBOUNCE_MS) short_press_action();
            /* sinon : faux contact (rebond), on ignore */
        } else if ((now - s_press_start_ms) >= LONG_PRESS_MS) {
            s_state = BTN_LONG_FIRED;
            open_power_dialog();
        }
        break;

    case BTN_LONG_FIRED:
        if (!pressed) s_state = BTN_IDLE;
        /* sinon on attend que l'utilisateur lache, sans rien refaire */
        break;
    }
}

/* ------------------------------------------------------------ lifecycle */

bool pwrbtn_init(void)
{
    s_chip = gpiod_chip_open(GPIO_CHIP);
    if (!s_chip) {
        fprintf(stderr, "pwrbtn: cannot open %s (LED + bouton desactives)\n", GPIO_CHIP);
        return false;
    }
    if (!request_input_pullup(BTN_OFFSET, &s_btn_req))
        fprintf(stderr, "pwrbtn: GPIO%d (btn) indisponible\n", BTN_OFFSET);
    if (!request_output(LED_OFFSET, &s_led_req))
        fprintf(stderr, "pwrbtn: GPIO%d (led) indisponible\n", LED_OFFSET);

    /* Au moins l'un des deux doit etre OK pour qu'on demarre le timer. */
    if (!s_btn_req && !s_led_req) {
        gpiod_chip_close(s_chip);
        s_chip = NULL;
        return false;
    }
    led_set(false);
    s_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
    return true;
}

void pwrbtn_close(void)
{
    if (s_timer)   { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_btn_req) { gpiod_line_request_release(s_btn_req); s_btn_req = NULL; }
    if (s_led_req) {
        led_set(false);
        gpiod_line_request_release(s_led_req);
        s_led_req = NULL;
    }
    if (s_chip)    { gpiod_chip_close(s_chip); s_chip = NULL; }
}

#else  /* CFG_PWRBTN == 0 : stubs vides pour ne pas casser les appelants */

bool pwrbtn_init(void)  { return false; }
void pwrbtn_close(void) {}

#endif /* CFG_PWRBTN */
