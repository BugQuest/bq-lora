#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Interface "backend" Meshtastic. Implémentée par mesh.c : client protobuf
 * natif (API Stream TCP de meshtasticd sur 127.0.0.1:4403). Les accesseurs
 * renvoient des const char* vers du stockage interne stable.
 */

typedef struct {
    uint32_t    num;        /* node number (clé stable, pour la maj incrémentale UI) */
    const char *id;         /* identifiant court, ex "!a1b2" */
    const char *name;       /* nom long du nœud */
    int8_t      snr;        /* dB (dernier vu) */
    int16_t     rssi;       /* dBm */
    uint8_t     batt;       /* % */
    uint8_t     hops;       /* sauts */
    int8_t      best_snr;   /* meilleur SNR jamais vu (dB), -128 = inconnu */
    uint32_t    first_heard;/* epoch du premier contact */
    uint32_t    last_heard; /* epoch du dernier contact (tri/affichage) */
    const char *last;       /* dernier contact relatif, ex "2m" */
    bool        self;       /* true = ce nœud-ci */
} mesh_node_t;

typedef struct {
    const char *name;      /* nom du canal */
    bool        enc;       /* true = chiffré (PSK custom), false = public */
    uint8_t     index;     /* index réel Meshtastic (0 = primaire) */
    uint8_t     role;      /* 0 désactivé, 1 primaire, 2 secondaire */
} mesh_channel_t;

typedef struct {
    uint8_t     ch;        /* index de canal */
    const char *from;      /* expéditeur (nom) */
    char        text[160];
    const char *time;      /* ex "12:43" */
    bool        out;       /* true = envoyé par nous */
    uint8_t     ack;       /* 0 aucun, 1 émis, 2 acquitté */
} mesh_message_t;

typedef struct {
    const char *region;    /* ex "EU868" */
    const char *preset;    /* ex "LongFast" */
    uint8_t     batt;      /* % */
    float       volt;      /* V */
    float       chan_util; /* % utilisation canal */
    float       air_tx;    /* % air time TX */
    const char *uptime;    /* ex "3h 12m" */
    int         nodes;     /* nœuds vus */
} mesh_self_t;

int                 mesh_channel_count(void);
const mesh_channel_t *mesh_channel(int i);

/* ---- Gestion des canaux (configure le nœud local via AdminMessage) ----
 * Toutes ces fonctions émettent vers meshtasticd ; la liste locale est
 * rafraîchie ~1 s plus tard (re-handshake), donc mesh_take_dirty() repassera
 * à true quand les changements seront pris en compte. */

/* Crée un canal secondaire au premier index libre.
 * encrypted=true -> PSK AES256 aléatoire ; false -> canal public (clé par défaut).
 * Retourne false si plus d'index libre ou nœud non prêt. */
bool mesh_channel_create(const char *name, bool encrypted);

/* Renomme le canal en position i (conserve clé + rôle). */
bool mesh_channel_rename(int i, const char *name);

/* Supprime (désactive) le canal secondaire en position i. Refuse le primaire. */
bool mesh_channel_delete(int i);

/* URL de partage "https://meshtastic.org/e/#..." du canal en position i.
 * Renvoie un pointeur vers un buffer interne (écrasé à chaque appel), NULL si erreur. */
const char *mesh_channel_share_url(int i);

/* Importe le(s) canal(aux) d'une URL collée. Retourne le nombre ajouté, -1 si URL invalide. */
int  mesh_channel_import_url(const char *url);

/* Renomme le nœud local sur le mesh (AdminMessage set_owner). long_name = nom
 * complet diffusé aux autres nœuds ; short_name = 1 à 4 caractères. Nécessite la
 * liaison active. Retourne false si nœud non prêt ou long_name vide. */
bool mesh_set_owner(const char *long_name, const char *short_name);

int                 mesh_node_count(void);
const mesh_node_t  *mesh_node(int i);

int                 mesh_message_count(uint8_t ch);
const mesh_message_t *mesh_message(uint8_t ch, int idx);

/* Compteur cumulatif de messages texte REÇUS (hors les nôtres), monotone depuis
 * le lancement. L'UI en garde une copie "lue" pour calculer les non-lus. */
unsigned            mesh_rx_msg_total(void);
void                mesh_send(uint8_t ch, const char *text);

const mesh_self_t  *mesh_self(void);

/* ---- Cycle de vie de la liaison vers meshtasticd ---- */
void mesh_init(void);       /* ouvre la connexion API (non bloquant) */
void mesh_poll(void);       /* à appeler régulièrement depuis la boucle principale */
bool mesh_connected(void);  /* true si la liaison est établie et configurée */
/* true (et remet à zéro) si messages/nœuds ont changé depuis le dernier appel */
bool mesh_take_dirty(void);

/* Active/désactive l'usage de meshtasticd par l'UI. Désactiver ferme la
 * connexion API locale (libère le port 4403 pour un autre client, ex. l'appli
 * Android via le hotspot). Réactiver relance la connexion. */
void mesh_set_enabled(bool en);
bool mesh_enabled(void);
