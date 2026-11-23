// main/live_config.h -- validation and masking for provisioned settings.
// Pure C, host-tested by tests/test_live_config.c. Storage lives in app_config.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define LIVE_CFG_SSID_MAX      32
#define LIVE_CFG_WIFI_PASS_MAX 64
#define LIVE_CFG_KEY_MAX      200
#define LIVE_CFG_HOST_MAX      96
#define LIVE_CFG_VOICE_MAX     32
#define LIVE_CFG_PROMPT_MAX   600

#define LIVE_CFG_DEFAULT_HOST  "api.xsirouter.com"
#define LIVE_CFG_DEFAULT_VOICE "marin"
#define LIVE_CFG_DEFAULT_PROMPT \
    "你是 AI Passport 上的中文语音助手。回答要口语化、简洁，一般不超过三句话。"

#define LIVE_CFG_WIFI_MAX       4   // saved Wi-Fi networks (e.g. home IoT SSID + phone hotspot)

typedef struct {
    char ssid[LIVE_CFG_SSID_MAX + 1];
    char pass[LIVE_CFG_WIFI_PASS_MAX + 1];
} live_wifi_net_t;

typedef struct {
    live_wifi_net_t wifi[LIVE_CFG_WIFI_MAX];
    int wifi_count;
    char api_key[LIVE_CFG_KEY_MAX + 1];
    char host[LIVE_CFG_HOST_MAX + 1];
    char voice[LIVE_CFG_VOICE_MAX + 1];
    char prompt[LIVE_CFG_PROMPT_MAX + 1];
} live_config_t;

void live_config_defaults(live_config_t *cfg);

bool live_config_valid_ssid(const char *s);        // 1..32 bytes
bool live_config_valid_wifi_pass(const char *s);   // empty (open) or 8..63 printable / 64 hex
bool live_config_valid_api_key(const char *s);     // 16..200 printable ASCII, no spaces
bool live_config_valid_host(const char *s);        // hostname[:port], letters/digits/.-
bool live_config_valid_voice(const char *s);       // 1..32 of [a-z0-9_-]
bool live_config_valid_prompt(const char *s);      // 0..600 bytes, no control chars but \n

// True when at least one Wi-Fi network and the API key are present.
bool live_config_ready(const live_config_t *cfg);

// Saved network list. Upsert updates the password of an existing SSID or appends;
// when full the oldest entry is dropped. Return false on invalid input / not found.
int live_wifi_find(const live_config_t *cfg, const char *ssid);
bool live_wifi_upsert(live_config_t *cfg, const char *ssid, const char *pass);
bool live_wifi_remove(live_config_t *cfg, const char *ssid);
// Moves a saved network to the front (tried first when its signal is usable).
bool live_wifi_prefer(live_config_t *cfg, const char *ssid);

// Chooses which saved network to join from a scan, skipping tried_mask (bit i =
// cfg->wifi[i] already failed this round). Saved order is the priority: the first
// saved network visible at LIVE_WIFI_USABLE_RSSI or better wins (home before the
// phone hotspot, even when the phone sits right next to the device); if none is that
// strong, the strongest visible saved network is used. Returns an index or -1.
#define LIVE_WIFI_USABLE_RSSI (-80)
typedef struct {
    const char *ssid;
    int rssi;
} live_scan_ap_t;
int live_wifi_pick(const live_config_t *cfg, const live_scan_ap_t *aps, int n, unsigned tried_mask);

// "sk-abcd…wxyz"-style mask: first 7 and last 4 characters only.
void live_config_mask_key(const char *key, char *out, size_t out_cap);
