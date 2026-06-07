#pragma once
#include "lvgl/lvgl.h"
#include <stdint.h>

/* Vues "node" detachees de ui.c.
 *  - node_detail_open(num) : ouvre le modal detail d'un node (stats +
 *    sparkline SNR + textarea DM). Appele depuis le click handler des lignes.
 *  - ui_nodes_tree_open_e : callback du bouton ARBRE dans la vue NODES. */
void node_detail_open(uint32_t num);
void ui_nodes_tree_open_e(lv_event_t *e);
