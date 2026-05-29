#pragma once

/* Palette cyberpunk minimaliste */
#define CY_BG        0x070A0F   /* fond écran */
#define CY_PANEL     0x0C121C   /* panneaux */
#define CY_PANEL2    0x111A28   /* panneaux secondaires / bulles in */
#define CY_CYAN      0x00E5FF   /* accent principal */
#define CY_MAGENTA   0xFF2A6D   /* chiffré / alerte */
#define CY_GREEN     0x36F58A   /* ack / ok */
#define CY_AMBER     0xFFB000   /* warning / batterie basse */
#define CY_TEXT      0xCDE7F0   /* texte principal */
#define CY_DIM       0x52677A   /* texte secondaire */
#define CY_BORDER    0x1B2A3D   /* bordures neutres */

/* Polices (activées dans lv_conf.h) */
#define FONT_MONO    &lv_font_unscii_16   /* titres / statut, look terminal */
#define FONT_BODY    &lv_font_montserrat_14
#define FONT_SMALL   &lv_font_montserrat_12
#define FONT_BIG     &lv_font_montserrat_20
