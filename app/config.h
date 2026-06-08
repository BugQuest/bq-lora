#pragma once

/* Configuration des modules optionnels.
 *
 * Les valeurs sont normalement passees par CMake (cf. app/CMakeLists.txt et ses
 * options ENABLE_*). Ce header sert de filet : si le code est compile sans
 * CMake (cas rare, ex: IDE qui n'utilise pas notre CMakeLists), on retombe
 * sur les defauts -- tout active sauf BME280.
 *
 * Usage dans le code :
 *     #if CFG_CAMERA
 *         ...
 *     #endif
 *
 * (Les identifiants non definis valent 0 par defaut en preprocesseur C, mais
 *  on prefere etre explicite ici pour eviter les surprises.)
 */

#ifndef CFG_CAMERA
#define CFG_CAMERA 1
#endif

#ifndef CFG_BLUETOOTH
#define CFG_BLUETOOTH 1
#endif

#ifndef CFG_BADUSB
#define CFG_BADUSB 1
#endif

#ifndef CFG_WIFI_QR
#define CFG_WIFI_QR 1
#endif

#ifndef CFG_WPS
#define CFG_WPS 1
#endif

#ifndef CFG_BME280
#define CFG_BME280 0
#endif

#ifndef CFG_PWRBTN
#define CFG_PWRBTN 1
#endif

/* Coherence : WIFI_QR depend de CAMERA (partage le flux camera live). */
#if CFG_WIFI_QR && !CFG_CAMERA
#error "CFG_WIFI_QR requires CFG_CAMERA"
#endif
