/*
 * Backend Meshtastic réel : client protobuf natif vers meshtasticd via son
 * API "Stream" TCP (127.0.0.1:4403). Pas de threads : tout est piloté par
 * mesh_poll() appelé depuis la boucle LVGL (socket non bloquant).
 *
 * Trame Stream : 0x94 0xC3 <len_hi> <len_lo> <protobuf>.
 * On envoie des ToRadio (want_config_id, packet, heartbeat), on reçoit des
 * FromRadio (my_info, node_info, channel, config, config_complete_id, packet).
 */
#include "mesh.h"
#include "pb.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ----------------------------------------------------------------- constantes */
#define MESH_HOST        "127.0.0.1"
#define MESH_PORT        4403
#define FR_START1        0x94
#define FR_START2        0xC3
#define BROADCAST_ADDR   0xFFFFFFFFu

/* PortNums utiles */
#define PORT_TEXT        1
#define PORT_ROUTING     5
#define PORT_ADMIN       6

#define HEARTBEAT_SECS   30
#define RECONNECT_SECS   5
#define CONNECT_TMO_SECS 8

#define MAX_NODES        200
#define MAX_CHANS        8
#define MAX_MSGS         128
#define RXBUF_SZ         8192

/* ----------------------------------------------------------------- stockage */
typedef struct {
    uint32_t    num;        /* node number (clé) */
    mesh_node_t pub;        /* vue exposée (pointeurs -> buffers ci-dessous) */
    char        id[16];
    char        name[40];
    char        last[16];
    uint32_t    last_heard; /* epoch du dernier contact */
    uint32_t    first_heard;/* epoch du premier contact (persisté) */
    int8_t      best_snr;   /* meilleur SNR vu, INT8_MIN = inconnu (persisté) */
} node_slot_t;

typedef struct {
    int            index;   /* numéro de canal réel */
    mesh_channel_t pub;
    char           name[24];
    uint8_t        psk[32]; /* clé du canal (conservée pour renommer/partager) */
    size_t         psk_len;
    int            role;    /* 0 désactivé, 1 primaire, 2 secondaire */
} chan_slot_t;

typedef struct {
    mesh_message_t m;       /* m.text embarqué ; m.from/m.time -> buffers */
    char           from[40];
    char           time[12];
    uint32_t       id;      /* id du paquet (dédup + ACK) */
} msg_slot_t;

static node_slot_t s_nodes[MAX_NODES];
static int         s_node_count;

/* total cumulatif de messages texte reçus (hors les nôtres) -> badge non-lus */
static unsigned    s_rx_total;
/* idem mais par canal (badge par chip de canal). Index = position de canal. */
static unsigned    s_rx_per_ch[MAX_CHANS];

static chan_slot_t s_chans[MAX_CHANS];
static int         s_chan_count;

static msg_slot_t  s_msgs[MAX_MSGS];
static int         s_msg_count;

static char        s_region[16] = "EU868";
static char        s_preset[16] = "LongFast";
static char        s_uptime[16] = "-";
static uint32_t    s_region_code = 3;  /* EU_868 */
static uint32_t    s_preset_code = 0;  /* LONG_FAST */
static mesh_self_t s_self;

static uint32_t    s_my_num;
static bool        s_dirty;
static time_t      s_reload_at;        /* != 0 : re-demander la config à cette date */

/* Persistance de la liste des nœuds (trace locale, survit aux redémarrages). */
#define NODES_DB_PATH       "/home/bq-lora/bq-lora-ui/nodes.db"
#define NODES_SAVE_MIN_SECS 30         /* throttle écriture (ménage la carte SD) */
static bool        s_nodes_save_pending;
static time_t      s_nodes_last_save;

/* Persistance de l'historique des messages (survit aux redemarrages). */
#define MSGS_DB_PATH       "/home/bq-lora/bq-lora-ui/messages.db"
#define MSGS_SAVE_MIN_SECS 30
static bool        s_msgs_save_pending;
static time_t      s_msgs_last_save;

/* ----------------------------------------------------------------- liaison */
static bool   s_enabled = true;        /* l'UI pilote-t-elle la liaison ? */
static int    s_fd = -1;
static bool   s_connecting;
static bool   s_configured;
static time_t s_last_attempt;
static time_t s_connect_started;
static time_t s_last_hb;

static uint8_t s_rx[RXBUF_SZ];
static size_t  s_rxlen;

/* ----------------------------------------------------------------- utilitaires */
static void fmt_rel(char *out, size_t cap, uint32_t epoch)
{
    if (epoch == 0) { snprintf(out, cap, "-"); return; }
    time_t now = time(NULL);
    long d = (long)now - (long)epoch;
    if (d < 0) d = 0;
    if (d < 60)        snprintf(out, cap, "%lds", d);
    else if (d < 3600) snprintf(out, cap, "%ldm", d / 60);
    else if (d < 86400)snprintf(out, cap, "%ldh", d / 3600);
    else               snprintf(out, cap, "%ldj", d / 86400);
}

static void fmt_clock(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static const char *region_str(uint32_t r)
{
    switch (r) {
        case 1:  return "US";
        case 2:  return "EU433";
        case 3:  return "EU868";
        case 4:  return "CN";
        case 29: return "EU866";
        default: return "?";
    }
}

static const char *preset_str(uint32_t p)
{
    switch (p) {
        case 0: return "LongFast";
        case 1: return "LongSlow";
        case 3: return "MediumSlow";
        case 4: return "MediumFast";
        case 5: return "ShortSlow";
        case 6: return "ShortFast";
        case 8: return "ShortTurbo";
        case 9: return "LongTurbo";
        default: return "Preset";
    }
}

/* ----------------------------------------------------------------- persistance nodes */
/* Découpe une ligne en colonnes séparées par TAB (in-place). La dernière
 * colonne (nom) conserve ses espaces ; on retire le saut de ligne final. */
static int split_tabs(char *s, char *out[], int maxf)
{
    int n = 0;
    out[n++] = s;
    for (; *s && n < maxf; s++)
        if (*s == '\t') { *s = '\0'; out[n++] = s + 1; }
    char *lastf = out[n - 1];
    size_t L = strlen(lastf);
    while (L && (lastf[L - 1] == '\n' || lastf[L - 1] == '\r')) lastf[--L] = '\0';
    return n;
}

static node_slot_t *node_get_or_add(uint32_t num);  /* fwd */

/* Écriture atomique (tmp + rename) de la trace des nœuds. */
static void mesh_nodes_save(void)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", NODES_DB_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# bq-lora-ui nodes v1: num\\tid\\tfirst\\tlast\\tbest_snr\\tname\n");
    for (int i = 0; i < s_node_count; i++) {
        node_slot_t *s = &s_nodes[i];
        fprintf(f, "%u\t%s\t%u\t%u\t%d\t%s\n",
                s->num, s->id, s->first_heard, s->last_heard,
                (int)s->best_snr, s->name);
    }
    fclose(f);
    rename(tmp, NODES_DB_PATH);
    s_nodes_save_pending = false;
    s_nodes_last_save = time(NULL);
}

/* Echappe tabs/newlines pour serialisation TSV (in-place ; suffisamment large). */
static void escape_tsv(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    for (; src && *src && k < cap - 1; src++) {
        char c = *src;
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        dst[k++] = c;
    }
    dst[k] = '\0';
}

/* Sauvegarde l'historique des messages (jusqu'a MAX_MSGS = 128).
 * Format : ts\tch\tout\tack\tfrom\ttext  (un message par ligne). */
static void mesh_msgs_save(void)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", MSGS_DB_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# bq-lora-ui messages v1: time\\tch\\tout\\tack\\tfrom\\ttext\n");
    char fbuf[80], tbuf[200];
    for (int i = 0; i < s_msg_count; i++) {
        msg_slot_t *s = &s_msgs[i];
        escape_tsv(fbuf, sizeof(fbuf), s->from);
        escape_tsv(tbuf, sizeof(tbuf), s->m.text);
        fprintf(f, "%s\t%u\t%u\t%u\t%s\t%s\n",
                s->time, (unsigned)s->m.ch, (unsigned)s->m.out, (unsigned)s->m.ack,
                fbuf, tbuf);
    }
    fclose(f);
    rename(tmp, MSGS_DB_PATH);
    s_msgs_save_pending = false;
    s_msgs_last_save = time(NULL);
}

static void mesh_msgs_load(void)
{
    FILE *f = fopen(MSGS_DB_PATH, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof(line), f) && s_msg_count < MAX_MSGS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *c[6];
        if (split_tabs(line, c, 6) < 6) continue;
        msg_slot_t *s = &s_msgs[s_msg_count++];
        memset(s, 0, sizeof(*s));
        snprintf(s->time, sizeof(s->time), "%s", c[0]);
        s->m.time = s->time;
        s->m.ch   = (uint8_t)atoi(c[1]);
        s->m.out  = atoi(c[2]) ? true : false;
        s->m.ack  = (uint8_t)atoi(c[3]);
        snprintf(s->from, sizeof(s->from), "%s", c[4]);
        s->m.from = s->from;
        size_t n = strlen(c[5]);
        if (n >= sizeof(s->m.text)) n = sizeof(s->m.text) - 1;
        memcpy(s->m.text, c[5], n);
        s->m.text[n] = '\0';
    }
    fclose(f);
    s_msgs_save_pending = false;
}

/* Recharge la trace au démarrage : la liste est visible avant le handshake. */
static void mesh_nodes_load(void)
{
    FILE *f = fopen(NODES_DB_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *c[6];
        if (split_tabs(line, c, 6) < 6) continue;
        uint32_t num = (uint32_t)strtoul(c[0], NULL, 10);
        if (!num) continue;
        node_slot_t *s = node_get_or_add(num);
        if (!s) break;
        if (c[1][0]) snprintf(s->id, sizeof(s->id), "%s", c[1]);
        s->first_heard = (uint32_t)strtoul(c[2], NULL, 10);
        s->last_heard  = (uint32_t)strtoul(c[3], NULL, 10);
        s->best_snr    = (int8_t)atoi(c[4]);
        if (c[5][0]) snprintf(s->name, sizeof(s->name), "%s", c[5]);
    }
    fclose(f);
    s_nodes_save_pending = false;     /* on vient de charger : rien à réécrire */
}

/* ----------------------------------------------------------------- nodes */
static node_slot_t *node_get_or_add(uint32_t num)
{
    for (int i = 0; i < s_node_count; i++)
        if (s_nodes[i].num == num) return &s_nodes[i];
    if (s_node_count >= MAX_NODES) return NULL;
    node_slot_t *s = &s_nodes[s_node_count++];
    memset(s, 0, sizeof(*s));
    s->num = num;
    s->first_heard = (uint32_t)time(NULL);
    s->best_snr = INT8_MIN;          /* inconnu tant qu'aucun SNR vu */
    snprintf(s->id, sizeof(s->id), "!%08x", num);
    snprintf(s->name, sizeof(s->name), "%s", s->id);
    s->pub.num  = num;
    s->pub.id   = s->id;
    s->pub.name = s->name;
    s->pub.last = s->last;
    s_nodes_save_pending = true;      /* nouveau nœud -> persister */
    return s;
}

/* Enregistre un SNR : met à jour le dernier ET le record (best). */
static void node_note_snr(node_slot_t *s, float snr)
{
    int8_t v = (int8_t)(snr >= 0 ? snr + 0.5f : snr - 0.5f);
    s->pub.snr = v;
    if (s->best_snr == INT8_MIN || v > s->best_snr) s->best_snr = v;
}

static const char *node_name(uint32_t num)
{
    if (num == s_my_num && s_my_num) return settings_node_name();
    for (int i = 0; i < s_node_count; i++)
        if (s_nodes[i].num == num) return s_nodes[i].name;
    return NULL;
}

static void node_update_signal(uint32_t num, bool have_snr, float snr, int rssi)
{
    node_slot_t *s = node_get_or_add(num);
    if (!s) return;
    if (have_snr) node_note_snr(s, snr);
    if (rssi)     s->pub.rssi = (int16_t)rssi;
    s->last_heard = (uint32_t)time(NULL);
    s_nodes_save_pending = true;
}

/* ----------------------------------------------------------------- channels */
static int chan_pos_from_index(int index)
{
    for (int i = 0; i < s_chan_count; i++)
        if (s_chans[i].index == index) return i;
    return 0; /* défaut : canal principal */
}

/* ----------------------------------------------------------------- messages */
static void add_text(int chan_index, uint32_t from, uint32_t id,
                     const uint8_t *txt, size_t len)
{
    /* dédup : si un message porte déjà cet id (echo de notre propre TX,
       retransmission radio…), on l'ignore. */
    if (id) {
        for (int i = 0; i < s_msg_count; i++)
            if (s_msgs[i].id == id) return;
    }

    if (s_msg_count >= MAX_MSGS) {
        memmove(&s_msgs[0], &s_msgs[1], sizeof(s_msgs[0]) * (MAX_MSGS - 1));
        s_msg_count--;
    }

    msg_slot_t *m = &s_msgs[s_msg_count++];
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->m.ch = (uint8_t)chan_pos_from_index(chan_index);

    const char *nm = node_name(from);
    if (nm) snprintf(m->from, sizeof(m->from), "%s", nm);
    else    snprintf(m->from, sizeof(m->from), "!%08x", from);
    m->m.from = m->from;

    size_t n = len < sizeof(m->m.text) - 1 ? len : sizeof(m->m.text) - 1;
    memcpy(m->m.text, txt, n);
    m->m.text[n] = '\0';

    fmt_clock(m->time, sizeof(m->time));
    m->m.time = m->time;

    m->m.out = (from == s_my_num && s_my_num);
    m->m.ack = m->m.out ? 1 : 0;
    if (!m->m.out) {
        s_rx_total++;                /* message entrant -> incremente les non-lus */
        if (m->m.ch < MAX_CHANS) s_rx_per_ch[m->m.ch]++;
    }
    s_msgs_save_pending = true;      /* persistance differee */
    s_dirty = true;
}

static void ack_message(uint32_t request_id)
{
    if (!request_id) return;
    for (int i = 0; i < s_msg_count; i++) {
        if (s_msgs[i].id == request_id && s_msgs[i].m.out) {
            if (s_msgs[i].m.ack != 2) { s_msgs[i].m.ack = 2; s_dirty = true; }
            return;
        }
    }
}

/* ----------------------------------------------------------------- parseurs */
static void parse_user(const uint8_t *d, size_t n, node_slot_t *s)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    char idbuf[16] = "", longn[40] = "", shortn[16] = "";
    while (pb_read_tag(&r, &f, &w)) {
        const uint8_t *b; size_t bl;
        if (f == 1 && w == 2 && pb_read_bytes(&r, &b, &bl)) {
            size_t k = bl < sizeof(idbuf) - 1 ? bl : sizeof(idbuf) - 1;
            memcpy(idbuf, b, k); idbuf[k] = '\0';
        } else if (f == 2 && w == 2 && pb_read_bytes(&r, &b, &bl)) {
            size_t k = bl < sizeof(longn) - 1 ? bl : sizeof(longn) - 1;
            memcpy(longn, b, k); longn[k] = '\0';
        } else if (f == 3 && w == 2 && pb_read_bytes(&r, &b, &bl)) {
            size_t k = bl < sizeof(shortn) - 1 ? bl : sizeof(shortn) - 1;
            memcpy(shortn, b, k); shortn[k] = '\0';
        } else {
            pb_skip(&r, w);
        }
    }
    if (idbuf[0]) snprintf(s->id, sizeof(s->id), "%s", idbuf);
    if (longn[0])      snprintf(s->name, sizeof(s->name), "%s", longn);
    else if (shortn[0])snprintf(s->name, sizeof(s->name), "%s", shortn);
}

static void parse_devmetrics(const uint8_t *d, size_t n, uint32_t num)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    uint8_t batt = 0; bool have_batt = false;
    float volt = 0, cu = 0, at = 0; uint32_t up = 0;
    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; uint32_t fx;
        if (f == 1 && w == 0 && pb_read_varint(&r, &v)) { batt = (uint8_t)v; have_batt = true; }
        else if (f == 2 && w == 5 && pb_read_fixed32(&r, &fx)) volt = pb_as_float(fx);
        else if (f == 3 && w == 5 && pb_read_fixed32(&r, &fx)) cu = pb_as_float(fx);
        else if (f == 4 && w == 5 && pb_read_fixed32(&r, &fx)) at = pb_as_float(fx);
        else if (f == 5 && w == 0 && pb_read_varint(&r, &v)) up = (uint32_t)v;
        else pb_skip(&r, w);
    }
    node_slot_t *s = node_get_or_add(num);
    if (s && have_batt) s->pub.batt = batt;

    if (num == s_my_num && s_my_num) {
        if (have_batt) s_self.batt = batt;
        s_self.volt = volt;
        s_self.chan_util = cu;
        s_self.air_tx = at;
        if (up) {
            snprintf(s_uptime, sizeof(s_uptime), "%uh %um",
                     up / 3600, (up % 3600) / 60);
            s_self.uptime = s_uptime;
        }
    }
}

static void parse_nodeinfo(const uint8_t *d, size_t n)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    uint32_t num = 0; bool have_num = false;
    const uint8_t *user = NULL; size_t userl = 0;
    const uint8_t *dm = NULL;   size_t dml = 0;
    bool have_snr = false; float snr = 0;
    uint32_t last_heard = 0, hops = 0; bool have_hops = false;

    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; uint32_t fx; const uint8_t *b; size_t bl;
        if (f == 1 && w == 0 && pb_read_varint(&r, &v)) { num = (uint32_t)v; have_num = true; }
        else if (f == 2 && w == 2 && pb_read_bytes(&r, &b, &bl)) { user = b; userl = bl; }
        else if (f == 4 && w == 5 && pb_read_fixed32(&r, &fx)) { snr = pb_as_float(fx); have_snr = true; }
        else if (f == 5 && w == 5 && pb_read_fixed32(&r, &fx)) { last_heard = fx; }
        else if (f == 6 && w == 2 && pb_read_bytes(&r, &b, &bl)) { dm = b; dml = bl; }
        else if (f == 9 && w == 0 && pb_read_varint(&r, &v)) { hops = (uint32_t)v; have_hops = true; }
        else pb_skip(&r, w);
    }
    if (!have_num) return;

    node_slot_t *s = node_get_or_add(num);
    if (!s) return;
    if (user) parse_user(user, userl, s);
    if (have_snr) node_note_snr(s, snr);
    if (have_hops) s->pub.hops = (uint8_t)hops;
    if (last_heard) s->last_heard = last_heard;
    s->pub.self = (num == s_my_num && s_my_num);
    if (dm) parse_devmetrics(dm, dml, num);
    s_nodes_save_pending = true;
    s_dirty = true;
}

static void parse_data(const uint8_t *d, size_t n,
                       uint32_t from, uint32_t chan, uint32_t pkt_id)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    uint32_t portnum = 0, req_id = 0;
    const uint8_t *payload = NULL; size_t pll = 0;

    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; uint32_t fx; const uint8_t *b; size_t bl;
        if (f == 1 && w == 0 && pb_read_varint(&r, &v)) portnum = (uint32_t)v;
        else if (f == 2 && w == 2 && pb_read_bytes(&r, &b, &bl)) { payload = b; pll = bl; }
        else if (f == 6 && w == 5 && pb_read_fixed32(&r, &fx)) req_id = fx;
        else pb_skip(&r, w);
    }

    if (portnum == PORT_TEXT && payload)
        add_text((int)chan, from, pkt_id, payload, pll);
    else if (portnum == PORT_ROUTING)
        ack_message(req_id);
}

static void parse_packet(const uint8_t *d, size_t n)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    uint32_t from = 0, chan = 0, pkt_id = 0;
    int rssi = 0; float snr = 0; bool have_snr = false;
    const uint8_t *dec = NULL; size_t decl = 0;

    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; uint32_t fx; const uint8_t *b; size_t bl;
        if (f == 1 && w == 5 && pb_read_fixed32(&r, &fx)) from = fx;
        else if (f == 3 && w == 0 && pb_read_varint(&r, &v)) chan = (uint32_t)v;
        else if (f == 4 && w == 2 && pb_read_bytes(&r, &b, &bl)) { dec = b; decl = bl; }
        else if (f == 6 && w == 5 && pb_read_fixed32(&r, &fx)) pkt_id = fx;
        else if (f == 8 && w == 5 && pb_read_fixed32(&r, &fx)) { snr = pb_as_float(fx); have_snr = true; }
        else if (f == 12 && w == 0 && pb_read_varint(&r, &v)) rssi = (int)(int32_t)(uint32_t)v;
        else pb_skip(&r, w);
    }

    if (from && from != s_my_num) node_update_signal(from, have_snr, snr, rssi);
    if (dec) parse_data(dec, decl, from, chan, pkt_id);
}

static void parse_channel(const uint8_t *d, size_t n)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    int index = 0; uint32_t role = 0;
    char name[24] = ""; bool enc = false;
    uint8_t psk[32]; size_t psk_len = 0;
    const uint8_t *settings = NULL; size_t setl = 0;

    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; const uint8_t *b; size_t bl;
        if (f == 1 && w == 0 && pb_read_varint(&r, &v)) index = (int)(int32_t)(uint32_t)v;
        else if (f == 2 && w == 2 && pb_read_bytes(&r, &b, &bl)) { settings = b; setl = bl; }
        else if (f == 3 && w == 0 && pb_read_varint(&r, &v)) role = (uint32_t)v;
        else pb_skip(&r, w);
    }

    /* DISABLED : retirer l'entrée si on l'avait (cas d'une suppression). */
    if (role == 0) {
        for (int i = 0; i < s_chan_count; i++) {
            if (s_chans[i].index == index) {
                memmove(&s_chans[i], &s_chans[i + 1],
                        sizeof(s_chans[0]) * (s_chan_count - i - 1));
                s_chan_count--;
                s_dirty = true;
                break;
            }
        }
        return;
    }

    if (settings) {
        pb_reader sr; pb_reader_init(&sr, settings, setl);
        uint32_t sf, sw;
        while (pb_read_tag(&sr, &sf, &sw)) {
            const uint8_t *b; size_t bl;
            if (sf == 2 && sw == 2 && pb_read_bytes(&sr, &b, &bl)) {
                enc = (bl > 1);   /* psk > 1 octet = clé custom */
                psk_len = bl < sizeof(psk) ? bl : sizeof(psk);
                memcpy(psk, b, psk_len);
            } else if (sf == 3 && sw == 2 && pb_read_bytes(&sr, &b, &bl)) {
                size_t k = bl < sizeof(name) - 1 ? bl : sizeof(name) - 1;
                memcpy(name, b, k); name[k] = '\0';
            } else {
                pb_skip(&sr, sw);
            }
        }
    }
    if (!name[0]) snprintf(name, sizeof(name), "%s", index == 0 ? s_preset : "Canal");

    /* upsert par index */
    chan_slot_t *c = NULL;
    for (int i = 0; i < s_chan_count; i++)
        if (s_chans[i].index == index) { c = &s_chans[i]; break; }
    if (!c) {
        if (s_chan_count >= MAX_CHANS) return;
        c = &s_chans[s_chan_count++];
        c->index = index;
    }
    snprintf(c->name, sizeof(c->name), "%s", name);
    memcpy(c->psk, psk, psk_len);
    c->psk_len = psk_len;
    c->role = (int)role;
    c->pub.name  = c->name;
    c->pub.enc   = enc;
    c->pub.index = (uint8_t)index;
    c->pub.role  = (uint8_t)role;
    s_dirty = true;
}

static void parse_config(const uint8_t *d, size_t n)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    while (pb_read_tag(&r, &f, &w)) {
        const uint8_t *b; size_t bl;
        if (f == 6 && w == 2 && pb_read_bytes(&r, &b, &bl)) {  /* LoRaConfig */
            pb_reader lr; pb_reader_init(&lr, b, bl);
            uint32_t lf, lw;
            while (pb_read_tag(&lr, &lf, &lw)) {
                uint64_t v;
                if (lf == 2 && lw == 0 && pb_read_varint(&lr, &v)) {
                    s_preset_code = (uint32_t)v;
                    snprintf(s_preset, sizeof(s_preset), "%s", preset_str((uint32_t)v));
                    s_self.preset = s_preset;
                } else if (lf == 7 && lw == 0 && pb_read_varint(&lr, &v)) {
                    s_region_code = (uint32_t)v;
                    snprintf(s_region, sizeof(s_region), "%s", region_str((uint32_t)v));
                    s_self.region = s_region;
                } else if (lf == 8 && lw == 0 && pb_read_varint(&lr, &v)) {  /* hop_limit */
                    s_self.hop_limit = (uint8_t)v;
                } else if (lf == 10 && lw == 0 && pb_read_varint(&lr, &v)) { /* tx_power dBm */
                    s_self.tx_power = (int)(int32_t)v;
                } else {
                    pb_skip(&lr, lw);
                }
            }
        } else {
            pb_skip(&r, w);
        }
    }
}

static void parse_fromradio(const uint8_t *d, size_t n)
{
    pb_reader r; pb_reader_init(&r, d, n);
    uint32_t f, w;
    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; const uint8_t *b; size_t bl;
        if (f == 2 && w == 2 && pb_read_bytes(&r, &b, &bl)) parse_packet(b, bl);
        else if (f == 3 && w == 2 && pb_read_bytes(&r, &b, &bl)) {  /* my_info */
            pb_reader mr; pb_reader_init(&mr, b, bl);
            uint32_t mf, mw;
            while (pb_read_tag(&mr, &mf, &mw)) {
                uint64_t mv;
                if (mf == 1 && mw == 0 && pb_read_varint(&mr, &mv)) s_my_num = (uint32_t)mv;
                else pb_skip(&mr, mw);
            }
        }
        else if (f == 4 && w == 2 && pb_read_bytes(&r, &b, &bl)) parse_nodeinfo(b, bl);
        else if (f == 5 && w == 2 && pb_read_bytes(&r, &b, &bl)) parse_config(b, bl);
        else if (f == 7 && w == 0 && pb_read_varint(&r, &v)) { s_configured = true; s_dirty = true; }
        else if (f == 10 && w == 2 && pb_read_bytes(&r, &b, &bl)) parse_channel(b, bl);
        else pb_skip(&r, w);
    }
}

/* ----------------------------------------------------------------- envoi */
static void send_frame(const uint8_t *payload, size_t len)
{
    if (s_fd < 0 || len > 0xFFFF) return;
    uint8_t hdr[4] = { FR_START1, FR_START2, (uint8_t)(len >> 8), (uint8_t)(len & 0xff) };

    uint8_t out[2 + 2 + 512];
    if (len + 4 > sizeof(out)) return;
    memcpy(out, hdr, 4);
    memcpy(out + 4, payload, len);

    size_t total = len + 4, off = 0;
    for (int tries = 0; off < total && tries < 64; tries++) {
        ssize_t k = send(s_fd, out + off, total - off, MSG_NOSIGNAL);
        if (k > 0) { off += (size_t)k; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break; /* erreur : on laissera la lecture détecter la coupure */
    }
}

static void send_want_config(void)
{
    uint8_t buf[16];
    pb_writer w; pb_writer_init(&w, buf, sizeof(buf));
    pb_field_varint(&w, 3, (uint32_t)time(NULL)); /* want_config_id */
    if (!w.ovf) send_frame(buf, w.len);
}

static void send_heartbeat(void)
{
    uint8_t buf[8];
    pb_writer w; pb_writer_init(&w, buf, sizeof(buf));
    pb_field_bytes(&w, 7, NULL, 0);              /* heartbeat = sous-message vide */
    if (!w.ovf) send_frame(buf, w.len);
}

void mesh_send(uint8_t ch, const char *text)
{
    if (!text || !text[0]) return;
    size_t tlen = strlen(text);
    if (tlen > 200) tlen = 200;

    int real_index = (ch < s_chan_count) ? s_chans[ch].index : ch;
    uint32_t id = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ (uint32_t)time(NULL);
    if (!id) id = 1;

    /* Data { portnum=TEXT, payload=text } */
    uint8_t data[256];
    pb_writer dw; pb_writer_init(&dw, data, sizeof(data));
    pb_field_varint(&dw, 1, PORT_TEXT);
    pb_field_bytes(&dw, 2, (const uint8_t *)text, tlen);

    /* MeshPacket { to=BCAST, channel, decoded=Data, id, want_ack } */
    uint8_t pkt[320];
    pb_writer pw; pb_writer_init(&pw, pkt, sizeof(pkt));
    pb_field_fixed32(&pw, 2, BROADCAST_ADDR);
    pb_field_varint(&pw, 3, (uint32_t)real_index);
    pb_field_bytes(&pw, 4, data, dw.len);
    pb_field_fixed32(&pw, 6, id);
    pb_field_bool(&pw, 10, true);

    /* ToRadio { packet=MeshPacket } */
    uint8_t tr[400];
    pb_writer tw; pb_writer_init(&tw, tr, sizeof(tr));
    pb_field_bytes(&tw, 1, pkt, pw.len);

    if (!dw.ovf && !pw.ovf && !tw.ovf)
        send_frame(tr, tw.len);

    /* écho optimiste local (l'echo radio sera dédupliqué via id) */
    if (s_msg_count >= MAX_MSGS) {
        memmove(&s_msgs[0], &s_msgs[1], sizeof(s_msgs[0]) * (MAX_MSGS - 1));
        s_msg_count--;
    }
    msg_slot_t *m = &s_msgs[s_msg_count++];
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->m.ch = ch;
    snprintf(m->from, sizeof(m->from), "%s", settings_node_name());
    m->m.from = m->from;
    memcpy(m->m.text, text, tlen);
    m->m.text[tlen] = '\0';
    fmt_clock(m->time, sizeof(m->time));
    m->m.time = m->time;
    m->m.out = true;
    m->m.ack = 1;
    s_msgs_save_pending = true;
    s_dirty = true;
}

/* DM : meme structure qu'un broadcast mais champ "to" = dest_num. Canal = 0
 * (default) car les DM passent normalement sur le primary, ignore les autres. */
void mesh_send_dm(uint32_t dest_num, const char *text)
{
    if (!text || !text[0]) return;
    if (!dest_num) { mesh_send(0, text); return; }
    size_t tlen = strlen(text);
    if (tlen > 200) tlen = 200;

    int real_index = (s_chan_count > 0) ? s_chans[0].index : 0;
    uint32_t id = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ (uint32_t)time(NULL);
    if (!id) id = 1;

    uint8_t data[256];
    pb_writer dw; pb_writer_init(&dw, data, sizeof(data));
    pb_field_varint(&dw, 1, PORT_TEXT);
    pb_field_bytes(&dw, 2, (const uint8_t *)text, tlen);

    uint8_t pkt[320];
    pb_writer pw; pb_writer_init(&pw, pkt, sizeof(pkt));
    pb_field_fixed32(&pw, 2, dest_num);          /* DM : destinataire specifique */
    pb_field_varint(&pw, 3, (uint32_t)real_index);
    pb_field_bytes(&pw, 4, data, dw.len);
    pb_field_fixed32(&pw, 6, id);
    pb_field_bool(&pw, 10, true);

    uint8_t tr[400];
    pb_writer tw; pb_writer_init(&tw, tr, sizeof(tr));
    pb_field_bytes(&tw, 1, pkt, pw.len);
    if (!dw.ovf && !pw.ovf && !tw.ovf) send_frame(tr, tw.len);

    /* echo local : on l'attache au canal 0 (l'UI traitera le DM comme un */
    /* message texte sortant, avec destinataire connu via le 'to' du paquet) */
    if (s_msg_count >= MAX_MSGS) {
        memmove(&s_msgs[0], &s_msgs[1], sizeof(s_msgs[0]) * (MAX_MSGS - 1));
        s_msg_count--;
    }
    msg_slot_t *m = &s_msgs[s_msg_count++];
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->m.ch = 0;
    const char *nm = node_name(dest_num);
    if (nm) snprintf(m->from, sizeof(m->from), "%s", nm);
    else    snprintf(m->from, sizeof(m->from), "!%08x", dest_num);
    m->m.from = m->from;
    memcpy(m->m.text, text, tlen);
    m->m.text[tlen] = '\0';
    fmt_clock(m->time, sizeof(m->time));
    m->m.time = m->time;
    m->m.out = true;
    m->m.ack = 1;
    s_msgs_save_pending = true;
    s_dirty = true;
}

/* ----------------------------------------------------------------- socket */
static void mesh_close(void)
{
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    s_connecting = false;
    s_configured = false;
    s_rxlen = 0;
}

static void mesh_try_connect(void)
{
    s_last_attempt = time(NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MESH_PORT);
    inet_pton(AF_INET, MESH_HOST, &addr.sin_addr);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        s_fd = fd; s_connecting = false; s_configured = false;
        s_last_hb = time(NULL);
        send_want_config();
    } else if (errno == EINPROGRESS) {
        s_fd = fd; s_connecting = true; s_connect_started = time(NULL);
    } else {
        close(fd);
    }
}

static void mesh_check_connect(void)
{
    struct pollfd p = { .fd = s_fd, .events = POLLOUT };
    int r = poll(&p, 1, 0);
    if (r > 0 && (p.revents & POLLOUT)) {
        int err = 0; socklen_t l = sizeof(err);
        getsockopt(s_fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err == 0) {
            s_connecting = false;
            s_last_hb = time(NULL);
            send_want_config();
        } else {
            mesh_close();
        }
    } else if (time(NULL) - s_connect_started > CONNECT_TMO_SECS) {
        mesh_close();
    }
}

static void mesh_drain(void)
{
    for (;;) {
        if (s_rxlen >= RXBUF_SZ) { s_rxlen = 0; }  /* garde-fou : on resync */
        ssize_t k = recv(s_fd, s_rx + s_rxlen, RXBUF_SZ - s_rxlen, 0);
        if (k > 0) { s_rxlen += (size_t)k; continue; }
        if (k == 0) { mesh_close(); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        mesh_close(); return;
    }

    /* extraire les trames complètes */
    size_t off = 0;
    while (s_rxlen - off >= 4) {
        if (s_rx[off] != FR_START1 || s_rx[off + 1] != FR_START2) { off++; continue; }
        size_t plen = ((size_t)s_rx[off + 2] << 8) | s_rx[off + 3];
        if (s_rxlen - off - 4 < plen) break;       /* trame incomplète */
        parse_fromradio(s_rx + off + 4, plen);
        off += 4 + plen;
    }
    if (off > 0) {
        memmove(s_rx, s_rx + off, s_rxlen - off);
        s_rxlen -= off;
    }
}

/* ----------------------------------------------------------------- API publique */
void mesh_init(void)
{
    srand((unsigned)time(NULL));
    memset(&s_self, 0, sizeof(s_self));
    s_self.region = s_region;
    s_self.preset = s_preset;
    s_self.uptime = s_uptime;
    mesh_nodes_load();           /* trace locale : nodes connus visibles de suite */
    mesh_msgs_load();            /* historique : messages visibles avant la reconnexion */
    if (s_enabled) mesh_try_connect();
}

void mesh_poll(void)
{
    if (!s_enabled) return;          /* liaison libérée : on laisse le port à un autre client */

    time_t now = time(NULL);

    /* Persistance throttlée de la trace des nœuds (au plus 1 écriture / 30 s). */
    if (s_nodes_save_pending && now - s_nodes_last_save >= NODES_SAVE_MIN_SECS)
        mesh_nodes_save();
    /* Idem pour l'historique des messages. */
    if (s_msgs_save_pending && now - s_msgs_last_save >= MSGS_SAVE_MIN_SECS)
        mesh_msgs_save();

    if (s_fd < 0) {
        if (now - s_last_attempt >= RECONNECT_SECS) mesh_try_connect();
        return;
    }
    if (s_connecting) { mesh_check_connect(); return; }

    mesh_drain();
    if (s_fd < 0) return;

    /* recharger la config après une modification de canal (laisse le nœud committer) */
    if (s_reload_at && now >= s_reload_at) {
        s_reload_at = 0;
        send_want_config();
    }

    if (now - s_last_hb >= HEARTBEAT_SECS) {
        send_heartbeat();
        s_last_hb = now;
    }
}

bool mesh_connected(void) { return s_fd >= 0 && s_configured; }

void mesh_set_enabled(bool en)
{
    if (en == s_enabled) return;
    s_enabled = en;
    if (!en) {
        mesh_close();            /* ferme le socket -> libère le port 4403 */
    } else {
        s_last_attempt = 0;      /* force une reconnexion immédiate au prochain poll */
    }
    s_dirty = true;              /* l'UI rafraîchit l'indicateur de lien */
}

bool mesh_enabled(void) { return s_enabled; }

bool mesh_take_dirty(void)
{
    bool d = s_dirty;
    s_dirty = false;
    return d;
}

/* ----------------------------------------------------------------- accesseurs */
int mesh_channel_count(void) { return s_chan_count; }

const mesh_channel_t *mesh_channel(int i)
{
    if (i < 0 || i >= s_chan_count) return NULL;
    return &s_chans[i].pub;
}

unsigned mesh_rx_msg_total(void) { return s_rx_total; }
unsigned mesh_rx_msg_count(uint8_t ch) {
    return (ch < MAX_CHANS) ? s_rx_per_ch[ch] : 0;
}

int mesh_node_count(void) { return s_node_count; }

const mesh_node_t *mesh_node(int i)
{
    if (i < 0 || i >= s_node_count) return NULL;
    node_slot_t *s = &s_nodes[i];
    s->pub.self        = (s->num == s_my_num && s_my_num);
    s->pub.num         = s->num;
    s->pub.first_heard = s->first_heard;
    s->pub.last_heard  = s->last_heard;
    s->pub.best_snr    = s->best_snr;
    fmt_rel(s->last, sizeof(s->last), s->last_heard);
    return &s->pub;
}

int mesh_message_count(uint8_t ch)
{
    int n = 0;
    for (int i = 0; i < s_msg_count; i++)
        if (s_msgs[i].m.ch == ch) n++;
    return n;
}

const mesh_message_t *mesh_message(uint8_t ch, int idx)
{
    int n = 0;
    for (int i = 0; i < s_msg_count; i++) {
        if (s_msgs[i].m.ch == ch) {
            if (n == idx) return &s_msgs[i].m;
            n++;
        }
    }
    return NULL;
}

const mesh_self_t *mesh_self(void)
{
    s_self.nodes = s_node_count;
    return &s_self;
}

/* ============================================================ gestion canaux */

/* --- base64 url-safe (RFC 4648 §5), sans padding --- */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        int rem = (int)(n - i);
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        int chars = rem >= 3 ? 4 : rem + 1;     /* 1->2, 2->3, 3->4 (sans padding) */
        for (int k = 0; k < chars && o < cap - 1; k++)
            out[o++] = B64[(v >> (18 - 6 * k)) & 0x3f];
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;                                   /* '=', espaces, etc. : ignorés */
}

static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (const char *p = in; *p; p++) {
        int v = b64val(*p);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < cap) out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return o;
}

static void fill_random(uint8_t *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t k = read(fd, buf + got, n - got);
            if (k <= 0) break;
            got += (size_t)k;
        }
        close(fd);
    }
    for (; got < n; got++) buf[got] = (uint8_t)(rand() & 0xff);  /* repli */
}

/* --- envoi d'un AdminMessage au nœud local (to = soi-même, portnum ADMIN) --- */
static void send_admin(const uint8_t *admin, size_t alen)
{
    if (s_fd < 0 || s_my_num == 0) return;

    uint8_t data[300];
    pb_writer dw; pb_writer_init(&dw, data, sizeof(data));
    pb_field_varint(&dw, 1, PORT_ADMIN);          /* Data.portnum = ADMIN_APP */
    pb_field_bytes (&dw, 2, admin, alen);         /* Data.payload = AdminMessage */

    uint8_t pkt[360];
    pb_writer pw; pb_writer_init(&pw, pkt, sizeof(pkt));
    pb_field_fixed32(&pw, 2, s_my_num);           /* MeshPacket.to = nœud local */
    pb_field_varint (&pw, 3, 0);                  /* channel 0 */
    pb_field_bytes  (&pw, 4, data, dw.len);       /* decoded */
    pb_field_fixed32(&pw, 6, (uint32_t)rand() ^ (uint32_t)time(NULL));

    uint8_t tr[420];
    pb_writer tw; pb_writer_init(&tw, tr, sizeof(tr));
    pb_field_bytes(&tw, 1, pkt, pw.len);          /* ToRadio.packet */

    if (!dw.ovf && !pw.ovf && !tw.ovf) send_frame(tr, tw.len);
}

static void admin_flag(uint32_t field)            /* begin(64)/commit(65)_edit_settings */
{
    uint8_t am[8];
    pb_writer aw; pb_writer_init(&aw, am, sizeof(am));
    pb_field_bool(&aw, field, true);
    send_admin(am, aw.len);
}

/* AdminMessage{ set_channel(33): Channel{index, settings{psk,name}, role} } */
static void admin_set_channel(int index, const char *name,
                              const uint8_t *psk, size_t psk_len, int role)
{
    uint8_t cs[80];
    pb_writer cw; pb_writer_init(&cw, cs, sizeof(cs));
    pb_field_bytes(&cw, 2, psk, psk_len);
    if (name && name[0]) pb_field_bytes(&cw, 3, (const uint8_t *)name, strlen(name));

    uint8_t ch[120];
    pb_writer hw; pb_writer_init(&hw, ch, sizeof(ch));
    pb_field_varint(&hw, 1, (uint32_t)index);
    pb_field_bytes (&hw, 2, cs, cw.len);
    pb_field_varint(&hw, 3, (uint32_t)role);

    uint8_t am[160];
    pb_writer aw; pb_writer_init(&aw, am, sizeof(am));
    pb_field_bytes(&aw, 33, ch, hw.len);
    send_admin(am, aw.len);
}

static void schedule_reload(void) { s_reload_at = time(NULL) + 1; }

/* premier index libre dans [1..MAX_CHANS-1] non utilisé localement, -1 si plein */
static int free_channel_index(void)
{
    for (int idx = 1; idx < MAX_CHANS; idx++) {
        bool used = false;
        for (int i = 0; i < s_chan_count; i++)
            if (s_chans[i].index == idx) { used = true; break; }
        if (!used) return idx;
    }
    return -1;
}

bool mesh_channel_create(const char *name, bool encrypted)
{
    if (!mesh_connected() || !name || !name[0]) return false;
    int idx = free_channel_index();
    if (idx < 0) return false;

    uint8_t psk[32]; size_t plen;
    if (encrypted) { fill_random(psk, 32); plen = 32; }
    else           { psk[0] = 0x01;        plen = 1; }   /* clé par défaut -> public */

    admin_flag(64);
    admin_set_channel(idx, name, psk, plen, 2 /*SECONDARY*/);
    admin_flag(65);
    schedule_reload();
    return true;
}

bool mesh_channel_rename(int i, const char *name)
{
    if (!mesh_connected() || i < 0 || i >= s_chan_count || !name || !name[0]) return false;
    chan_slot_t *c = &s_chans[i];
    admin_flag(64);
    admin_set_channel(c->index, name, c->psk, c->psk_len, c->role);
    admin_flag(65);
    schedule_reload();
    return true;
}

bool mesh_channel_delete(int i)
{
    if (!mesh_connected() || i < 0 || i >= s_chan_count) return false;
    chan_slot_t *c = &s_chans[i];
    if (c->role == 1) return false;                      /* primaire non supprimable */
    admin_flag(64);
    admin_set_channel(c->index, NULL, NULL, 0, 0 /*DISABLED*/);
    admin_flag(65);
    schedule_reload();
    return true;
}

const char *mesh_channel_share_url(int i)
{
    static char url[256];
    if (i < 0 || i >= s_chan_count) return NULL;
    chan_slot_t *c = &s_chans[i];

    uint8_t cs[80];                                      /* ChannelSettings{psk,name} */
    pb_writer cw; pb_writer_init(&cw, cs, sizeof(cs));
    pb_field_bytes(&cw, 2, c->psk, c->psk_len);
    pb_field_bytes(&cw, 3, (const uint8_t *)c->name, strlen(c->name));

    uint8_t lc[16];                                      /* LoRaConfig{preset,region} */
    pb_writer lw; pb_writer_init(&lw, lc, sizeof(lc));
    pb_field_varint(&lw, 2, s_preset_code);
    pb_field_varint(&lw, 7, s_region_code);

    uint8_t set[128];                                    /* ChannelSet{settings,lora} */
    pb_writer sw; pb_writer_init(&sw, set, sizeof(set));
    pb_field_bytes(&sw, 1, cs, cw.len);
    pb_field_bytes(&sw, 2, lc, lw.len);
    if (cw.ovf || lw.ovf || sw.ovf) return NULL;

    const char *prefix = "https://meshtastic.org/e/#";
    size_t pn = strlen(prefix);
    memcpy(url, prefix, pn);
    b64url_encode(set, sw.len, url + pn, sizeof(url) - pn);
    return url;
}

int mesh_channel_import_url(const char *url)
{
    if (!mesh_connected() || !url) return -1;

    const char *frag = strrchr(url, '#');
    frag = frag ? frag + 1 : url;                        /* tolère donnée brute */

    uint8_t buf[256];
    size_t n = b64_decode(frag, buf, sizeof(buf));
    if (n < 2) return -1;

    bool assigned[MAX_CHANS] = { false };
    pb_reader r; pb_reader_init(&r, buf, n);
    uint32_t f, w;
    int added = 0;
    while (pb_read_tag(&r, &f, &w)) {
        const uint8_t *b; size_t bl;
        if (f == 1 && w == 2 && pb_read_bytes(&r, &b, &bl)) {   /* ChannelSettings */
            pb_reader sr; pb_reader_init(&sr, b, bl);
            uint32_t sf, sw2;
            uint8_t psk[32]; size_t plen = 0; char name[24] = "";
            while (pb_read_tag(&sr, &sf, &sw2)) {
                const uint8_t *sb; size_t sbl;
                if (sf == 2 && sw2 == 2 && pb_read_bytes(&sr, &sb, &sbl)) {
                    plen = sbl < sizeof(psk) ? sbl : sizeof(psk);
                    memcpy(psk, sb, plen);
                } else if (sf == 3 && sw2 == 2 && pb_read_bytes(&sr, &sb, &sbl)) {
                    size_t k = sbl < sizeof(name) - 1 ? sbl : sizeof(name) - 1;
                    memcpy(name, sb, k); name[k] = '\0';
                } else {
                    pb_skip(&sr, sw2);
                }
            }
            if (!name[0]) continue;                          /* sans nom : ignoré */
            bool exists = false;
            for (int k = 0; k < s_chan_count; k++)
                if (strcmp(s_chans[k].name, name) == 0) { exists = true; break; }
            if (exists) continue;

            int idx = -1;                                    /* index libre (local + batch) */
            for (int t = 1; t < MAX_CHANS; t++) {
                bool used = assigned[t];
                for (int k = 0; !used && k < s_chan_count; k++)
                    if (s_chans[k].index == t) used = true;
                if (!used) { idx = t; break; }
            }
            if (idx < 0) break;

            if (added == 0) admin_flag(64);
            admin_set_channel(idx, name, psk, plen, 2);
            assigned[idx] = true;
            added++;
        } else {
            pb_skip(&r, w);
        }
    }
    if (added) { admin_flag(65); schedule_reload(); }
    return added;
}

/* AdminMessage{ set_owner(32): User{ id(1), long_name(2), short_name(3) } } */
bool mesh_set_owner(const char *long_name, const char *short_name)
{
    if (!mesh_connected() || !long_name || !long_name[0]) return false;

    char id[12];
    snprintf(id, sizeof(id), "!%08x", s_my_num);

    uint8_t usr[120];
    pb_writer uw; pb_writer_init(&uw, usr, sizeof(usr));
    pb_field_bytes(&uw, 1, (const uint8_t *)id, strlen(id));
    pb_field_bytes(&uw, 2, (const uint8_t *)long_name, strlen(long_name));
    if (short_name && short_name[0])
        pb_field_bytes(&uw, 3, (const uint8_t *)short_name, strlen(short_name));

    uint8_t am[160];
    pb_writer aw; pb_writer_init(&aw, am, sizeof(am));
    pb_field_bytes(&aw, 32, usr, uw.len);
    if (uw.ovf || aw.ovf) return false;

    send_admin(am, aw.len);
    schedule_reload();          /* re-handshake -> le nouveau User revient ~1 s plus tard */
    return true;
}
