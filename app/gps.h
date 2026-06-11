#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Lecteur GPS NMEA (module NEO-6M / GY-GPS6MV2 sur /dev/serial0 @ 9600 8N1).
 * Non bloquant : gps_init() ouvre le port, gps_poll() draine les octets dispos
 * et parse les trames ($GPGGA/$GPRMC/$GPGSV/$GPGSA). L'etat est expose via
 * gps_state() (stockage interne stable). Aucune dependance externe.
 */

#define GPS_MAX_SATS 24

typedef struct {
    uint8_t  prn;     /* identifiant satellite */
    uint8_t  elev;    /* elevation deg (0-90) */
    uint16_t az;      /* azimut deg (0-359) */
    uint8_t  snr;     /* C/No dB-Hz (0 = non suivi) */
    bool     used;    /* utilise dans la solution (GSA) */
} gps_sat_t;

typedef struct {
    bool      present;       /* /dev/serial0 ouvert et des octets recus recemment */
    bool      valid;         /* fix valide (RMC=A et qual>0) */
    int       fix_dim;       /* 0 = pas de fix, 2 = 2D, 3 = 3D (GSA) */
    int       fix_qual;      /* GGA champ qualite (0 aucun, 1 GPS, 2 DGPS...) */

    double    lat, lon;      /* degres decimaux (positif N/E) */
    float     alt;           /* metres (MSL) */
    float     speed_kmh;     /* vitesse sol */
    float     course;        /* cap deg */
    float     hdop;          /* dilution horizontale */

    int       sats_used;     /* satellites dans la solution (GGA) */
    int       sats_view;     /* satellites en vue (GSV) */

    char      time_utc[12];  /* "HH:MM:SS" UTC, "" si inconnu */
    char      date_utc[12];  /* "JJ/MM/AA" UTC, "" si inconnu */

    gps_sat_t sat[GPS_MAX_SATS];
    int       sat_n;         /* entrees valides dans sat[] */

    uint32_t  last_fix;      /* epoch du dernier fix valide, 0 si jamais */
    uint32_t  last_rx;       /* epoch du dernier octet recu */
    unsigned  sentences;     /* trames NMEA parsees (diagnostic) */

    /* Acquittements de la config UBX (CFG-NAV5 static hold, CFG-SBAS EGNOS).
     * -1 = pas de reponse (TX non cable ?), 0 = refusee (NAK), 1 = acceptee (ACK). */
    int8_t    ubx_nav5;      /* CFG-NAV5 (modele pieton + static hold) */
    int8_t    ubx_sbas;      /* CFG-SBAS (EGNOS) */
    uint8_t   ubx_ack;       /* nb total d'ACK recus */
    uint8_t   ubx_nak;       /* nb total de NAK recus */
} gps_state_t;

void              gps_init(void);
void              gps_poll(void);
const gps_state_t *gps_state(void);
bool              gps_connected(void);   /* present && octets recents */

/* Active/desactive le lecteur GPS. Desactiver ferme /dev/serial0 (le port est
 * libere, le module peut etre coupe cote alim) et invalide l'etat (valid=false,
 * connected=false) : les apps qui s'en servent (CARTE, GPS) basculent en mode
 * "GPS off". Reactiver rouvre le port au prochain gps_poll(). */
void              gps_set_enabled(bool en);
bool              gps_enabled(void);

/* Derniere position connue (mise a jour a chaque fix valide, persistee sur
 * disque -> survit au reboot et au GPS coupe). Donne un point de reference
 * constant sur la carte meme sans fix. Renvoie false si aucune position connue.
 * lat/lon/epoch peuvent etre NULL. */
bool              gps_last_known(double *lat, double *lon, uint32_t *epoch);
