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
#define LK_PATH   "/home/bq-lora/bq-lora-ui/gps_last.txt"
#define LK_SAVE_S 30          /* ecriture disque au plus toutes les 30 s (usure SD) */

static int          s_fd = -1;
static char         s_line[LINE_MAX];
static int          s_len;
static gps_state_t  s_st;
static time_t       s_open_retry;     /* prochaine tentative d'ouverture */
static bool         s_enabled = true; /* lecteur GPS actif ? */

/* derniere position connue (point constant sur la carte) */
static double       s_lk_lat, s_lk_lon;
static uint32_t     s_lk_epoch;       /* 0 = aucune position connue */
static time_t       s_lk_saved;       /* derniere ecriture disque */

/* lissage anti-jitter : filtre exponentiel sur la position, gere par fix (~1 Hz).
 * Le coefficient depend de la vitesse -> fort lissage a l'arret, suivi en mouvement. */
static double       s_flt_lat, s_flt_lon;
static bool         s_flt_init;
static uint32_t     s_flt_fix;        /* dernier last_fix deja lisse */

static void lk_load(void)
{
    FILE *f = fopen(LK_PATH, "r");
    if (!f) return;
    double la, lo; long ep = 0;
    if (fscanf(f, "%lf %lf %ld", &la, &lo, &ep) == 3 && ep > 0) {
        s_lk_lat = la; s_lk_lon = lo; s_lk_epoch = (uint32_t)ep;
    }
    fclose(f);
}

static void lk_save(void)
{
    if (!s_lk_epoch) return;
    FILE *f = fopen(LK_PATH, "w");
    if (!f) return;
    fprintf(f, "%.7f %.7f %u\n", s_lk_lat, s_lk_lon, s_lk_epoch);
    fclose(f);
    s_lk_saved = time(NULL);
}

/* Accumulation GSV (les satellites en vue arrivent sur plusieurs trames). */
static gps_sat_t    s_gsv[GPS_MAX_SATS];
static int          s_gsv_n;
static int          s_gsv_total_msg;

/* ---- configuration binaire u-blox (UBX) ---- */
/* Construit une trame UBX (B5 62 cls id len payload ck_a ck_b, checksum Fletcher
 * sur cls..payload) et l'envoie. Best-effort : le module ignore une trame
 * tronquee, et on rejoue la config a chaque ouverture du port. */
static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    if (s_fd < 0) return;
    uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += hdr[i]; b += a; }
    for (uint16_t i = 0; i < len; i++) { a += payload[i]; b += a; }
    uint8_t ck[2] = { a, b };
    ssize_t w;
    w = write(s_fd, hdr, 6);            (void)w;
    if (len) { w = write(s_fd, payload, len); (void)w; }
    w = write(s_fd, ck, 2);            (void)w;
    tcdrain(s_fd);                      /* laisse partir la trame avant la suivante */
}

/* Modele "pieton" + Static Hold (anti-jitter materiel) + SBAS/EGNOS (corrections
 * differentielles). Applique en RAM du module ; rejoue a chaque gps_open() pour
 * ne pas dependre de la sauvegarde flash (pile de sauvegarde absente/faible). */
static void ubx_configure(void)
{
    /* CFG-NAV5 (0x06 0x24, 36 o) : dynModel=3 (pieton) + static hold 0.8 m/s.
     * Le static hold fige activement la position sous le seuil de vitesse. */
    uint8_t nav5[36] = {0};
    nav5[0]  = 0x41; nav5[1] = 0x00;   /* mask : dyn (0x01) + staticHold (0x40) */
    nav5[2]  = 3;                      /* dynModel : 3 = pedestrian */
    nav5[3]  = 3;                      /* fixMode  : 3 = auto 2D/3D */
    nav5[12] = 5;                      /* minElev  : 5 deg */
    nav5[22] = 80;                     /* staticHoldThresh : 80 cm/s (0.8 m/s) */
    ubx_send(0x06, 0x24, nav5, 36);

    /* CFG-SBAS (0x06 0x16, 8 o) : SBAS active (EGNOS en EU), range + diffCorr. */
    uint8_t sbas[8] = { 0x01, 0x03, 3, 0, 0, 0, 0, 0 };
    ubx_send(0x06, 0x16, sbas, 8);
}

static void gps_open(void)
{
    /* O_RDWR : on doit pouvoir ECRIRE la config UBX (Pi TX -> GPS RX cable). */
    s_fd = open(GPS_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);
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
    ubx_configure();      /* anti-jitter materiel + SBAS */
    s_st.present = true;
}

void gps_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_len = 0;
    lk_load();                 /* recharge la derniere position connue */
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

    /* Lissage anti-jitter : a chaque NOUVEAU fix (last_fix change, soit ~1 Hz),
     * on melange la position brute avec la position filtree. Le coefficient
     * depend de la vitesse :
     *   - a l'arret  (< 1.5 km/h) : a=0.12 -> le point se fige (le bruit GPS
     *     a l'arret tourne autour de la vraie position, la moyenne converge) ;
     *   - lent       (< 5 km/h)   : a=0.40 -> compromis ;
     *   - clairement en mouvement : a=1.00 -> position brute, pleine reactivite.
     * Si HDOP est mauvais (>4), on lisse davantage (fix peu fiable). */
    if (s_st.valid && s_st.last_fix != s_flt_fix) {
        s_flt_fix = s_st.last_fix;
        double rla = s_st.lat, rlo = s_st.lon;
        if (!s_flt_init) {
            s_flt_lat = rla; s_flt_lon = rlo; s_flt_init = true;
        } else {
            double a = s_st.speed_kmh > 5.0f ? 1.00
                     : s_st.speed_kmh > 1.5f ? 0.40
                     :                         0.12;
            if (s_st.hdop > 4.0f && a > 0.20) a = 0.20;   /* fix douteux -> on lisse */
            s_flt_lat += a * (rla - s_flt_lat);
            s_flt_lon += a * (rlo - s_flt_lon);
        }
        s_st.lat = s_flt_lat; s_st.lon = s_flt_lon;
    }

    /* memorise la derniere position connue (point constant carte) */
    if (s_st.valid) {
        s_lk_lat = s_st.lat; s_lk_lon = s_st.lon;
        s_lk_epoch = (uint32_t)time(NULL);
        if (time(NULL) - s_lk_saved >= LK_SAVE_S) lk_save();
    }

    /* perte de fix : si plus de trame valide depuis >5 s, on invalide et on
     * reinitialise le filtre (le prochain fix repart de la mesure brute). */
    if (s_st.valid && time(NULL) - (time_t)s_st.last_fix > 5) {
        s_st.valid = false; s_flt_init = false;
    }
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
        /* coupe le port et oublie l'etat (les apps voient "GPS off").
         * On persiste d'abord la derniere position connue (point constant). */
        lk_save();
        if (s_fd >= 0) { close(s_fd); s_fd = -1; }
        s_len = 0;
        s_gsv_n = 0; s_gsv_total_msg = 0;
        s_flt_init = false;
        memset(&s_st, 0, sizeof(s_st));
    } else {
        /* reouverture immediate au prochain poll */
        s_open_retry = 0;
    }
}

bool gps_enabled(void) { return s_enabled; }

bool gps_last_known(double *lat, double *lon, uint32_t *epoch)
{
    if (!s_lk_epoch) return false;
    if (lat)   *lat   = s_lk_lat;
    if (lon)   *lon   = s_lk_lon;
    if (epoch) *epoch = s_lk_epoch;
    return true;
}
