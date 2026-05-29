#pragma once

/* Construit l'interface complète sur l'écran actif. */
void ui_init(void);

/* Affiche le splash d'intro (~1.8s) par-dessus, puis appelle `done` (peut être NULL). */
void ui_show_splash(void (*done)(void));
