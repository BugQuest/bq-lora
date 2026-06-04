#include "settings.h"
#include <stdio.h>
#include <string.h>

#define CONFIG_PATH "/home/bq-lora/meshui/config.ini"

/* Valeurs courantes (initialisees aux defauts, ecrasees par config.ini). */
static char s_node[64]    = "NODE-7F3A";
static char s_ssid[64]    = "BugQuest-Lora";
static char s_pass[64]    = "bugquest-lora";
static char s_tz[64]      = "Europe/Paris";
static int  s_mesh_en     = 1;     /* 1 = l'UI se connecte a meshtasticd */

static void copy_in(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

void settings_load(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        rstrip(val);
        if      (!strcmp(key, "node_name"))    copy_in(s_node, sizeof(s_node), val);
        else if (!strcmp(key, "hotspot_ssid")) copy_in(s_ssid, sizeof(s_ssid), val);
        else if (!strcmp(key, "hotspot_pass")) copy_in(s_pass, sizeof(s_pass), val);
        else if (!strcmp(key, "timezone"))     copy_in(s_tz,   sizeof(s_tz),   val);
        else if (!strcmp(key, "mesh_enabled")) s_mesh_en = (val[0] == '0') ? 0 : 1;
    }
    fclose(f);
}

void settings_save(void)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# meshui config (genere par l'UI)\n");
    fprintf(f, "node_name=%s\n",    s_node);
    fprintf(f, "hotspot_ssid=%s\n", s_ssid);
    fprintf(f, "hotspot_pass=%s\n", s_pass);
    fprintf(f, "timezone=%s\n",     s_tz);
    fprintf(f, "mesh_enabled=%d\n", s_mesh_en);
    fclose(f);
}

const char *settings_node_name(void)    { return s_node; }
const char *settings_hotspot_ssid(void) { return s_ssid; }
const char *settings_hotspot_pass(void) { return s_pass; }
const char *settings_timezone(void)     { return s_tz; }
bool        settings_mesh_enabled(void)  { return s_mesh_en != 0; }

void settings_set_node_name(const char *v)    { copy_in(s_node, sizeof(s_node), v); }
void settings_set_hotspot_ssid(const char *v) { copy_in(s_ssid, sizeof(s_ssid), v); }
void settings_set_hotspot_pass(const char *v) { copy_in(s_pass, sizeof(s_pass), v); }
void settings_set_timezone(const char *v)     { copy_in(s_tz,   sizeof(s_tz),   v); }
void settings_set_mesh_enabled(bool v)        { s_mesh_en = v ? 1 : 0; }
