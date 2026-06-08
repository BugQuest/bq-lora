#pragma once

/* Helpers de dialogue modaux : toutes les boites s'affichent sur lv_layer_top()
 * avec un backdrop semi-opaque qui bloque les interactions derriere. Une seule
 * boite a la fois ; ouvrir une nouvelle ferme la precedente.
 *
 * Toutes les fonctions sont thread-unsafe (appelees uniquement depuis le thread
 * UI / boucle LVGL).
 */

/* Loading : sans bouton, doit etre ferme manuellement via ui_dialog_loading_hide().
 * Utilise pour les ops potentiellement longues (reconfig radio, network, etc.).
 * Affiche un spinner anime + le message. */
void ui_dialog_loading_show(const char *msg);
void ui_dialog_loading_hide(void);

/* Info : 1 bouton OK, theme cyan. */
void ui_dialog_info(const char *msg);

/* Warning : 1 bouton OK, theme ambre + icone warning. */
void ui_dialog_warning(const char *msg);

/* Error : 1 bouton OK, theme magenta + icone close. */
void ui_dialog_error(const char *msg);

/* Confirm : 2 boutons (Oui / Non). on_yes peut etre NULL.
 * on_yes est appele apres fermeture de la boite (donc safe pour rouvrir
 * une autre boite depuis le callback). */
void ui_dialog_confirm(const char *msg, void (*on_yes)(void));

/* Choix entre 2 actions + annuler. Callbacks NULL autorises (= bouton inactif
 * mais visible). Les callbacks sont appeles apres fermeture (rouverture safe). */
void ui_dialog_choice(const char *msg,
                      const char *btn1_txt, void (*cb1)(void),
                      const char *btn2_txt, void (*cb2)(void));
