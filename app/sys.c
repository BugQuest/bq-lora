#include "sys.h"
#include "settings.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <dirent.h>
#include <strings.h>

#define CTL "/usr/bin/sudo /usr/local/sbin/bq-lora-ui-ctl"

/* ---------- helpers ---------- */
static void chomp(char *s) { size_t n = strlen(s); if (n && s[n-1] == '\n') s[n-1] = 0; }

static void run_get(const char *cmd, char *out, size_t cap)
{
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return;
    if (fgets(out, (int)cap, p)) chomp(out);
    pclose(p);
}

/* ---------- info système ---------- */
static void get_ip(const char *iface, char *out, size_t cap)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ip -o -4 addr show %s 2>/dev/null | awk '{print $4}' | cut -d/ -f1", iface);
    run_get(cmd, out, cap);
    if (!out[0]) strncpy(out, "-", cap);
}

static void get_uptime(char *out, size_t cap)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) { strncpy(out, "?", cap); return; }
    double s = 0; fscanf(f, "%lf", &s); fclose(f);
    int total = (int)s;
    int d = total / 86400, h = (total / 3600) % 24, m = (total / 60) % 60;
    if (d) snprintf(out, cap, "%dj %dh", d, h);
    else if (h) snprintf(out, cap, "%dh %dm", h, m);
    else snprintf(out, cap, "%dm", m);
}

static float get_cpu_temp(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return 0.f;
    int v = 0; fscanf(f, "%d", &v); fclose(f);
    return v / 1000.0f;
}

static void get_mem(int *used, int *total)
{
    *used = *total = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char k[64]; long v; char unit[16];
    long memTotal = 0, memAvail = 0;
    while (fscanf(f, "%63s %ld %15s\n", k, &v, unit) == 3) {
        if (!strcmp(k, "MemTotal:")) memTotal = v;
        else if (!strcmp(k, "MemAvailable:")) memAvail = v;
        if (memTotal && memAvail) break;
    }
    fclose(f);
    *total = (int)(memTotal / 1024);
    *used = (int)((memTotal - memAvail) / 1024);
}

static int get_disk_pct(void)
{
    char buf[32];
    run_get("df -P / | awk 'NR==2 {gsub(\"%\",\"\",$5); print $5}'", buf, sizeof(buf));
    return atoi(buf);
}

static void get_throttled(bool *now, bool *ever)
{
    char buf[64];
    run_get("/usr/bin/vcgencmd get_throttled 2>/dev/null", buf, sizeof(buf));
    /* "throttled=0x<hex>" */
    unsigned int v = 0;
    const char *eq = strchr(buf, '=');
    if (eq) sscanf(eq + 1, "%x", &v);
    *now  = (v & 0x7) != 0;          /* bits 0..2 : sous-tension/freq cap/throttle */
    *ever = (v & 0x70000) != 0;       /* bits 16..18 : a été en sous-tension */
}

static void get_kernel(char *out, size_t cap)
{
    run_get("uname -r", out, cap);
}

static void get_wifi(char *ssid, size_t cap, int *signal)
{
    char buf[256];
    run_get("nmcli -t -f ACTIVE,SSID,SIGNAL device wifi list 2>/dev/null | awk -F: '$1==\"yes\"{print $2 \"|\" $3; exit}'",
            buf, sizeof(buf));
    *signal = -1;
    ssid[0] = 0;
    char *bar = strchr(buf, '|');
    if (bar) {
        *bar = 0;
        strncpy(ssid, buf, cap - 1);
        ssid[cap - 1] = 0;
        *signal = atoi(bar + 1);
    } else {
        strncpy(ssid, "-", cap);
    }
}

void sys_info_get(sys_info_t *o)
{
    memset(o, 0, sizeof(*o));
    run_get("hostname", o->hostname, sizeof(o->hostname));
    get_ip("wlan0", o->ip_wlan, sizeof(o->ip_wlan));
    get_ip("usb0",  o->ip_usb,  sizeof(o->ip_usb));
    get_uptime(o->uptime, sizeof(o->uptime));
    o->cpu_temp_c = get_cpu_temp();
    get_mem(&o->mem_used_mb, &o->mem_total_mb);
    o->disk_used_pct = get_disk_pct();
    get_throttled(&o->throttled_now, &o->throttled_ever);
    get_kernel(o->kernel, sizeof(o->kernel));
    get_wifi(o->wifi_ssid, sizeof(o->wifi_ssid), &o->wifi_signal);
}

void sys_warn_get(sys_warn_t *o)
{
    o->cpu_temp_c    = get_cpu_temp();
    o->disk_used_pct = get_disk_pct();
    get_throttled(&o->throttled_now, &o->throttled_ever);
}

/* ---------- actions ---------- */
void sys_reboot(void)      { system(CTL " reboot &"); }
void sys_shutdown(void)    { system(CTL " poweroff &"); }
void sys_restart_app(void) { system(CTL " restart-bq-lora-ui &"); }

bool sys_ssh_enabled(void)
{
    char b[16]; run_get("systemctl is-enabled ssh 2>/dev/null", b, sizeof(b));
    return strcmp(b, "enabled") == 0;
}
bool sys_ssh_running(void)
{
    char b[16]; run_get("systemctl is-active ssh 2>/dev/null", b, sizeof(b));
    return strcmp(b, "active") == 0;
}
void sys_ssh_set(bool on)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), CTL " %s", on ? "ssh-on" : "ssh-off");
    system(cmd);
}

bool sys_hotspot_active(void)
{
    char b[16];
    run_get("nmcli -t -f NAME connection show --active 2>/dev/null | grep -c '^Hotspot$'",
            b, sizeof(b));
    return atoi(b) > 0;
}

void sys_hotspot_set(bool on)
{
    char cmd[256];
    if (on)
        snprintf(cmd, sizeof(cmd), CTL " hotspot-on '%s' '%s' &",
                 settings_hotspot_ssid(), settings_hotspot_pass());
    else
        snprintf(cmd, sizeof(cmd), CTL " hotspot-off &");
    system(cmd);
}

void sys_set_timezone(const char *tz)
{
    if (!tz || !*tz) return;
    char cmd[160];
    snprintf(cmd, sizeof(cmd), CTL " timezone '%s' &", tz);
    system(cmd);
}

bool sys_wifi_radio_on(void)
{
    char b[16]; run_get("nmcli radio wifi 2>/dev/null", b, sizeof(b));
    return strcmp(b, "enabled") == 0;
}

void sys_wifi_radio_set(bool on)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), CTL " %s &", on ? "wifi-on" : "wifi-off");
    system(cmd);
}

bool sys_bt_on(void)
{
    /* "Soft blocked: no" => actif */
    char b[16];
    run_get("rfkill list bluetooth 2>/dev/null | awk -F: '/Soft blocked/{gsub(/ /,\"\",$2); print $2; exit}'",
            b, sizeof(b));
    return strcmp(b, "no") == 0;
}

void sys_bt_set(bool on)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), CTL " %s &", on ? "bt-on" : "bt-off");
    system(cmd);
}

usb_net_mode_t sys_usb_net_mode(void)
{
    char b[16]; run_get("nmcli -g ipv4.method connection show usb0 2>/dev/null", b, sizeof(b));
    if (strcmp(b, "shared") == 0) return USB_NET_SHARED;
    if (strcmp(b, "auto")   == 0) return USB_NET_CLIENT;
    return USB_NET_UNKNOWN;
}

void sys_usb_net_set(usb_net_mode_t mode)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), CTL " %s &",
             mode == USB_NET_CLIENT ? "usb-net-client" : "usb-net-share");
    system(cmd);
}

usb_mode_t sys_usb_mode(void)
{
    if (access("/sys/kernel/config/usb_gadget/bqlora/functions/hid.usb0", F_OK) == 0)
        return USB_MODE_HID;
    if (access("/sys/kernel/config/usb_gadget/bqlora/functions/mass_storage.usb0", F_OK) == 0)
        return USB_MODE_STORAGE;
    if (access("/sys/kernel/config/usb_gadget/bqlora/functions/ncm.usb0", F_OK) == 0)
        return USB_MODE_NCM;
    return USB_MODE_UNKNOWN;
}

typedef struct { usb_mode_cb_t cb; void *user; usb_mode_t mode; bool ok; } mode_ctx_t;
static void mode_deliver(void *a) { mode_ctx_t *c = a; if (c->cb) c->cb(c->ok, c->user); free(c); }
static void *mode_thread(void *a)
{
    mode_ctx_t *c = a;
    const char *sub = "usb-ncm";
    if      (c->mode == USB_MODE_HID)     sub = "usb-hid";
    else if (c->mode == USB_MODE_STORAGE) sub = "usb-storage";
    char cmd[128];
    snprintf(cmd, sizeof(cmd), CTL " %s", sub);
    c->ok = (system(cmd) == 0);
    lv_async_call(mode_deliver, c);
    return NULL;
}
void sys_usb_mode_set_async(usb_mode_t mode, usb_mode_cb_t cb, void *user)
{
    mode_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user; c->mode = mode;
    pthread_t t; pthread_create(&t, NULL, mode_thread, c); pthread_detach(t);
}

typedef struct { badusb_cb_t cb; void *user; char path[256]; bool ok; } bu_ctx_t;
static void bu_deliver(void *a) { bu_ctx_t *c = a; c->cb(c->ok, c->user); free(c); }
static void *bu_thread(void *a)
{
    bu_ctx_t *c = a;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/python3 /home/bq-lora/bq-lora-ui/tools/badusb.py '%s' >/dev/null 2>&1",
             c->path);
    c->ok = (system(cmd) == 0);
    lv_async_call(bu_deliver, c);
    return NULL;
}
void sys_badusb_run_async(const char *path, badusb_cb_t cb, void *user)
{
    bu_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    strncpy(c->path, path, sizeof(c->path) - 1);
    pthread_t t; pthread_create(&t, NULL, bu_thread, c); pthread_detach(t);
}

/* ---------- Camera CSI (IMX219) ---------- */
#define PHOTO_DIR   "/home/bq-lora/bq-lora-ui/photos"
#define CAM_PREVIEW "/tmp/bq-lora-ui-cam.rgb565"

typedef struct {
    cam_capture_cb_t cb; void *user;
    int  w, h;
    char photo[256], preview[256];
    bool ok;
} cam_ctx_t;

static void cam_deliver(void *a)
{
    cam_ctx_t *c = a;
    if (c->cb) c->cb(c->ok, c->photo, c->preview, c->user);
    free(c);
}

static void *cam_thread(void *a)
{
    cam_ctx_t *c = a;
    char cmd[768];

    if (system("mkdir -p " PHOTO_DIR) != 0) { /* non bloquant */ }

    /* nom de fichier horodate */
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
    snprintf(c->photo, sizeof(c->photo), PHOTO_DIR "/cam_%s.jpg", ts);
    strncpy(c->preview, CAM_PREVIEW, sizeof(c->preview) - 1);

    /* capture pleine FoV binnee 1640x1232 (mode rapide de l'IMX219).
     * -n : pas de fenetre de preview ; -t : laisse l'auto-expo se stabiliser. */
    snprintf(cmd, sizeof(cmd),
             "rpicam-still -n -t 1200 --width 1640 --height 1232 -o '%s' "
             ">/dev/null 2>&1", c->photo);
    bool cap = (system(cmd) == 0);

    /* preview brute RGB565 pour le canvas LVGL */
    bool prev = false;
    if (cap) {
        snprintf(cmd, sizeof(cmd),
                 "/usr/bin/python3 /home/bq-lora/bq-lora-ui/tools/cam.py '%s' '%s' %d %d "
                 ">/dev/null 2>&1", c->photo, c->preview, c->w, c->h);
        prev = (system(cmd) == 0);
    }

    c->ok = cap && prev;
    lv_async_call(cam_deliver, c);
    return NULL;
}

void sys_cam_capture_async(int prev_w, int prev_h, cam_capture_cb_t cb, void *user)
{
    cam_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user; c->w = prev_w; c->h = prev_h;
    pthread_t t; pthread_create(&t, NULL, cam_thread, c); pthread_detach(t);
}

/* ---------- Camera : flux live (viewfinder) ---------- */
static volatile int   cam_stream_run;
static pthread_t      cam_stream_tid;
static pid_t          cam_stream_pid = -1;
static int            cam_stream_fd  = -1;
static uint8_t       *cam_stream_buf;
static int            cam_stream_w, cam_stream_h;
static cam_frame_cb_t cam_stream_cb;
static void          *cam_stream_user;
static volatile int   cam_frame_pending;

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

/* rappelle (thread UI) le callback fourni par l'UI pour invalider le canvas */
static void cam_frame_deliver(void *unused)
{
    (void)unused;
    cam_frame_pending = 0;
    if (cam_stream_cb && cam_stream_run) cam_stream_cb(cam_stream_user);
}

/* lit exactement n octets : 1 = ok, 0 = EOF, -1 = erreur */
static int read_full(int fd, uint8_t *p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
    return 1;
}

static void *cam_stream_thread(void *a)
{
    (void)a;
    int w = cam_stream_w, h = cam_stream_h;
    size_t ysz = (size_t)w * h;
    size_t csz = (size_t)(w / 2) * (h / 2);
    size_t fsz = ysz + 2 * csz;                 /* YUV420 compact */
    uint8_t *frame = malloc(fsz);
    if (!frame) return NULL;

    while (cam_stream_run) {
        int r = read_full(cam_stream_fd, frame, fsz);
        if (r <= 0) break;

        const uint8_t *Y = frame;
        const uint8_t *U = frame + ysz;
        const uint8_t *V = U + csz;
        uint16_t *out = (uint16_t *)cam_stream_buf;

        for (int y = 0; y < h; y++) {
            const uint8_t *yr = Y + (size_t)y * w;
            const uint8_t *ur = U + (size_t)(y / 2) * (w / 2);
            const uint8_t *vr = V + (size_t)(y / 2) * (w / 2);
            uint16_t      *orow = out + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                int C = (int)yr[x] - 16;
                int D = (int)ur[x >> 1] - 128;
                int E = (int)vr[x >> 1] - 128;
                uint8_t R = clamp8((298 * C + 409 * E + 128) >> 8);
                uint8_t G = clamp8((298 * C - 100 * D - 208 * E + 128) >> 8);
                uint8_t B = clamp8((298 * C + 516 * D + 128) >> 8);
                orow[x] = (uint16_t)(((R & 0xF8) << 8) |
                                     ((G & 0xFC) << 3) |
                                     ( B >> 3));
            }
        }

        if (!cam_frame_pending) {
            cam_frame_pending = 1;
            lv_async_call(cam_frame_deliver, NULL);
        }
    }
    free(frame);
    return NULL;
}

void sys_cam_stream_start(uint8_t *buf, int w, int h,
                          cam_frame_cb_t on_frame, void *user)
{
    if (cam_stream_run) return;                 /* deja actif */

    cam_stream_buf  = buf;
    cam_stream_w    = w;
    cam_stream_h    = h;
    cam_stream_cb   = on_frame;
    cam_stream_user = user;
    cam_frame_pending = 0;

    /* formate les arguments avant fork (snprintf n'est pas async-signal-safe) */
    char ws[8], hs[8];
    snprintf(ws, sizeof ws, "%d", w);
    snprintf(hs, sizeof hs, "%d", h);

    int fds[2];
    if (pipe(fds) != 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        /* enfant : stdout -> pipe, stderr -> /dev/null, exec rpicam-vid */
        dup2(fds[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        close(fds[0]); close(fds[1]);
        execlp("rpicam-vid", "rpicam-vid", "-n", "-t", "0",
               "--framerate", "12", "--width", ws, "--height", hs,
               "--codec", "yuv420", "--flush", "-o", "-", (char *)NULL);
        _exit(127);
    }

    /* parent */
    close(fds[1]);
    cam_stream_fd  = fds[0];
    cam_stream_pid = pid;
    cam_stream_run = 1;
    if (pthread_create(&cam_stream_tid, NULL, cam_stream_thread, NULL) != 0) {
        cam_stream_run = 0;
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        close(cam_stream_fd); cam_stream_fd = -1; cam_stream_pid = -1;
    }
}

void sys_cam_stream_stop(void)
{
    if (!cam_stream_run && cam_stream_pid < 0) return;

    cam_stream_run = 0;
    if (cam_stream_pid > 0) kill(cam_stream_pid, SIGTERM);
    pthread_join(cam_stream_tid, NULL);          /* le SIGTERM -> EOF debloque read() */
    if (cam_stream_fd >= 0) { close(cam_stream_fd); cam_stream_fd = -1; }
    if (cam_stream_pid > 0) waitpid(cam_stream_pid, NULL, 0);
    cam_stream_pid = -1;

    lv_async_call_cancel(cam_frame_deliver, NULL);
    cam_frame_pending = 0;
}

bool sys_cam_stream_active(void)
{
    return cam_stream_run != 0;
}

/* ---------- Camera : galerie ---------- */
int sys_cam_photo_list(char paths[][256], int max)
{
    DIR *d = opendir(PHOTO_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 5 || strcasecmp(nm + l - 4, ".jpg") != 0) continue;
        snprintf(paths[n], 256, PHOTO_DIR "/%s", nm);
        n++;
    }
    closedir(d);
    /* tri decroissant : noms cam_YYYYMMDD_HHMMSS triables lexicalement
     * -> les plus recents en premier */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(paths[j], paths[i]) > 0) {
                char tmp[256];
                strcpy(tmp, paths[i]); strcpy(paths[i], paths[j]); strcpy(paths[j], tmp);
            }
    return n;
}

void sys_cam_photo_delete(const char *path)
{
    if (path && *path) unlink(path);
}

typedef struct {
    cam_preview_cb_t cb; void *user;
    int  w, h;
    char jpg[256], preview[256];
    bool ok;
} campv_ctx_t;

static void campv_deliver(void *a)
{
    campv_ctx_t *c = a;
    if (c->cb) c->cb(c->ok, c->preview, c->user);
    free(c);
}

static void *campv_thread(void *a)
{
    campv_ctx_t *c = a; char cmd[768];
    strncpy(c->preview, CAM_PREVIEW, sizeof(c->preview) - 1);
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/python3 /home/bq-lora/bq-lora-ui/tools/cam.py '%s' '%s' %d %d "
             ">/dev/null 2>&1", c->jpg, c->preview, c->w, c->h);
    c->ok = (system(cmd) == 0);
    lv_async_call(campv_deliver, c);
    return NULL;
}

void sys_cam_preview_async(const char *jpg_path, int w, int h,
                           cam_preview_cb_t cb, void *user)
{
    campv_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user; c->w = w; c->h = h;
    strncpy(c->jpg, jpg_path, sizeof(c->jpg) - 1);
    pthread_t t; pthread_create(&t, NULL, campv_thread, c); pthread_detach(t);
}

/* ---------- Camera : scanner QR (flux live + decode libzbar) ----------
 * Reprend exactement la mecanique de sys_cam_stream_* (rpicam-vid YUV420
 * dans un pipe, conversion vers RGB565 pour le canvas), mais a chaque frame
 * le plan Y (greyscale natif) est passe a un zbar_image_scanner. A la
 * premiere detection, on appelle on_hit() sur le thread UI avec le payload.
 * Variables et pthread separes du flux camera 'normal' : les deux ne peuvent
 * pas tourner en parallele (camera mono-acces) mais le code reste isole. */
#include <zbar.h>
static volatile int      qr_run;
static pthread_t         qr_tid;
static pid_t             qr_pid = -1;
static int               qr_fd  = -1;
static uint8_t          *qr_buf;
static int               qr_w, qr_h;
static cam_frame_cb_t    qr_frame_cb;
static qr_hit_cb_t       qr_hit_cb;
static void             *qr_user;
static volatile int      qr_frame_pending;
static volatile int      qr_hit_fired;
static char              qr_hit_payload[512];

static void qr_frame_deliver(void *unused)
{
    (void)unused;
    qr_frame_pending = 0;
    if (qr_frame_cb && qr_run) qr_frame_cb(qr_user);
}

static void qr_hit_deliver(void *unused)
{
    (void)unused;
    if (qr_hit_cb) qr_hit_cb(qr_hit_payload, qr_user);
}

static void *qr_thread(void *a)
{
    (void)a;
    int w = qr_w, h = qr_h;
    size_t ysz = (size_t)w * h;
    size_t csz = (size_t)(w / 2) * (h / 2);
    size_t fsz = ysz + 2 * csz;
    uint8_t *frame = malloc(fsz);
    if (!frame) return NULL;

    zbar_image_scanner_t *scanner = zbar_image_scanner_create();
    /* on ne s'interesse qu'au QR : evite le bruit des codes-barres lineaires */
    zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_ENABLE, 0);
    zbar_image_scanner_set_config(scanner, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);

    int decimate = 0;       /* on ne scanne qu'une frame sur 2 pour soulager le CPU */

    while (qr_run) {
        int r = read_full(qr_fd, frame, fsz);
        if (r <= 0) break;

        /* conversion YUV420 -> RGB565 (identique a cam_stream_thread) */
        const uint8_t *Y = frame;
        const uint8_t *U = frame + ysz;
        const uint8_t *V = U + csz;
        uint16_t *out = (uint16_t *)qr_buf;
        for (int y = 0; y < h; y++) {
            const uint8_t *yr = Y + (size_t)y * w;
            const uint8_t *ur = U + (size_t)(y / 2) * (w / 2);
            const uint8_t *vr = V + (size_t)(y / 2) * (w / 2);
            uint16_t      *orow = out + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                int C = (int)yr[x] - 16;
                int D = (int)ur[x >> 1] - 128;
                int E = (int)vr[x >> 1] - 128;
                uint8_t R = clamp8((298 * C + 409 * E + 128) >> 8);
                uint8_t G = clamp8((298 * C - 100 * D - 208 * E + 128) >> 8);
                uint8_t B = clamp8((298 * C + 516 * D + 128) >> 8);
                orow[x] = (uint16_t)(((R & 0xF8) << 8) |
                                     ((G & 0xFC) << 3) |
                                     ( B >> 3));
            }
        }
        if (!qr_frame_pending) {
            qr_frame_pending = 1;
            lv_async_call(qr_frame_deliver, NULL);
        }

        /* decode QR sur le plan Y (greyscale natif) -- une frame sur 2 */
        if (!qr_hit_fired && (decimate++ & 1) == 0) {
            zbar_image_t *img = zbar_image_create();
            zbar_image_set_format(img, *(int *)"Y800");
            zbar_image_set_size(img, w, h);
            zbar_image_set_data(img, Y, ysz, NULL);
            int n = zbar_scan_image(scanner, img);
            if (n > 0) {
                const zbar_symbol_t *s = zbar_image_first_symbol(img);
                if (s) {
                    const char *data = zbar_symbol_get_data(s);
                    if (data && data[0]) {
                        strncpy(qr_hit_payload, data, sizeof(qr_hit_payload) - 1);
                        qr_hit_payload[sizeof(qr_hit_payload) - 1] = 0;
                        qr_hit_fired = 1;
                        lv_async_call(qr_hit_deliver, NULL);
                    }
                }
            }
            zbar_image_destroy(img);
        }
    }

    zbar_image_scanner_destroy(scanner);
    free(frame);
    return NULL;
}

void sys_qr_start(uint8_t *buf, int w, int h,
                  cam_frame_cb_t on_frame, qr_hit_cb_t on_hit, void *user)
{
    if (qr_run) return;
    qr_buf = buf; qr_w = w; qr_h = h;
    qr_frame_cb = on_frame; qr_hit_cb = on_hit; qr_user = user;
    qr_frame_pending = 0; qr_hit_fired = 0; qr_hit_payload[0] = 0;

    char ws[8], hs[8];
    snprintf(ws, sizeof ws, "%d", w);
    snprintf(hs, sizeof hs, "%d", h);

    int fds[2];
    if (pipe(fds) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        close(fds[0]); close(fds[1]);
        execlp("rpicam-vid", "rpicam-vid", "-n", "-t", "0",
               "--framerate", "12", "--width", ws, "--height", hs,
               "--codec", "yuv420", "--flush", "-o", "-", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    qr_fd = fds[0]; qr_pid = pid; qr_run = 1;
    if (pthread_create(&qr_tid, NULL, qr_thread, NULL) != 0) {
        qr_run = 0;
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        close(qr_fd); qr_fd = -1; qr_pid = -1;
    }
}

void sys_qr_stop(void)
{
    if (!qr_run && qr_pid < 0) return;
    qr_run = 0;
    if (qr_pid > 0) kill(qr_pid, SIGTERM);
    pthread_join(qr_tid, NULL);
    if (qr_fd >= 0) { close(qr_fd); qr_fd = -1; }
    if (qr_pid > 0) waitpid(qr_pid, NULL, 0);
    qr_pid = -1;
    lv_async_call_cancel(qr_frame_deliver, NULL);
    lv_async_call_cancel(qr_hit_deliver, NULL);
    qr_frame_pending = 0;
}

#define PWM_DUTY   "/sys/class/pwm/pwmchip0/pwm0/duty_cycle"
#define PWM_PERIOD 1000000

int sys_backlight_get(void)
{
    char b[32]; run_get("cat " PWM_DUTY " 2>/dev/null", b, sizeof(b));
    int d = atoi(b);
    return (d * 100 + PWM_PERIOD / 2) / PWM_PERIOD;
}

void sys_backlight_set(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int d = pct * PWM_PERIOD / 100;

    /* Tente l'ecriture directe (permission group video posee par backlight-init.sh). */
    FILE *f = fopen(PWM_DUTY, "w");
    if (f) { fprintf(f, "%d\n", d); fclose(f); return; }

    /* Fallback via le helper privilegie. */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), CTL " backlight %d &", d);
    system(cmd);
}

void sys_beep(int freq_hz, int duration_ms)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/python3 /home/bq-lora/bq-lora-ui/tools/beep.py %d %d >/dev/null 2>&1 &",
             freq_hz, duration_ms);
    system(cmd);
}

#define REPO "/home/bq-lora/bq-lora-ui"

typedef struct {
    update_check_cb_t cb;
    void *user;
    char local[16], remote[16];
    bool available;
} upd_ctx_t;

static void upd_check_deliver(void *a)
{
    upd_ctx_t *c = a;
    if (c->cb) c->cb(c->available, c->local, c->remote, c->user);
    free(c);
}

static void *upd_check_thread(void *a)
{
    upd_ctx_t *c = a;
    /* fetch silencieux (peut echouer si pas d'internet) */
    system("git -C " REPO " fetch -q origin master 2>/dev/null");
    run_get("git -C " REPO " rev-parse --short HEAD 2>/dev/null", c->local, sizeof(c->local));
    run_get("git -C " REPO " rev-parse --short origin/master 2>/dev/null", c->remote, sizeof(c->remote));
    if (!c->local[0])  strncpy(c->local,  "?", sizeof(c->local));
    if (!c->remote[0]) strncpy(c->remote, "?", sizeof(c->remote));
    c->available = (c->local[0] != '?' && c->remote[0] != '?' && strcmp(c->local, c->remote) != 0);
    lv_async_call(upd_check_deliver, c);
    return NULL;
}

void sys_update_check_async(update_check_cb_t cb, void *user)
{
    upd_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    pthread_t t; pthread_create(&t, NULL, upd_check_thread, c); pthread_detach(t);
}

typedef struct { update_apply_cb_t cb; void *user; bool ok; } upd_apply_ctx_t;
static void upd_apply_deliver(void *a) { upd_apply_ctx_t *c = a; if (c->cb) c->cb(c->ok, c->user); free(c); }
static void *upd_apply_thread(void *a)
{
    upd_apply_ctx_t *c = a;
    /* bq-lora-ui-ctl update fait pull + build + restart (privilegies). Le restart
     * tuera ce process avant le callback ; on signale just before. */
    c->ok = (system(CTL " update") == 0);
    lv_async_call(upd_apply_deliver, c);
    return NULL;
}
#define UPD_PROGRESS_FILE "/tmp/bq-lora-ui-update.progress"

void sys_update_apply_async(update_apply_cb_t cb, void *user)
{
    remove(UPD_PROGRESS_FILE);   /* repart de zero (evite de lire un ancien jalon) */
    upd_apply_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    pthread_t t; pthread_create(&t, NULL, upd_apply_thread, c); pthread_detach(t);
}

/* Jalon de progression ecrit par bq-lora-ui-update.sh : 0..100, ou -1 si echec,
 * ou 0 si le fichier n'existe pas encore. */
int sys_update_progress(void)
{
    FILE *f = fopen(UPD_PROGRESS_FILE, "r");
    if (!f) return 0;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    if (v < -1) v = -1;
    if (v > 100) v = 100;
    return v;
}

void sys_log_tail(char *out, int cap, int n_lines)
{
    out[0] = 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "journalctl -n %d --no-pager --no-hostname -o short 2>&1 | tail -%d",
             n_lines, n_lines);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    int used = 0;
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        int n = (int)strlen(line);
        if (used + n + 1 >= cap) break;
        memcpy(out + used, line, n);
        used += n;
    }
    out[used] = 0;
    pclose(p);
}

/* ---------- WiFi (asynchrone, via pthread + lv_async_call) ---------- */
typedef struct {
    wifi_scan_cb_t cb;
    void *user;
    wifi_net_t *list;
    int n;
} scan_ctx_t;

static void scan_deliver(void *arg)
{
    scan_ctx_t *c = arg;
    c->cb(c->list, c->n, c->user);
    free(c->list);
    free(c);
}

static void *scan_thread(void *arg)
{
    scan_ctx_t *c = arg;
    FILE *p = popen(CTL " wifi-scan 2>/dev/null", "r");
    int cap = 16; c->list = malloc(sizeof(wifi_net_t) * cap); c->n = 0;
    if (p) {
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            if (c->n >= cap) {
                cap *= 2;
                c->list = realloc(c->list, sizeof(wifi_net_t) * cap);
            }
            /* nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY : 4 champs séparés par ':' */
            char *f1 = line, *f2, *f3, *f4;
            f2 = strchr(f1, ':'); if (!f2) continue; *f2++ = 0;
            f3 = strchr(f2, ':'); if (!f3) continue; *f3++ = 0;
            f4 = strchr(f3, ':'); if (!f4) continue; *f4++ = 0;
            chomp(f4);
            if (!f2[0]) continue;                 /* SSID vide */
            wifi_net_t *n = &c->list[c->n++];
            strncpy(n->ssid, f2, sizeof(n->ssid) - 1);
            n->ssid[sizeof(n->ssid) - 1] = 0;
            n->signal = atoi(f3);
            n->active = (f1[0] == '*');
            n->secured = !(strcmp(f4, "--") == 0 || strcmp(f4, "") == 0);
        }
        pclose(p);
    }
    lv_async_call(scan_deliver, c);
    return NULL;
}

void sys_wifi_scan_async(wifi_scan_cb_t cb, void *user)
{
    scan_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    pthread_t t; pthread_create(&t, NULL, scan_thread, c); pthread_detach(t);
}

typedef struct {
    wifi_connect_cb_t cb;
    void *user;
    char ssid[64], pass[128];
    bool ok;
    char msg[256];
} conn_ctx_t;

static void conn_deliver(void *arg)
{
    conn_ctx_t *c = arg;
    c->cb(c->ok, c->msg, c->user);
    free(c);
}

static void *conn_thread(void *arg)
{
    conn_ctx_t *c = arg;
    char cmd[512];
    if (c->pass[0])
        snprintf(cmd, sizeof(cmd), CTL " wifi-connect '%s' '%s' 2>&1", c->ssid, c->pass);
    else
        snprintf(cmd, sizeof(cmd), CTL " wifi-connect '%s' '' 2>&1", c->ssid);
    FILE *p = popen(cmd, "r");
    c->msg[0] = 0;
    if (p) {
        char line[256];
        while (fgets(line, sizeof(line), p)) {
            strncat(c->msg, line, sizeof(c->msg) - strlen(c->msg) - 1);
        }
        c->ok = (pclose(p) == 0);
    } else {
        c->ok = false;
        strncpy(c->msg, "popen failed", sizeof(c->msg));
    }
    chomp(c->msg);
    lv_async_call(conn_deliver, c);
    return NULL;
}

void sys_wifi_connect_async(const char *ssid, const char *password,
                            wifi_connect_cb_t cb, void *user)
{
    conn_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    strncpy(c->ssid, ssid, sizeof(c->ssid) - 1);
    if (password) strncpy(c->pass, password, sizeof(c->pass) - 1);
    pthread_t t; pthread_create(&t, NULL, conn_thread, c); pthread_detach(t);
}

/* ---------- WPS push-button (PBC) ----------
 * wpa_cli wps_pbc rend la main immediatement ("OK") apres avoir lance le scan
 * WPS cote supplicant. On polle ensuite nmcli pour detecter l'apparition d'une
 * connexion active sur wlan0 (timeout 120 s, valeur standard WPS). */
static void wps_deliver(void *arg)
{
    conn_ctx_t *c = arg;
    c->cb(c->ok, c->msg, c->user);
    free(c);
}

static void *wps_thread(void *arg)
{
    conn_ctx_t *c = arg;

    /* nom de la connexion active actuelle sur wlan0 (peut etre vide) */
    char before[64] = "";
    run_get("nmcli -t -f DEVICE,CONNECTION device status 2>/dev/null "
            "| awk -F: '$1==\"wlan0\"{print $2}'", before, sizeof(before));

    /* declenche WPS push-button */
    FILE *p = popen(CTL " wifi-wps 2>&1", "r");
    char buf[128] = "";
    if (p) { fgets(buf, sizeof(buf), p); pclose(p); chomp(buf); }
    if (strncmp(buf, "OK", 2) != 0) {
        c->ok = false;
        snprintf(c->msg, sizeof(c->msg),
                 "wps_pbc echec : %s", buf[0] ? buf : "supplicant injoignable");
        lv_async_call(wps_deliver, c);
        return NULL;
    }

    /* attend qu'une connexion active autre que la precedente apparaisse */
    char cur[64];
    for (int i = 0; i < 120; i++) {                 /* ~120 s */
        sleep(1);
        cur[0] = 0;
        run_get("nmcli -t -f DEVICE,STATE,CONNECTION device status 2>/dev/null "
                "| awk -F: '$1==\"wlan0\" && $2==\"connected\"{print $3}'",
                cur, sizeof(cur));
        if (cur[0] && strcmp(cur, before) != 0) {
            c->ok = true;
            snprintf(c->msg, sizeof(c->msg), "connecte : %s", cur);
            lv_async_call(wps_deliver, c);
            return NULL;
        }
    }
    c->ok = false;
    snprintf(c->msg, sizeof(c->msg), "timeout WPS (120s)");
    lv_async_call(wps_deliver, c);
    return NULL;
}

void sys_wifi_wps_async(wifi_connect_cb_t cb, void *user)
{
    conn_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    pthread_t t; pthread_create(&t, NULL, wps_thread, c); pthread_detach(t);
}

/* ---------- Bluetooth (asynchrone, via pthread + lv_async_call) ---------- */
typedef struct {
    bt_scan_cb_t cb;
    void *user;
    bt_device_t *list;
    int n;
} bt_scan_ctx_t;

static void bt_scan_deliver(void *arg)
{
    bt_scan_ctx_t *c = arg;
    c->cb(c->list, c->n, c->user);
    free(c->list);
    free(c);
}

static void *bt_scan_thread(void *arg)
{
    bt_scan_ctx_t *c = arg;
    int cap = 16; c->list = malloc(sizeof(bt_device_t) * cap); c->n = 0;
    FILE *p = popen(CTL " bt-scan 8 2>/dev/null", "r");
    if (p) {
        char line[256];
        while (fgets(line, sizeof(line), p)) {
            /* MAC|NOM|RSSI|paired|connected| */
            char *f[6]; int nf = 0; char *s = line;
            for (; nf < 6; nf++) {
                f[nf] = s;
                char *sep = strchr(s, '|');
                if (!sep) { nf++; break; }
                *sep = 0; s = sep + 1;
            }
            if (nf < 5) continue;
            chomp(f[1]);
            if (!f[0][0]) continue;
            if (c->n >= cap) { cap *= 2; c->list = realloc(c->list, sizeof(bt_device_t) * cap); }
            bt_device_t *d = &c->list[c->n++];
            memset(d, 0, sizeof(*d));
            strncpy(d->addr, f[0], sizeof(d->addr) - 1);
            strncpy(d->name, f[1][0] ? f[1] : f[0], sizeof(d->name) - 1);
            d->rssi      = atoi(f[2]);
            d->paired    = (f[3][0] == '1');
            d->connected = (f[4][0] == '1');
        }
        pclose(p);
    }
    lv_async_call(bt_scan_deliver, c);
    return NULL;
}

void sys_bt_scan_async(bt_scan_cb_t cb, void *user)
{
    bt_scan_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    pthread_t t; pthread_create(&t, NULL, bt_scan_thread, c); pthread_detach(t);
}

typedef struct {
    bt_action_cb_t cb;
    void *user;
    char verb[16], addr[18];
    bool ok;
    char msg[256];
} bt_act_ctx_t;

static void bt_act_deliver(void *arg)
{
    bt_act_ctx_t *c = arg;
    if (c->cb) c->cb(c->ok, c->msg, c->user);
    free(c);
}

static void *bt_act_thread(void *arg)
{
    bt_act_ctx_t *c = arg;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), CTL " bt-%s '%s' 2>&1", c->verb, c->addr);
    FILE *p = popen(cmd, "r");
    c->msg[0] = 0;
    if (p) {
        char line[256];
        while (fgets(line, sizeof(line), p))
            strncat(c->msg, line, sizeof(c->msg) - strlen(c->msg) - 1);
        int rc = pclose(p);
        /* bluetoothctl renvoie souvent 0 ; on confirme via le texte de sortie */
        bool fail = strstr(c->msg, "Failed") || strstr(c->msg, "not available")
                 || strstr(c->msg, "org.bluez.Error");
        bool ok_word = strstr(c->msg, "successful") || strstr(c->msg, "Connected: yes")
                    || strstr(c->msg, "Paired: yes") || strstr(c->msg, "Device has been removed");
        c->ok = (rc == 0 && !fail) || ok_word;
    } else {
        c->ok = false;
        strncpy(c->msg, "popen failed", sizeof(c->msg));
    }
    chomp(c->msg);
    lv_async_call(bt_act_deliver, c);
    return NULL;
}

void sys_bt_action_async(const char *verb, const char *addr,
                         bt_action_cb_t cb, void *user)
{
    bt_act_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    strncpy(c->verb, verb, sizeof(c->verb) - 1);
    strncpy(c->addr, addr, sizeof(c->addr) - 1);
    pthread_t t; pthread_create(&t, NULL, bt_act_thread, c); pthread_detach(t);
}

bool sys_bt_serial_active(void)
{
    char b[16];
    run_get(CTL " bt-serial-status 2>/dev/null", b, sizeof(b));
    return strcmp(b, "active") == 0;
}

void sys_bt_serial_set(bool on)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), CTL " %s &", on ? "bt-serial-on" : "bt-serial-off");
    system(cmd);
}
