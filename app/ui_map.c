#include "ui_common.h"
#include "ui_map.h"
#include "gps.h"
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>

/* ============================================================ */
/* Carte slippy offline, optimisee Pi Zero 2 W (512 Mo, A53).    */
/* Espace de coordonnees = pixels Web Mercator au zoom courant    */
/* (tuile = 256 px). Grille de tuiles autour du centre + marqueurs*/
/* GPS / noeuds par-dessus. Tuiles = RAW RGB565 lues du disque    */
/* (aucun decodage PNG).                                          */
/*                                                                */
/* Choix "faible ressource" :                                     */
/*  - le cache de tuiles (~2 Mo) est ALLOUE a l'ouverture de la    */
/*    vue et LIBERE a la fermeture -> 0 octet gele hors carte.    */
/*  - map_render() n'est rejoue que si l'etat visible a change     */
/*    (centre / zoom / fix GPS / noeuds) -> CPU au repos sinon.    */
/*  - taille du viewport mise en cache (pas de relayout par image).*/
/* ============================================================ */

#define MAP_DIR    "/home/bq-lora/bq-lora-ui/maps"
#define TILE       256
#define MAP_PI     3.14159265358979323846
#define ZOOM_MIN   11
#define ZOOM_MAX   16
#define TILE_BYTES (TILE * TILE * 2)

#define POOL_N     12          /* objets image : 3x3 visibles max (480x320) + marge */
#define TCACHE_N   16          /* tuiles en RAM quand la vue est ouverte (~2 Mo) */
#define NODE_MARK_N 48         /* marqueurs noeuds max affiches */

/* --- etat vue --- */
static double   s_clat = 45.764, s_clon = 4.835;   /* centre (defaut : Lyon) */
static int      s_zoom = 15;
static bool     s_follow = true;
static bool     s_show_nodes = true;
static bool     s_center_init = false;  /* a-t-on deja centre sur la derniere pos ? */

/* --- widgets --- */
static lv_obj_t *s_map;                 /* conteneur viewport (recoit le touch) */
static lv_obj_t *s_pool[POOL_N];        /* images tuiles */
static uint64_t  s_pool_key[POOL_N];    /* cle z/x/y actuellement affichee */
static lv_obj_t *s_gps_mark;            /* marqueur position live (fix actuel) */
static lv_obj_t *s_lk_mark;             /* marqueur derniere position connue */
static lv_obj_t *s_nm[NODE_MARK_N];     /* marqueurs noeuds */
static lv_obj_t *s_info_lbl;            /* zoom / etat */

/* viewport (taille fixe : mise en cache pour eviter un relayout par rendu) */
static int s_vw, s_vh;

/* dispo des donnees (garde-fous) */
static bool s_have_maps;                /* MAP_DIR present */

/* --- cache de tuiles : alloue a l'ouverture, libere a la fermeture --- */
typedef struct {
    bool     used, ok;
    int      z; uint32_t x, y;
    uint32_t lru;
    uint8_t  buf[TILE_BYTES];
    lv_image_dsc_t dsc;
} tcache_t;
static tcache_t *s_tc;                  /* tableau[TCACHE_N], NULL = non alloue */
static uint32_t  s_lru;

/* --- drag --- */
static bool s_drag;
static lv_point_t s_last;

/* --- snapshot pour la detection de changement (anti-rendu inutile) --- */
static bool     s_rendered;
static int      s_last_zoom = -1;
static double   s_last_clat, s_last_clon;
static bool     s_last_gpsvalid, s_last_conn, s_last_nodes;
static double   s_last_gpslat, s_last_gpslon;
static unsigned s_last_node_sig;

/* ---- math Web Mercator ---- */
static double lon2px(double lon, int z) { return (lon + 180.0) / 360.0 * TILE * (double)(1u << z); }
static double lat2px(double lat, int z) {
    double r = lat * MAP_PI / 180.0;
    return (1.0 - asinh(tan(r)) / MAP_PI) / 2.0 * TILE * (double)(1u << z);
}
static double px2lon(double px, int z) { return px / (TILE * (double)(1u << z)) * 360.0 - 180.0; }
static double px2lat(double px, int z) {
    double n = MAP_PI - 2.0 * MAP_PI * px / (TILE * (double)(1u << z));
    return 180.0 / MAP_PI * atan(sinh(n));
}

static uint64_t tkey(int z, uint32_t x, uint32_t y) {
    return ((uint64_t)z << 56) | ((uint64_t)x << 28) | (uint64_t)y;
}

/* Signature de l'etat des noeuds positionnes (count + positions quantifiees).
 * Sert a ne re-rendre le calque que s'il a reellement bouge. 0 si calque off. */
static unsigned node_sig(void)
{
    if (!s_show_nodes) return 0;
    unsigned s = 1u;
    int nn = mesh_node_count();
    for (int i = 0; i < nn; i++) {
        const mesh_node_t *nd = mesh_node(i);
        if (!nd || !nd->has_pos || nd->self) continue;
        s = s * 131u + nd->num;
        s = s * 131u + (unsigned)(int)(nd->lat * 1e5);
        s = s * 131u + (unsigned)(int)(nd->lon * 1e5);
    }
    return s;
}

/* Charge (ou recupere du cache) la tuile z/x/y. Renvoie son descripteur image
 * ou NULL si cache non alloue / fichier absent / illisible. */
static lv_image_dsc_t *tile_get(int z, uint32_t x, uint32_t y)
{
    if (!s_tc) return NULL;                 /* garde-fou : RAM non allouee */

    tcache_t *victim = NULL;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < TCACHE_N; i++) {
        tcache_t *t = &s_tc[i];
        if (t->used && t->z == z && t->x == x && t->y == y) {
            t->lru = ++s_lru;
            return t->ok ? &t->dsc : NULL;
        }
        if (!t->used) { victim = t; break; }
        if (t->lru < oldest) { oldest = t->lru; victim = t; }
    }
    if (!victim) return NULL;               /* garde-fou : TCACHE_N == 0 */

    /* charge dans 'victim' */
    victim->used = true; victim->ok = false;
    victim->z = z; victim->x = x; victim->y = y; victim->lru = ++s_lru;

    char path[160];
    snprintf(path, sizeof(path), MAP_DIR "/%d/%u/%u.bin", z, x, y);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t rd = fread(victim->buf, 1, TILE_BYTES, f);
        fclose(f);
        if (rd == TILE_BYTES) {
            memset(&victim->dsc, 0, sizeof(victim->dsc));
            victim->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            victim->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            victim->dsc.header.w      = TILE;
            victim->dsc.header.h      = TILE;
            victim->dsc.header.stride = TILE * 2;
            victim->dsc.data          = victim->buf;
            victim->dsc.data_size     = TILE_BYTES;
            victim->ok = true;
        }
    }
    return victim->ok ? &victim->dsc : NULL;
}

/* Place un marqueur (objet) a la position geo, le cache s'il sort du viewport. */
static void place_marker(lv_obj_t *m, double lat, double lon,
                         double cx, double cy, int W, int H)
{
    double sx = lon2px(lon, s_zoom) - cx + W / 2.0;
    double sy = lat2px(lat, s_zoom) - cy + H / 2.0;
    if (sx < -40 || sx > W + 40 || sy < -20 || sy > H + 20) {
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(m, (int)(sx) - lv_obj_get_width(m) / 2,
                      (int)(sy) - lv_obj_get_height(m) / 2);
}

static void map_render(void)
{
    if (!s_map) return;

    /* viewport fixe : on ne relayoute qu'une fois */
    if (s_vw <= 0 || s_vh <= 0) {
        lv_obj_update_layout(s_map);
        s_vw = lv_obj_get_width(s_map);
        s_vh = lv_obj_get_height(s_map);
        if (s_vw <= 0 || s_vh <= 0) { s_vw = 480; s_vh = 280; }
    }
    int W = s_vw, H = s_vh;

    double cx = lon2px(s_clon, s_zoom);
    double cy = lat2px(s_clat, s_zoom);

    /* fenetre de tuiles couvrant le viewport */
    int tx0 = (int)floor((cx - W / 2.0) / TILE);
    int ty0 = (int)floor((cy - H / 2.0) / TILE);
    int tx1 = (int)floor((cx + W / 2.0) / TILE);
    int ty1 = (int)floor((cy + H / 2.0) / TILE);
    uint32_t world = 1u << s_zoom;

    int p = 0;
    for (int ty = ty0; ty <= ty1 && p < POOL_N; ty++) {
        for (int tx = tx0; tx <= tx1 && p < POOL_N; tx++) {
            if (tx < 0 || ty < 0 || (uint32_t)tx >= world || (uint32_t)ty >= world)
                continue;
            lv_obj_t *img = s_pool[p];
            int sx = (int)lround(tx * (double)TILE - cx + W / 2.0);
            int sy = (int)lround(ty * (double)TILE - cy + H / 2.0);
            lv_obj_set_pos(img, sx, sy);

            uint64_t k = tkey(s_zoom, (uint32_t)tx, (uint32_t)ty);
            if (s_pool_key[p] != k) {          /* evite invalidation inutile */
                lv_image_dsc_t *d = tile_get(s_zoom, (uint32_t)tx, (uint32_t)ty);
                if (d) { lv_image_set_src(img, d); lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN); }
                else   { lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN); }
                s_pool_key[p] = k;
            }
            p++;
        }
    }
    for (; p < POOL_N; p++) { lv_obj_add_flag(s_pool[p], LV_OBJ_FLAG_HIDDEN); s_pool_key[p] = 0; }

    /* marqueur GPS live (fix actuel) */
    const gps_state_t *g = gps_state();
    if (g->valid) place_marker(s_gps_mark, g->lat, g->lon, cx, cy, W, H);
    else lv_obj_add_flag(s_gps_mark, LV_OBJ_FLAG_HIDDEN);

    /* marqueur derniere position connue (point constant : visible des qu'il n'y
     * a pas de fix live, y compris GPS coupe) */
    double lk_lat, lk_lon;
    if (!g->valid && gps_last_known(&lk_lat, &lk_lon, NULL))
        place_marker(s_lk_mark, lk_lat, lk_lon, cx, cy, W, H);
    else
        lv_obj_add_flag(s_lk_mark, LV_OBJ_FLAG_HIDDEN);

    /* calque noeuds */
    int used = 0;
    if (s_show_nodes) {
        int nn = mesh_node_count();
        for (int i = 0; i < nn && used < NODE_MARK_N; i++) {
            const mesh_node_t *nd = mesh_node(i);
            if (!nd || !nd->has_pos || nd->self) continue;
            lv_obj_t *m = s_nm[used];
            lv_label_set_text(m, nd->id);
            place_marker(m, nd->lat, nd->lon, cx, cy, W, H);
            used++;
        }
    }
    for (int i = used; i < NODE_MARK_N; i++) lv_obj_add_flag(s_nm[i], LV_OBJ_FLAG_HIDDEN);

    bool conn = gps_connected();
    if (s_info_lbl) {
        const char *txt = !s_have_maps  ? "carte absente"
                        : !s_tc         ? "RAM insuffisante"
                        : !gps_enabled()? "GPS coupe"
                        : g->valid      ? (s_follow ? "suivi GPS" : "libre")
                        : (conn ? "no fix" : "no gps");
        char b[64];
        snprintf(b, sizeof(b), "z%d  %s", s_zoom, txt);
        lv_label_set_text(s_info_lbl, b);
    }

    /* snapshot : etat reellement affiche (sert a sauter les rendus identiques) */
    s_last_zoom     = s_zoom;
    s_last_clat     = s_clat;     s_last_clon = s_clon;
    s_last_gpsvalid = g->valid;   s_last_conn = conn;
    s_last_gpslat   = g->lat;     s_last_gpslon = g->lon;
    s_last_nodes    = s_show_nodes;
    s_last_node_sig = node_sig();
    s_rendered      = true;
}

/* ---- interactions ---- */
static void drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt; lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        s_drag = true; s_last = pt; s_follow = false;
    } else if (code == LV_EVENT_PRESSING && s_drag) {
        int dx = pt.x - s_last.x;
        int dy = pt.y - s_last.y;
        if (dx == 0 && dy == 0) return;
        s_last = pt;
        double cx = lon2px(s_clon, s_zoom) - dx;
        double cy = lat2px(s_clat, s_zoom) - dy;
        s_clon = px2lon(cx, s_zoom);
        s_clat = px2lat(cy, s_zoom);
        map_render();
    } else if (code == LV_EVENT_RELEASED) {
        s_drag = false;
    }
}

static void zoom_in_cb(lv_event_t *e)  { (void)e; if (s_zoom < ZOOM_MAX) { s_zoom++; memset(s_pool_key,0,sizeof(s_pool_key)); map_render(); } }
static void zoom_out_cb(lv_event_t *e) { (void)e; if (s_zoom > ZOOM_MIN) { s_zoom--; memset(s_pool_key,0,sizeof(s_pool_key)); map_render(); } }
static void follow_cb(lv_event_t *e)
{
    (void)e;
    const gps_state_t *g = gps_state();
    s_follow = true;
    double la, lo;
    if (g->valid) { s_clat = g->lat; s_clon = g->lon; }
    else if (gps_last_known(&la, &lo, NULL)) { s_clat = la; s_clon = lo; }
    map_render();
}
static void nodes_cb(lv_event_t *e) { (void)e; s_show_nodes = !s_show_nodes; map_render(); }

/* petit bouton rond flottant */
static lv_obj_t *map_btn(const char *txt, lv_align_t al, int x, int y, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(s_map);
    lv_obj_set_size(b, 38, 30);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, al, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, txt, FONT_BODY, CY_CYAN);
    lv_obj_center(l);
    return b;
}

void ui_map_sync_if_visible(void)
{
    if (!s_map) return;

    const gps_state_t *g = gps_state();
    if (s_follow && g->valid) { s_clat = g->lat; s_clon = g->lon; }

    /* ne re-rendre que si quelque chose de visible a change */
    unsigned sig = node_sig();
    bool chg = !s_rendered
            || s_zoom        != s_last_zoom
            || s_clat        != s_last_clat
            || s_clon        != s_last_clon
            || g->valid      != s_last_gpsvalid
            || gps_connected()!= s_last_conn
            || (g->valid && (g->lat != s_last_gpslat || g->lon != s_last_gpslon))
            || s_show_nodes  != s_last_nodes
            || sig           != s_last_node_sig;
    if (chg) map_render();
}

void ui_map_reset(void)
{
    /* libere le cache de tuiles : 0 octet gele quand la carte n'est pas vue.
     * Appele apres lv_obj_clean (les images ne referencent plus s_tc->buf). */
    if (s_tc) { free(s_tc); s_tc = NULL; }
    s_lru = 0;

    s_map = NULL; s_gps_mark = NULL; s_lk_mark = NULL; s_info_lbl = NULL;
    s_drag = false;
    s_vw = s_vh = 0;
    s_rendered = false; s_last_zoom = -1;
    for (int i = 0; i < POOL_N; i++) { s_pool[i] = NULL; s_pool_key[i] = 0; }
    for (int i = 0; i < NODE_MARK_N; i++) s_nm[i] = NULL;
}

void ui_map_build(void)
{
    ui_map_reset();

    /* garde-fou : le dossier de tuiles existe-t-il ? */
    struct stat st;
    s_have_maps = (stat(MAP_DIR, &st) == 0 && S_ISDIR(st.st_mode));

    /* au tout premier affichage, centre sur la derniere position connue si on
     * en a une (sinon on garde le defaut Lyon) */
    if (!s_center_init) {
        double la, lo;
        if (gps_last_known(&la, &lo, NULL)) { s_clat = la; s_clon = lo; }
        s_center_init = true;
    }

    /* cache de tuiles alloue maintenant, libere dans ui_map_reset() */
    s_tc = calloc(TCACHE_N, sizeof(tcache_t));   /* ~2 Mo ; NULL toleré (tile_get garde) */

    /* viewport plein cadre, recoit le touch pour le panoramique */
    lv_obj_t *map = lv_obj_create(content);
    lv_obj_set_size(map, LV_PCT(100), LV_PCT(100));
    flat(map);
    lv_obj_set_style_bg_color(map, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(map, LV_OPA_COVER, 0);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map, drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(map, drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(map, drag_cb, LV_EVENT_RELEASED, NULL);
    s_map = map;

    /* pool de tuiles */
    for (int i = 0; i < POOL_N; i++) {
        lv_obj_t *img = lv_image_create(map);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        s_pool[i] = img; s_pool_key[i] = 0;
    }

    /* marqueurs noeuds (pastilles avec id court) */
    for (int i = 0; i < NODE_MARK_N; i++) {
        lv_obj_t *m = lv_label_create(map);
        lv_label_set_text(m, "");
        lv_obj_set_style_text_font(m, FONT_SMALL, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(CY_TEXT), 0);
        lv_obj_set_style_bg_color(m, lv_color_hex(CY_MAGENTA), 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(m, 3, 0);
        lv_obj_set_style_pad_hor(m, 3, 0);
        lv_obj_set_style_pad_ver(m, 1, 0);
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
        s_nm[i] = m;
    }

    /* marqueur GPS (point cyan) */
    lv_obj_t *gm = lv_obj_create(map);
    lv_obj_set_size(gm, 14, 14);
    lv_obj_set_style_radius(gm, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(gm, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_bg_opa(gm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(gm, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(gm, 2, 0);
    lv_obj_set_style_shadow_width(gm, 0, 0);
    lv_obj_add_flag(gm, LV_OBJ_FLAG_HIDDEN);
    s_gps_mark = gm;

    /* marqueur derniere position connue : anneau creux (distinct du point live) */
    lv_obj_t *lk = lv_obj_create(map);
    lv_obj_set_size(lk, 14, 14);
    lv_obj_set_style_radius(lk, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(lk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(lk, lv_color_hex(CY_CYAN), 0);
    lv_obj_set_style_border_width(lk, 2, 0);
    lv_obj_set_style_shadow_width(lk, 0, 0);
    lv_obj_add_flag(lk, LV_OBJ_FLAG_HIDDEN);
    s_lk_mark = lk;

    /* controles flottants */
    map_btn(LV_SYMBOL_PLUS,  LV_ALIGN_TOP_RIGHT,    -4, 4,  zoom_in_cb);
    map_btn(LV_SYMBOL_MINUS, LV_ALIGN_TOP_RIGHT,    -4, 38, zoom_out_cb);
    map_btn(LV_SYMBOL_GPS,   LV_ALIGN_BOTTOM_RIGHT, -4, -4, follow_cb);
    map_btn(LV_SYMBOL_EYE_OPEN, LV_ALIGN_BOTTOM_RIGHT, -4, -38, nodes_cb);

    s_info_lbl = label(map, "", FONT_SMALL, CY_CYAN);
    lv_obj_set_style_bg_color(s_info_lbl, lv_color_hex(CY_PANEL), 0);
    lv_obj_set_style_bg_opa(s_info_lbl, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_info_lbl, 4, 0);
    lv_obj_set_style_pad_ver(s_info_lbl, 1, 0);
    lv_obj_set_style_radius(s_info_lbl, 3, 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_LEFT, 4, 4);

    map_render();
}
