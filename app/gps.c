#include "gps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>

/* ============================================================ */
/* Lecteur NMEA non bloquant. gps_poll() est appele depuis la    */
/* boucle principale (~50 Hz) : il draine le buffer serie et     */
/* parse chaque trame complete. Le GPS emet ~1 Hz a 9600 bauds.  */
/* ============================================================ */

#define GPS_DEV   "/dev/serial0"
#define LINE_MAX  120

static int          s_fd = -1;
static char         s_line[LINE_MAX];
static int          s_len;
static gps_state_t  s_st;
static time_t       s_open_retry;     /* prochaine tentative d'ouverture */
static bool         s_enabled = true; /* lecteur GPS actif ? */

/* Accumulation GSV (les satellites en vue arrivent sur plusieurs trames). */
static gps_sat_t    s_gsv[GPS_MAX_SATS];
static int          s_gsv_n;
static int          s_gsv_total_msg;

static void gps_open(void)
{
    s_fd = open(GPS_DEV, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (s_fd < 0) return;

    struct termios tio;
    if (tcgetattr(s_fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B9600);
        cfsetospeed(&tio, B9600);
        tio.c_cflag |= (CLOCAL | CREAD);
        tio.c_cflag &= ~CRTSCTS;
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(s_fd, TCSANOW, &tio);
    }
    s_st.present = true;
}

void gps_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_len = 0;
    if (s_enabled) gps_open();
}

/* ---- helpers de parsing ---- */

/* Convertit un champ NMEA ddmm.mmmm + hemisphere en degres decimaux signes. */
static double nmea_coord(const char *v, const char *hemi)
{
    if (!v || !*v) return 0.0;
    double raw = atof(v);
    int deg = (int)(raw / 100.0);
    double min = raw - deg * 100.0;
    double d = deg + min / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}

/* Verifie le checksum NMEA ( *HH apres le contenu ). Tolerant : accepte aussi
 * les trames sans checksum. */
static bool nmea_checksum_ok(const char *s)
{
    if (s[0] != '$') return false;
    const char *star = strrchr(s, '*');
    if (!star) return true;            /* pas de checksum -> on tolere */
    unsigned sum = 0;
    for (const char *p = s + 1; p < star; p++) sum ^= (unsigned char)*p;
    unsigned ref = (unsigned)strtol(star + 1, NULL, 16);
    return sum == (ref & 0xFF);
}

/* Decoupe la trame en champs (separateur ',' ; '*' termine). Renvoie le nombre
 * de champs. Modifie 'line' en place. */
static int nmea_split(char *line, char *f[], int maxf)
{
    int n = 0;
    char *p = line;
    f[n++] = p;
    while (*p && n < maxf) {
        if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
        else if (*p == '*') { *p = '\0'; break; }
        p++;
    }
    return n;
}

static void parse_gga(char *f[], int nf)
{
    if (nf < 10) return;
    if (*f[6]) s_st.fix_qual = atoi(f[6]);
    if (*f[7]) s_st.sats_used = atoi(f[7]);
    if (*f[8]) s_st.hdop = (float)atof(f[8]);
    if (s_st.fix_qual > 0 && *f[2] && *f[4]) {
        s_st.lat = nmea_coord(f[2], f[3]);
        s_st.lon = nmea_coord(f[4], f[5]);
    }
    if (*f[9]) s_st.alt = (float)atof(f[9]);
}

static void parse_rmc(char *f[], int nf)
{
    if (nf < 10) return;
    bool active = (*f[2] == 'A');
    s_st.valid = active && s_st.fix_qual > 0;
    if (active && *f[3] && *f[5]) {
        s_st.lat = nmea_coord(f[3], f[4]);
        s_st.lon = nmea_coord(f[5], f[6]);
    }
    if (*f[7]) s_st.speed_kmh = (float)atof(f[7]) * 1.852f;   /* noeuds -> km/h */
    if (*f[8]) s_st.course = (float)atof(f[8]);
    /* heure hhmmss.ss */
    if (strlen(f[1]) >= 6)
        snprintf(s_st.time_utc, sizeof(s_st.time_utc), "%c%c:%c%c:%c%c",
                 f[1][0], f[1][1], f[1][2], f[1][3], f[1][4], f[1][5]);
    /* date ddmmyy */
    if (strlen(f[9]) >= 6)
        snprintf(s_st.date_utc, sizeof(s_st.date_utc), "%c%c/%c%c/%c%c",
                 f[9][0], f[9][1], f[9][2], f[9][3], f[9][4], f[9][5]);
    if (s_st.valid) s_st.last_fix = (uint32_t)time(NULL);
}

static void parse_gsa(char *f[], int nf)
{
    if (nf < 3) return;
    if (*f[2]) s_st.fix_dim = atoi(f[2]);   /* 1 none, 2 = 2D, 3 = 3D */
    /* PRN utilises : champs 3..14 -> on marque used dans la liste GSV courante */
    for (int i = 3; i <= 14 && i < nf; i++) {
        if (!*f[i]) continue;
        int prn = atoi(f[i]);
        for (int k = 0; k < s_st.sat_n; k++)
            if (s_st.sat[k].prn == prn) s_st.sat[k].used = true;
    }
}

static void parse_gsv(char *f[], int nf)
{
    /* $xxGSV,total,msg,inview, prn,elev,az,snr (x4) */
    if (nf < 4) return;
    int total = atoi(f[1]);
    int msg   = atoi(f[2]);
    int view  = atoi(f[3]);
    s_st.sats_view = view;

    if (msg == 1) { s_gsv_n = 0; s_gsv_total_msg = total; }

    for (int base = 4; base + 3 < nf && s_gsv_n < GPS_MAX_SATS; base += 4) {
        if (!*f[base]) continue;
        gps_sat_t *s = &s_gsv[s_gsv_n++];
        s->prn  = (uint8_t)atoi(f[base]);
        s->elev = (uint8_t)atoi(f[base + 1]);
        s->az   = (uint16_t)atoi(f[base + 2]);
        s->snr  = *f[base + 3] ? (uint8_t)atoi(f[base + 3]) : 0;
        s->used = false;
    }

    /* derniere trame du cycle -> on publie */
    if (msg == s_gsv_total_msg) {
        s_st.sat_n = s_gsv_n;
        memcpy(s_st.sat, s_gsv, sizeof(gps_sat_t) * s_gsv_n);
    }
}

static void parse_line(char *line)
{
    if (line[0] != '$') return;
    if (!nmea_checksum_ok(line)) return;
    s_st.sentences++;

    /* talker sur 2 char (GP/GN/GL...) puis type sur 3 char. */
    const char *type = line + 3;
    char *f[24];
    int nf = nmea_split(line, f, 24);

    if      (!strncmp(type, "GGA", 3)) parse_gga(f, nf);
    else if (!strncmp(type, "RMC", 3)) parse_rmc(f, nf);
    else if (!strncmp(type, "GSA", 3)) parse_gsa(f, nf);
    else if (!strncmp(type, "GSV", 3)) parse_gsv(f, nf);
}

void gps_poll(void)
{
    if (!s_enabled) return;

    if (s_fd < 0) {
        time_t now = time(NULL);
        if (now < s_open_retry) return;
        s_open_retry = now + 3;
        gps_open();
        if (s_fd < 0) return;
    }

    char buf[256];
    ssize_t got;
    bool any = false;
    while ((got = read(s_fd, buf, sizeof(buf))) > 0) {
        any = true;
        for (ssize_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (s_len > 0) { s_line[s_len] = '\0'; parse_line(s_line); s_len = 0; }
            } else if (s_len < LINE_MAX - 1) {
                s_line[s_len++] = c;
            } else {
                s_len = 0;   /* trame trop longue -> on jette */
            }
        }
    }
    if (got == 0 && !any) {
        /* rien a lire : normal entre deux secondes. */
    } else if (got < 0 && !any) {
        /* EAGAIN sur non bloquant : pas de donnees, ok. */
    }
    if (any) s_st.last_rx = (uint32_t)time(NULL);

    /* perte de fix : si plus de trame valide depuis >5 s, on invalide. */
    if (s_st.valid && time(NULL) - (time_t)s_st.last_fix > 5) s_st.valid = false;
}

const gps_state_t *gps_state(void) { return &s_st; }

bool gps_connected(void)
{
    if (!s_enabled) return false;
    return s_st.present && (time(NULL) - (time_t)s_st.last_rx <= 3);
}

void gps_set_enabled(bool en)
{
    if (en == s_enabled) return;
    s_enabled = en;
    if (!en) {
        /* coupe le port et oublie l'etat (les apps voient "GPS off") */
        if (s_fd >= 0) { close(s_fd); s_fd = -1; }
        s_len = 0;
        s_gsv_n = 0; s_gsv_total_msg = 0;
        memset(&s_st, 0, sizeof(s_st));
    } else {
        /* reouverture immediate au prochain poll */
        s_open_retry = 0;
    }
}

bool gps_enabled(void) { return s_enabled; }
