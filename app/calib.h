#pragma once

/* Lance le calibrage tactile 5 points (croix rouges). Overlay plein écran ;
 * à la fin, l'affine est appliquée + sauvegardée, puis `done` est appelé (peut être NULL). */
void calib_start(void (*done)(void));
