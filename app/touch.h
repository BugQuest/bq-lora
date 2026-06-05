#pragma once
#include "lvgl/lvgl.h"
#include <stdbool.h>

/* Pilote tactile maison : lit /dev/input/event0 et applique une transformation
 * affine (brut ADC -> pixels). Remplace lv_evdev pour permettre un calibrage
 * 5 points complet (sens des axes, inversion, biais). */

lv_indev_t *touch_init(void);                 /* ouvre le device + crée l'indev LVGL */

/* Coordonnées ADC brutes courantes ; retourne true si l'écran est pressé. */
bool touch_raw(int *rx, int *ry);

/* Pendant le calibrage : neutralise le pointeur LVGL (les croix ne cliquent rien). */
void touch_set_calib_mode(bool on);

/* Applique une nouvelle affine : sx = cx[0]*rx + cx[1]*ry + cx[2] (idem cy). */
void touch_set_affine(const double cx[3], const double cy[3]);

/* Mode veille : avale le toucher de réveil (pas de clic accidentel à l'allumage). */
void touch_set_sleep(bool on);
/* true (et remet à false) si un toucher a eu lieu pendant la veille. */
bool touch_woke(void);
/* true si on est en mode calibrage (le power-save doit alors être inhibé). */
bool touch_calib_mode(void);

bool touch_have_cal(void);                     /* true si une calibration est chargée */
void touch_save(void);                         /* persiste l'affine sur disque */
void touch_load(void);                         /* charge l'affine depuis le disque */
