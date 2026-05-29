#include "sys.h"
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
void sys_reboot(void)   { system(CTL " reboot &"); }
void sys_shutdown(void) { system(CTL " poweroff &"); }

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
