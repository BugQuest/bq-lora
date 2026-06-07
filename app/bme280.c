#include "bme280.h"

#if CFG_BME280

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

/* Driver minimaliste pour le Bosch BME280 sur I2C (forced mode).
 * Implementation scaffold : ouvre /dev/i2c-1, sonde 0x76/0x77, lit l'ID puis
 * effectue une mesure compensee (algo officiel Bosch sur les coefficients de
 * calibration NVM). A completer pour le tuning (oversampling, IIR, sleep).
 *
 * Status : code fonctionnel mais NON teste en hardware -- le BME280 n'est pas
 * encore cable. La premiere fois qu'on le branche, valider que :
 *  1. i2cdetect -y 1 voit le capteur a 0x76 ou 0x77
 *  2. bme280_init() retourne true
 *  3. bme280_read() donne des valeurs plausibles (T ambiante, P ~1000 hPa). */

#define I2C_DEV     "/dev/i2c-1"
#define REG_ID      0xD0    /* doit valoir 0x60 pour un vrai BME280 */
#define REG_RESET   0xE0
#define REG_CTRL_HUM   0xF2
#define REG_STATUS  0xF3
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG  0xF5
#define REG_DATA    0xF7    /* burst 8 octets : press_msb..hum_lsb */
#define REG_CALIB_T1 0x88   /* T1..T3 puis P1..P9 (24 octets) */
#define REG_CALIB_H1 0xA1
#define REG_CALIB_H2 0xE1

static int  s_fd = -1;
static int  s_addr;
static bme280_sample_t s_last;

/* coefficients NVM (signed/unsigned selon datasheet) */
static uint16_t dig_T1, dig_P1;
static int16_t  dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5,
                dig_P6, dig_P7, dig_P8, dig_P9;
static uint8_t  dig_H1, dig_H3;
static int16_t  dig_H2, dig_H4, dig_H5;
static int8_t   dig_H6;
static int32_t  t_fine;

static int i2c_read(uint8_t reg, uint8_t *buf, int n) {
    if (write(s_fd, &reg, 1) != 1) return -1;
    if (read(s_fd, buf, n) != n)    return -1;
    return 0;
}
static int i2c_write_u8(uint8_t reg, uint8_t v) {
    uint8_t b[2] = { reg, v };
    return write(s_fd, b, 2) == 2 ? 0 : -1;
}

static bool read_calib(void) {
    uint8_t buf[26];
    if (i2c_read(REG_CALIB_T1, buf, 24)) return false;
    dig_T1 = buf[0] | (buf[1] << 8);
    dig_T2 = (int16_t)(buf[2] | (buf[3] << 8));
    dig_T3 = (int16_t)(buf[4] | (buf[5] << 8));
    dig_P1 = buf[6] | (buf[7] << 8);
    dig_P2 = (int16_t)(buf[8] | (buf[9] << 8));
    dig_P3 = (int16_t)(buf[10] | (buf[11] << 8));
    dig_P4 = (int16_t)(buf[12] | (buf[13] << 8));
    dig_P5 = (int16_t)(buf[14] | (buf[15] << 8));
    dig_P6 = (int16_t)(buf[16] | (buf[17] << 8));
    dig_P7 = (int16_t)(buf[18] | (buf[19] << 8));
    dig_P8 = (int16_t)(buf[20] | (buf[21] << 8));
    dig_P9 = (int16_t)(buf[22] | (buf[23] << 8));
    uint8_t h1; if (i2c_read(REG_CALIB_H1, &h1, 1)) return false;
    dig_H1 = h1;
    uint8_t h[7]; if (i2c_read(REG_CALIB_H2, h, 7)) return false;
    dig_H2 = (int16_t)(h[0] | (h[1] << 8));
    dig_H3 = h[2];
    dig_H4 = (int16_t)((h[3] << 4) | (h[4] & 0x0F));
    dig_H5 = (int16_t)((h[5] << 4) | (h[4] >> 4));
    dig_H6 = (int8_t)h[6];
    return true;
}

static int32_t compensate_T(int32_t adc_T) {
    int32_t v1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * (int32_t)dig_T2) >> 11;
    int32_t v2 = (((((adc_T >> 4) - (int32_t)dig_T1) *
                    ((adc_T >> 4) - (int32_t)dig_T1)) >> 12) * (int32_t)dig_T3) >> 14;
    t_fine = v1 + v2;
    return (t_fine * 5 + 128) >> 8;     /* x100 deg C */
}
static uint32_t compensate_P(int32_t adc_P) {
    int64_t v1 = (int64_t)t_fine - 128000;
    int64_t v2 = v1 * v1 * (int64_t)dig_P6;
    v2 += (v1 * (int64_t)dig_P5) << 17;
    v2 += ((int64_t)dig_P4) << 35;
    v1 = ((v1 * v1 * (int64_t)dig_P3) >> 8) + ((v1 * (int64_t)dig_P2) << 12);
    v1 = (((((int64_t)1) << 47) + v1) * (int64_t)dig_P1) >> 33;
    if (v1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = ((int64_t)dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    v2 = ((int64_t)dig_P8 * p) >> 19;
    p = ((p + v1 + v2) >> 8) + ((int64_t)dig_P7 << 4);
    return (uint32_t)p;                 /* Q24.8 Pa */
}
static uint32_t compensate_H(int32_t adc_H) {
    int32_t v = t_fine - 76800;
    v = (((((adc_H << 14) - ((int32_t)dig_H4 << 20) - ((int32_t)dig_H5 * v)) +
           16384) >> 15) *
         (((((((v * (int32_t)dig_H6) >> 10) *
             (((v * (int32_t)dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
            (int32_t)dig_H2 + 8192) >> 14));
    v -= (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)dig_H1) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    return (uint32_t)(v >> 12);        /* Q22.10 % */
}

bool bme280_init(void) {
    memset(&s_last, 0, sizeof(s_last));
    s_fd = open(I2C_DEV, O_RDWR);
    if (s_fd < 0) return false;
    for (int addr = 0x76; addr <= 0x77; addr++) {
        if (ioctl(s_fd, I2C_SLAVE, addr) < 0) continue;
        uint8_t id = 0;
        if (i2c_read(REG_ID, &id, 1) == 0 && id == 0x60) {
            s_addr = addr;
            if (!read_calib()) { close(s_fd); s_fd = -1; return false; }
            /* config : standby 500ms, IIR coeff 4 */
            i2c_write_u8(REG_CONFIG,    (4 << 5) | (2 << 2));
            /* osrs_h = x1 (a ecrire AVANT ctrl_meas) */
            i2c_write_u8(REG_CTRL_HUM,  1);
            /* osrs_t=x1, osrs_p=x1, mode=normal */
            i2c_write_u8(REG_CTRL_MEAS, (1 << 5) | (1 << 2) | 3);
            return true;
        }
    }
    close(s_fd); s_fd = -1;
    return false;
}

bool bme280_read(bme280_sample_t *out) {
    if (s_fd < 0) { if (out) *out = s_last; return false; }
    uint8_t buf[8];
    if (i2c_read(REG_DATA, buf, 8)) { if (out) *out = s_last; return false; }
    int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
    int32_t adc_H = ((int32_t)buf[6] << 8)  |  (int32_t)buf[7];

    int32_t T  = compensate_T(adc_T);       /* x100 C */
    uint32_t P = compensate_P(adc_P);       /* Q24.8 Pa */
    uint32_t H = compensate_H(adc_H);       /* Q22.10 % */

    s_last.ok           = true;
    s_last.temp_c       = T / 100.0f;
    s_last.pressure_hpa = (P / 256.0f) / 100.0f;
    s_last.humidity     = H / 1024.0f;
    /* altitude barometrique (formule simplifiee, QNH 1013.25 hPa) */
    s_last.altitude_m   = 44330.0f * (1.0f - powf(s_last.pressure_hpa / 1013.25f, 0.1903f));
    s_last.last_read    = (uint32_t)time(NULL);
    if (out) *out = s_last;
    return true;
}

const bme280_sample_t *bme280_last(void) {
    return &s_last;
}

#endif /* CFG_BME280 */
