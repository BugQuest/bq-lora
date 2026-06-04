#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Interface "backend" Meshtastic. Implémentée par mesh.c : client protobuf
 * natif (API Stream TCP de meshtasticd sur 127.0.0.1:4403). Les accesseurs
 * renvoient des const char* vers du stockage interne stable.
 */

typedef struct {
    const char *id;        /* identifiant court, ex "!a1b2" */
    const char *name;      /* nom long du nœud */
    int8_t      snr;       /* dB */
    int16_t     rssi;      /* dBm */
    uint8_t     batt;      /* % */
    uint8_t     hops;      /* sauts */
    const char *last;      /* dernier contact, ex "2 min" */
    bool        self;      /* true = ce nœud-ci */
} mesh_node_t;

typedef struct {
    const char *name;      /* nom du canal */
    bool        enc;       /* true = chiffré (PSK custom), false = public */
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

int                 mesh_node_count(void);
const mesh_node_t  *mesh_node(int i);

int                 mesh_message_count(uint8_t ch);
const mesh_message_t *mesh_message(uint8_t ch, int idx);
void                mesh_send(uint8_t ch, const char *text);

const mesh_self_t  *mesh_self(void);

/* ---- Cycle de vie de la liaison vers meshtasticd ---- */
void mesh_init(void);       /* ouvre la connexion API (non bloquant) */
void mesh_poll(void);       /* à appeler régulièrement depuis la boucle principale */
bool mesh_connected(void);  /* true si la liaison est établie et configurée */
/* true (et remet à zéro) si messages/nœuds ont changé depuis le dernier appel */
bool mesh_take_dirty(void);
