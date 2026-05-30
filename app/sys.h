#pragma once
#include <stdbool.h>

/* Infos système instantanées (pour l'onglet SYS). */
typedef struct {
    char  hostname[64];
    char  ip_wlan[40];
    char  ip_usb[40];
    char  uptime[32];
    float cpu_temp_c;
    int   mem_used_mb;
    int   mem_total_mb;
    int   disk_used_pct;
    char  kernel[64];
    bool  throttled_now;
    bool  throttled_ever;
    char  wifi_ssid[64];
    int   wifi_signal;       /* 0-100 ; -1 si déconnecté */
} sys_info_t;

void sys_info_get(sys_info_t *out);

/* Actions privilégiées — passent par /usr/local/sbin/meshui-ctl (sudo NOPASSWD). */
void sys_reboot(void);
void sys_shutdown(void);
void sys_restart_app(void);    /* systemctl restart meshui */
bool sys_ssh_enabled(void);
bool sys_ssh_running(void);
void sys_ssh_set(bool on);

bool sys_hotspot_active(void);
void sys_hotspot_set(bool on);

/* Radio WiFi (nmcli radio wifi on/off). */
bool sys_wifi_radio_on(void);
void sys_wifi_radio_set(bool on);

/* Bluetooth (rfkill + bluetooth.service). */
bool sys_bt_on(void);
void sys_bt_set(bool on);

/* Change le fuseau horaire systeme (timedatectl set-timezone). */
void sys_set_timezone(const char *tz);

/* Rétroéclairage : pourcentage 0..100, lecture via /sys/class/pwm. */
int  sys_backlight_get(void);
void sys_backlight_set(int pct);

/* Beep court sur le piezo GPIO17 (asynchrone). */
void sys_beep(int freq_hz, int duration_ms);

/* Dernières lignes du journal système (journalctl -n N). */
void sys_log_tail(char *out, int cap, int n_lines);

/* WiFi : scan + connexion asynchrones. Les callbacks sont rappelés sur le thread UI. */
typedef struct {
    char ssid[64];
    int  signal;     /* 0-100 */
    bool secured;
    bool active;
} wifi_net_t;

typedef void (*wifi_scan_cb_t)(const wifi_net_t *list, int n, void *user);
void sys_wifi_scan_async(wifi_scan_cb_t cb, void *user);

typedef void (*wifi_connect_cb_t)(bool ok, const char *msg, void *user);
void sys_wifi_connect_async(const char *ssid, const char *password,
                            wifi_connect_cb_t cb, void *user);
