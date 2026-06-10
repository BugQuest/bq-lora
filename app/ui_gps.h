#pragma once

/* Vue GPS (debug) : etat du fix, position, satellites + SNR par PRN.
 * Detachee de ui.c, meme schema que ui_nodes / ui_diag. */
void ui_gps_build(void);
void ui_gps_reset(void);
void ui_gps_sync_if_visible(void);
