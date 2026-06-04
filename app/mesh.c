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

#define HEARTBEAT_SECS   30
#define RECONNECT_SECS   5
#define CONNECT_TMO_SECS 8

#define MAX_NODES        64
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
} node_slot_t;

typedef struct {
    int            index;   /* numéro de canal réel */
    mesh_channel_t pub;
    char           name[24];
} chan_slot_t;

typedef struct {
    mesh_message_t m;       /* m.text embarqué ; m.from/m.time -> buffers */
    char           from[40];
    char           time[12];
    uint32_t       id;      /* id du paquet (dédup + ACK) */
} msg_slot_t;

static node_slot_t s_nodes[MAX_NODES];
static int         s_node_count;

static chan_slot_t s_chans[MAX_CHANS];
static int         s_chan_count;

static msg_slot_t  s_msgs[MAX_MSGS];
static int         s_msg_count;

static char        s_region[16] = "EU868";
static char        s_preset[16] = "LongFast";
static char        s_uptime[16] = "-";
static mesh_self_t s_self;

static uint32_t    s_my_num;
static bool        s_dirty;

/* ----------------------------------------------------------------- liaison */
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

/* ----------------------------------------------------------------- nodes */
static node_slot_t *node_get_or_add(uint32_t num)
{
    for (int i = 0; i < s_node_count; i++)
        if (s_nodes[i].num == num) return &s_nodes[i];
    if (s_node_count >= MAX_NODES) return NULL;
    node_slot_t *s = &s_nodes[s_node_count++];
    memset(s, 0, sizeof(*s));
    s->num = num;
    snprintf(s->id, sizeof(s->id), "!%08x", num);
    snprintf(s->name, sizeof(s->name), "%s", s->id);
    s->pub.id   = s->id;
    s->pub.name = s->name;
    s->pub.last = s->last;
    return s;
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
    if (have_snr) s->pub.snr = (int8_t)(snr >= 0 ? snr + 0.5f : snr - 0.5f);
    if (rssi)     s->pub.rssi = (int16_t)rssi;
    s->last_heard = (uint32_t)time(NULL);
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
    if (have_snr) s->pub.snr = (int8_t)(snr >= 0 ? snr + 0.5f : snr - 0.5f);
    if (have_hops) s->pub.hops = (uint8_t)hops;
    if (last_heard) s->last_heard = last_heard;
    s->pub.self = (num == s_my_num && s_my_num);
    if (dm) parse_devmetrics(dm, dml, num);
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
    const uint8_t *settings = NULL; size_t setl = 0;

    while (pb_read_tag(&r, &f, &w)) {
        uint64_t v; const uint8_t *b; size_t bl;
        if (f == 1 && w == 0 && pb_read_varint(&r, &v)) index = (int)(int32_t)(uint32_t)v;
        else if (f == 2 && w == 2 && pb_read_bytes(&r, &b, &bl)) { settings = b; setl = bl; }
        else if (f == 3 && w == 0 && pb_read_varint(&r, &v)) role = (uint32_t)v;
        else pb_skip(&r, w);
    }
    if (role == 0) return;   /* DISABLED */

    if (settings) {
        pb_reader sr; pb_reader_init(&sr, settings, setl);
        uint32_t sf, sw;
        while (pb_read_tag(&sr, &sf, &sw)) {
            const uint8_t *b; size_t bl;
            if (sf == 2 && sw == 2 && pb_read_bytes(&sr, &b, &bl)) {
                enc = (bl > 1);   /* psk > 1 octet = clé custom */
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
    c->pub.name = c->name;
    c->pub.enc = enc;
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
                    snprintf(s_preset, sizeof(s_preset), "%s", preset_str((uint32_t)v));
                    s_self.preset = s_preset;
                } else if (lf == 7 && lw == 0 && pb_read_varint(&lr, &v)) {
                    snprintf(s_region, sizeof(s_region), "%s", region_str((uint32_t)v));
                    s_self.region = s_region;
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
    mesh_try_connect();
}

void mesh_poll(void)
{
    time_t now = time(NULL);

    if (s_fd < 0) {
        if (now - s_last_attempt >= RECONNECT_SECS) mesh_try_connect();
        return;
    }
    if (s_connecting) { mesh_check_connect(); return; }

    mesh_drain();
    if (s_fd < 0) return;

    if (now - s_last_hb >= HEARTBEAT_SECS) {
        send_heartbeat();
        s_last_hb = now;
    }
}

bool mesh_connected(void) { return s_fd >= 0 && s_configured; }

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

int mesh_node_count(void) { return s_node_count; }

const mesh_node_t *mesh_node(int i)
{
    if (i < 0 || i >= s_node_count) return NULL;
    node_slot_t *s = &s_nodes[i];
    s->pub.self = (s->num == s_my_num && s_my_num);
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
