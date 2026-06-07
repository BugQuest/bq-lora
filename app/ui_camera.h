#pragma once

/* App CAMERA + GALERIE (viewfinder live + capture HD + parcours + mode nav).
 * Detache de ui.c. Toutes les statics restent file-local. */

void ui_camera_build(void);
void ui_camera_reset(void);
void ui_gallery_build(void);
void ui_gallery_reset(void);

/* Arret du flux camera (a appeler depuis show_tab avant de cleaner content). */
void ui_camera_stream_stop(void);
