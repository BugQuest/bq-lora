#include "sys.h"
#include "settings.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define CTL "/usr/bin/sudo /usr/local/sbin/meshui-ctl"

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

/* ---------- actions ---------- */
void sys_reboot(void)      { system(CTL " reboot &"); }
void sys_shutdown(void)    { system(CTL " poweroff &"); }
void sys_restart_app(void) { system(CTL " restart-meshui &"); }

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
             "/usr/bin/python3 /home/bq-lora/meshui/tools/badusb.py '%s' >/dev/null 2>&1",
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
             "/usr/bin/python3 /home/bq-lora/meshui/tools/beep.py %d %d >/dev/null 2>&1 &",
             freq_hz, duration_ms);
    system(cmd);
}

#define REPO "/home/bq-lora/meshui"

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
    /* meshui-ctl update fait pull + build + restart (privilegies). Le restart
     * tuera ce process avant le callback ; on signale just before. */
    c->ok = (system(CTL " update") == 0);
    lv_async_call(upd_apply_deliver, c);
    return NULL;
}
void sys_update_apply_async(update_apply_cb_t cb, void *user)
{
    upd_apply_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->user = user;
    pthread_t t; pthread_create(&t, NULL, upd_apply_thread, c); pthread_detach(t);
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
