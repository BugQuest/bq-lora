#pragma once

/* Vue CARTE : tuiles OSM offline (RAW RGB565 dans ~/bq-lora-ui/maps/z/x/y.bin),
 * geoloc GPS, panoramique tactile, calque des noeuds du mesh.
 * Meme schema que ui_nodes / ui_diag / ui_gps. */
void ui_map_build(void);
void ui_map_reset(void);
void ui_map_sync_if_visible(void);
