#pragma once

/* Vue DIAG RF : analyse du trafic radio.
 * - en-tete : compteurs RX/TX, utilisation canal / air-time, compteurs par
 *   type de paquet (texte, position, nodeinfo, telemetrie, chiffre...).
 * - liste live : derniers paquets recus d'un autre noeud, decodes ou non.
 * Detachee de ui.c, meme schema que ui_nodes. */
void ui_diag_build(void);

/* A appeler depuis show_tab APRES lv_obj_clean(content) : null tous les
 * pointeurs vers les widgets liberes (sinon le timer mesh_refresh tape dans
 * la memoire libre -> SIGSEGV). */
void ui_diag_reset(void);

/* Re-sync la vue si elle est actuellement construite. Appelee depuis
 * mesh_refresh_cb pour suivre le trafic en direct. No-op autrement. */
void ui_diag_sync_if_visible(void);
