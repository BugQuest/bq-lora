#pragma once
#include <stdbool.h>

/* Bouton d'alimentation physique + LED d'activite RX (GPIO via libgpiod).
 *
 * Cablage :
 *   Bouton : GPIO 27 (pin 13) --- [push button] --- GND (pin 14)
 *            Pull-up interne actif, donc REPOS = HIGH, PRESSE = LOW.
 *            Appui court : bascule la veille (ecran off / on).
 *            Appui >= 3 s : ouvre le dialogue Eteindre / Redemarrer.
 *            Note : GPIO 17 (pin 11) ne peut PAS etre utilise ici, il est
 *            deja affecte au beeper du MKS TS35 (cf. tools/beep.py).
 *
 *   LED    : GPIO 4 (pin 7) --- [resistance 330 ohm] --- [LED anode]
 *                                                       [LED cathode] --- GND (pin 6)
 *            S'allume 150 ms a chaque paquet LoRa recu (mesh_take_rx_pulse).
 *            Active aussi pendant la veille -> indicateur visible meme ecran off.
 *
 * Le mesh continue d'ecouter pendant la veille : seul l'ecran + le tactile sont
 * suspendus. La LED + le bouton restent fonctionnels.
 */

/* Ouvre /dev/gpiochip0, requete les lignes et demarre un lv_timer de polling.
 * Retourne false si GPIO inaccessibles (UI continue sans bouton/LED). */
bool pwrbtn_init(void);

/* Ferme proprement les requetes GPIO (appel optionnel a la fin). */
void pwrbtn_close(void);
