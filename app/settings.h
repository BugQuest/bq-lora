#pragma once
#include <stdbool.h>

/* Valeurs runtime, lues/écrites dans /home/bq-lora/meshui/config.ini.
 * Defaults appliques si fichier absent (premiere execution). */

void  settings_load(void);
void  settings_save(void);

const char *settings_node_name(void);
const char *settings_hotspot_ssid(void);
const char *settings_hotspot_pass(void);
const char *settings_timezone(void);
bool        settings_mesh_enabled(void);   /* l'UI pilote-t-elle meshtasticd ? */
int         settings_screen_timeout(void); /* veille ecran : secondes, 0 = jamais */

void settings_set_node_name(const char *v);
void settings_set_hotspot_ssid(const char *v);
void settings_set_hotspot_pass(const char *v);
void settings_set_timezone(const char *v);
void settings_set_mesh_enabled(bool v);
void settings_set_screen_timeout(int s);
