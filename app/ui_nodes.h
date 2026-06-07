#pragma once

/* Vue NODES (liste des nœuds + radio header + tri + sparkline header).
 * Detache de ui.c. */
void ui_nodes_build(void);

/* Re-sync la liste si la vue est actuellement construite (nodes_list != NULL).
 * Appelee depuis mesh_refresh_cb pour suivre les nouveaux signaux. No-op
 * autrement. */
void ui_nodes_sync_if_visible(void);
