// main/live_config.c -- see live_config.h.
#include "live_config.h"

#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

void live_config_defaults(live_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_str(cfg->host, sizeof(cfg->host), LIVE_CFG_DEFAULT_HOST);
    copy_str(cfg->voice, sizeof(cfg->voice), LIVE_CFG_DEFAULT_VOICE);
    copy_str(cfg->prompt, sizeof(cfg->prompt), LIVE_CFG_DEFAULT_PROMPT);
}

bool live_config_valid_ssid(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    return n >= 1 && n <= LIVE_CFG_SSID_MAX;
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool live_config_valid_wifi_pass(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0) return true;
    if (n == 64) {
        for (size_t i = 0; i < n; i++) {
            if (!is_hex(s[i])) return false;
        }
        return true;
    }
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

bool live_config_valid_api_key(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 16 || n > LIVE_CFG_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

bool live_config_valid_host(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 3 || n > LIVE_CFG_HOST_MAX || s[0] == '.' || s[0] == '-' || s[0] == ':') return false;
    const char *colon = strchr(s, ':');
    size_t host_len = colon ? (size_t)(colon - s) : n;
    if (host_len == 0) return false;
    for (size_t i = 0; i < host_len; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-';
        if (!ok) return false;
    }
    if (colon) {
        const char *p = colon + 1;
        size_t digits = strlen(p);
        if (digits == 0 || digits > 5) return false;
        long port = 0;
        for (size_t i = 0; i < digits; i++) {
            if (p[i] < '0' || p[i] > '9') return false;
            port = port * 10 + (p[i] - '0');
        }
        if (port < 1 || port > 65535) return false;
    }
    return true;
}

bool live_config_valid_voice(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 1 || n > LIVE_CFG_VOICE_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool live_config_valid_prompt(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n > LIVE_CFG_PROMPT_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 && c != '\n') return false;
    }
    return true;
}

bool live_config_ready(const live_config_t *cfg)
{
    return cfg && cfg->wifi_count > 0 && cfg->api_key[0];
}

int live_wifi_find(const live_config_t *cfg, const char *ssid)
{
    if (!cfg || !ssid) return -1;
    for (int i = 0; i < cfg->wifi_count; i++) {
        if (strcmp(cfg->wifi[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

bool live_wifi_upsert(live_config_t *cfg, const char *ssid, const char *pass)
{
    if (!cfg || !live_config_valid_ssid(ssid) || !live_config_valid_wifi_pass(pass)) return false;
    int i = live_wifi_find(cfg, ssid);
    if (i < 0) {
        if (cfg->wifi_count == LIVE_CFG_WIFI_MAX) {
            memmove(&cfg->wifi[0], &cfg->wifi[1], sizeof(cfg->wifi[0]) * (LIVE_CFG_WIFI_MAX - 1));
            cfg->wifi_count--;
        }
        i = cfg->wifi_count++;
        copy_str(cfg->wifi[i].ssid, sizeof(cfg->wifi[i].ssid), ssid);
    }
    copy_str(cfg->wifi[i].pass, sizeof(cfg->wifi[i].pass), pass);
    return true;
}

bool live_wifi_prefer(live_config_t *cfg, const char *ssid)
{
    int i = live_wifi_find(cfg, ssid);
    if (i < 0) return false;
    live_wifi_net_t net = cfg->wifi[i];
    memmove(&cfg->wifi[1], &cfg->wifi[0], sizeof(cfg->wifi[0]) * (size_t)i);
    cfg->wifi[0] = net;
    return true;
}

bool live_wifi_remove(live_config_t *cfg, const char *ssid)
{
    int i = live_wifi_find(cfg, ssid);
    if (i < 0) return false;
    memmove(&cfg->wifi[i], &cfg->wifi[i + 1], sizeof(cfg->wifi[0]) * (size_t)(cfg->wifi_count - i - 1));
    cfg->wifi_count--;
    memset(&cfg->wifi[cfg->wifi_count], 0, sizeof(cfg->wifi[0]));
    return true;
}

int live_wifi_pick(const live_config_t *cfg, const live_scan_ap_t *aps, int n, unsigned tried_mask)
{
    // Best RSSI per saved network.
    int rssi[LIVE_CFG_WIFI_MAX];
    for (int i = 0; i < LIVE_CFG_WIFI_MAX; i++) rssi[i] = -1000;
    for (int a = 0; a < n; a++) {
        if (!aps[a].ssid) continue;
        int i = live_wifi_find(cfg, aps[a].ssid);
        if (i >= 0 && aps[a].rssi > rssi[i]) rssi[i] = aps[a].rssi;
    }
    for (int i = 0; i < cfg->wifi_count; i++) {
        if (!(tried_mask & (1u << i)) && rssi[i] >= LIVE_WIFI_USABLE_RSSI) return i;
    }
    int best = -1;
    int best_rssi = -1000;
    for (int a = 0; a < n; a++) {
        if (!aps[a].ssid) continue;
        int i = live_wifi_find(cfg, aps[a].ssid);
        if (i < 0 || (tried_mask & (1u << i))) continue;
        if (aps[a].rssi > best_rssi) {
            best_rssi = aps[a].rssi;
            best = i;
        }
    }
    return best;
}

void live_config_mask_key(const char *key, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return;
    size_t n = key ? strlen(key) : 0;
    if (n == 0) {
        snprintf(out, out_cap, "(未设置)");
    } else if (n <= 12) {
        snprintf(out, out_cap, "****");
    } else {
        snprintf(out, out_cap, "%.7s...%s", key, key + n - 4);
    }
}
