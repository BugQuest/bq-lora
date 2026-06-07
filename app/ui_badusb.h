#pragma once
#include "lvgl/lvgl.h"

/* Vue BAD USB (explorateur de scripts + selecteur mode USB + dialog d'execution).
 * Detache de ui.c. Toutes les statics du module restent file-local. */

/* Construit la vue dans le 'content' partage (cf. ui_common.h). */
void ui_badusb_build(void);

/* A appeler depuis show_tab(content) APRES lv_obj_clean(content) : NULL tous
 * les pointeurs de widgets (qui pointent vers de la memoire liberee). */
void ui_badusb_reset(void);
