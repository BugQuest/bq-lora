#pragma once
#include "config.h"
#include <stdbool.h>
#include <stdint.h>

/* Capteur environnemental Bosch BME280 (I2C, temp / hum / pression).
 * Adresse 0x76 (SDO=GND) ou 0x77 (SDO=VCC). Branche sur le bus I2C1
 * (GPIO2=SDA / GPIO3=SCL) avec dtparam=i2c_arm=on dans config.txt.
 *
 * Etat actuel : scaffold seul. Le code I2C reel (initialisation, lecture des
 * registres de calibration, compensation) viendra dans une 2e passe. Les
 * accesseurs ci-dessous renvoient des valeurs neutres / un fix=false tant
 * que CFG_BME280 est OFF (defaut) ou que la lecture n'est pas implementee. */

typedef struct {
    bool   ok;          /* true = derniere lecture valide */
    float  temp_c;      /* degres Celsius */
    float  humidity;    /* % humidite relative */
    float  pressure_hpa;/* hPa (= mbar) */
    float  altitude_m;  /* estimation depuis QNH 1013.25 hPa */
    uint32_t last_read; /* epoch de la derniere lecture */
} bme280_sample_t;

#if CFG_BME280
/* Initialise le bus + sonde l'adresse I2C (auto-detect 0x76 puis 0x77).
 * Renvoie true si le capteur a repondu correctement. */
bool bme280_init(void);

/* Lecture forcee (mode one-shot) : remplit out avec les valeurs compensees.
 * Renvoie true si la lecture est OK (sample.ok aussi mis a jour). */
bool bme280_read(bme280_sample_t *out);

/* Dernier echantillon connu (sans relancer une lecture I2C). */
const bme280_sample_t *bme280_last(void);
#else
/* Stubs : sans CFG_BME280, les accesseurs ne plantent jamais et renvoient
 * un sample non valide -> l'UI / la telemetry savent qu'il n'y a rien. */
static inline bool bme280_init(void) { return false; }
static inline bool bme280_read(bme280_sample_t *o) {
    if (o) { o->ok = false; o->temp_c = o->humidity = o->pressure_hpa = o->altitude_m = 0; o->last_read = 0; }
    return false;
}
static inline const bme280_sample_t *bme280_last(void) { return (const bme280_sample_t *)0; }
#endif
