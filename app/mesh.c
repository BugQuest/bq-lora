#include "mesh.h"
#include "settings.h"
#include <string.h>
#include <stdio.h>

/* ---- Canaux : 0 = public, le reste chiffré ---- */
static const mesh_channel_t s_channels[] = {
    { "LongFast", false },
    { "Secure",   true  },
    { "Family",   true  },
};

/* ---- Noeuds du mesh (factices) ---- */
static const mesh_node_t s_nodes[] = {
    { "!7f3a", "NODE-7F3A (moi)", 0,    0,  82, 0, "-",      true  },
    { "!a1b2", "Tour-Eiffel",    8,  -92,  64, 1, "2 min",  false },
    { "!c3d4", "Relais-Nord",   11,  -78,  91, 1, "12 s",   false },
    { "!e5f6", "Mobile-Jules",  -3, -114,  37, 2, "5 min",  false },
    { "!0a9b", "Cabane-Bois",    5, -101,  44, 2, "31 min", false },
};

/* ---- Messages (factices + ceux envoyés en session) ---- */
static mesh_message_t s_msgs[64] = {
    { 0, "Relais-Nord", "Mesh up, 5 noeuds visibles.",        "12:31", false, 0 },
    { 0, "Tour-Eiffel", "Bien recu, SNR correct ici.",        "12:33", false, 0 },
    { 0, "NODE-7F3A",   "Test portee depuis le parc.",        "12:35", true,  2 },
    { 1, "Mobile-Jules","Canal securise OK ?",                "12:40", false, 0 },
    { 1, "NODE-7F3A",   "Oui, PSK partagee.",                 "12:41", true,  2 },
    { 2, "Cabane-Bois", "On mange a 20h.",                    "11:58", false, 0 },
};
static int s_msg_count = 6;

static const mesh_self_t s_self = {
    "EU868", "LongFast", 82, 4.02f, 7.3f, 1.1f, "3h 12m", 5
};

int mesh_channel_count(void) { return (int)(sizeof(s_channels)/sizeof(s_channels[0])); }
const mesh_channel_t *mesh_channel(int i) { return &s_channels[i]; }

int mesh_node_count(void) { return (int)(sizeof(s_nodes)/sizeof(s_nodes[0])); }
const mesh_node_t *mesh_node(int i) { return &s_nodes[i]; }

int mesh_message_count(uint8_t ch) {
    int n = 0;
    for (int i = 0; i < s_msg_count; i++) if (s_msgs[i].ch == ch) n++;
    return n;
}

const mesh_message_t *mesh_message(uint8_t ch, int idx) {
    int n = 0;
    for (int i = 0; i < s_msg_count; i++) {
        if (s_msgs[i].ch == ch) {
            if (n == idx) return &s_msgs[i];
            n++;
        }
    }
    return NULL;
}

void mesh_send(uint8_t ch, const char *text) {
    if (s_msg_count >= (int)(sizeof(s_msgs)/sizeof(s_msgs[0]))) return;
    mesh_message_t *m = &s_msgs[s_msg_count++];
    m->ch = ch;
    m->from = settings_node_name();
    strncpy(m->text, text, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';
    m->time = "now";
    m->out = true;
    m->ack = 1; /* émis ; passera à 2 quand le vrai ACK arrivera */
}

const mesh_self_t *mesh_self(void) { return &s_self; }
