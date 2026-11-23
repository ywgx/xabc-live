// main/app_config.c -- settings stored in NVS namespace "gptlive".
//
// Secrets (Wi-Fi password, API key) live only in NVS on the device. They are
// written over the USB console (app_console.c) and never compiled into firmware.
// Writing the merged full.bin at 0x0 erases them; re-run tools/live_provision.py.
#include "app.h"

#include "esp_system.h"
#include "live_policy.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_config";
#define NS "gptlive"

static live_config_t s_cfg;
static int s_volume = LIVE_VOLUME_DEFAULT;
static bool s_autostart = true;
static SemaphoreHandle_t s_lock;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) return;   // keep default
}

esp_err_t app_config_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除重建: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    live_config_defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t count = 0;
        if (nvs_get_u8(h, "wcnt", &count) == ESP_OK && count <= LIVE_CFG_WIFI_MAX) {
            for (int i = 0; i < count; i++) {
                char k1[8], k2[8];
                snprintf(k1, sizeof(k1), "ws%d", i);
                snprintf(k2, sizeof(k2), "wp%d", i);
                load_str(h, k1, s_cfg.wifi[i].ssid, sizeof(s_cfg.wifi[i].ssid));
                load_str(h, k2, s_cfg.wifi[i].pass, sizeof(s_cfg.wifi[i].pass));
                if (s_cfg.wifi[i].ssid[0]) s_cfg.wifi_count = i + 1;
            }
        } else {
            // Migrate the single-network keys used by earlier firmware.
            char ssid[LIVE_CFG_SSID_MAX + 1] = "", pass[LIVE_CFG_WIFI_PASS_MAX + 1] = "";
            load_str(h, "ssid", ssid, sizeof(ssid));
            load_str(h, "wpass", pass, sizeof(pass));
            if (ssid[0]) live_wifi_upsert(&s_cfg, ssid, pass);
        }
        load_str(h, "key", s_cfg.api_key, sizeof(s_cfg.api_key));
        load_str(h, "host", s_cfg.host, sizeof(s_cfg.host));
        load_str(h, "voice", s_cfg.voice, sizeof(s_cfg.voice));
        load_str(h, "prompt", s_cfg.prompt, sizeof(s_cfg.prompt));
        uint8_t v;
        if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= LIVE_VOLUME_MAX) s_volume = v;
        if (nvs_get_u8(h, "auto", &v) == ESP_OK) s_autostart = v != 0;
        nvs_close(h);
    }
    char masked[32];
    live_config_mask_key(s_cfg.api_key, masked, sizeof(masked));
    char nets[LIVE_CFG_WIFI_MAX * (LIVE_CFG_SSID_MAX + 2) + 1] = "";
    for (int i = 0; i < s_cfg.wifi_count; i++) {
        strlcat(nets, i ? "," : "", sizeof(nets));
        strlcat(nets, s_cfg.wifi[i].ssid, sizeof(nets));
    }
    ESP_LOGI(TAG, "配置: wifi=[%s] key=%s host=%s voice=%s volume=%d",
             nets[0] ? nets : "(未设置)", masked, s_cfg.host, s_cfg.voice, s_volume);
    return ESP_OK;
}

void app_config_get(live_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

static esp_err_t save_str(const char *key, const char *value, char *field, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(field, value, cap);
        xSemaphoreGive(s_lock);
    }
    return err;
}

// Writes the whole network list (small: at most 4 entries) and drops legacy keys.
static esp_err_t save_wifi_list(const live_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (int i = 0; i < LIVE_CFG_WIFI_MAX && err == ESP_OK; i++) {
        char k1[8], k2[8];
        snprintf(k1, sizeof(k1), "ws%d", i);
        snprintf(k2, sizeof(k2), "wp%d", i);
        if (i < cfg->wifi_count) {
            err = nvs_set_str(h, k1, cfg->wifi[i].ssid);
            if (err == ESP_OK) err = nvs_set_str(h, k2, cfg->wifi[i].pass);
        } else {
            nvs_erase_key(h, k1);
            nvs_erase_key(h, k2);
        }
    }
    if (err == ESP_OK) err = nvs_set_u8(h, "wcnt", (uint8_t)cfg->wifi_count);
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "wpass");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t update_wifi(bool add, const char *ssid, const char *pass, bool first)
{
    // live_config_t is ~1.3 KB: keep the working copy off the caller's stack (the Wi-Fi
    // manager saves from a small task and once overflowed it).
    live_config_t *next = malloc(sizeof(*next));
    if (!next) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *next = s_cfg;
    xSemaphoreGive(s_lock);
    esp_err_t err;
    bool ok = add ? live_wifi_upsert(next, ssid, pass) : live_wifi_remove(next, ssid);
    if (ok && add && first) live_wifi_prefer(next, ssid);
    if (!ok) {
        err = add ? ESP_ERR_INVALID_ARG : ESP_ERR_NOT_FOUND;
    } else {
        err = save_wifi_list(next);
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(s_cfg.wifi, next->wifi, sizeof(s_cfg.wifi));
            s_cfg.wifi_count = next->wifi_count;
            xSemaphoreGive(s_lock);
        }
    }
    free(next);
    return err;
}

esp_err_t app_config_set_wifi(const char *ssid, const char *pass)
{
    return update_wifi(true, ssid, pass, false);
}

esp_err_t app_config_set_wifi_first(const char *ssid, const char *pass)
{
    return update_wifi(true, ssid, pass, true);
}

// A network entered on the setup page is kept apart until the device has actually
// joined it, so a mistyped password never replaces a working one.
esp_err_t app_config_set_pending_wifi(const char *ssid, const char *pass)
{
    if (!live_config_valid_ssid(ssid) || !live_config_valid_wifi_pass(pass)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "pssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "ppass", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool app_config_get_pending_wifi(live_wifi_net_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t a = sizeof(out->ssid), b = sizeof(out->pass);
    const bool ok = nvs_get_str(h, "pssid", out->ssid, &a) == ESP_OK && nvs_get_str(h, "ppass", out->pass, &b) == ESP_OK;
    nvs_close(h);
    return ok;
}

// Called only once the pending network is saved for good, or definitely failed: a crash
// or power cut in between keeps it, and the next boot verifies it again.
void app_config_clear_pending_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "pssid");
    nvs_erase_key(h, "ppass");
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t app_config_del_wifi(const char *ssid)
{
    return update_wifi(false, ssid, NULL, false);
}

esp_err_t app_config_set_key(const char *key)
{
    if (!live_config_valid_api_key(key)) return ESP_ERR_INVALID_ARG;
    return save_str("key", key, s_cfg.api_key, sizeof(s_cfg.api_key));
}

esp_err_t app_config_set_host(const char *host)
{
    if (!live_config_valid_host(host)) return ESP_ERR_INVALID_ARG;
    return save_str("host", host, s_cfg.host, sizeof(s_cfg.host));
}

esp_err_t app_config_set_voice(const char *voice)
{
    if (!live_config_valid_voice(voice)) return ESP_ERR_INVALID_ARG;
    return save_str("voice", voice, s_cfg.voice, sizeof(s_cfg.voice));
}

esp_err_t app_config_set_prompt(const char *prompt)
{
    if (!live_config_valid_prompt(prompt)) return ESP_ERR_INVALID_ARG;
    return save_str("prompt", prompt, s_cfg.prompt, sizeof(s_cfg.prompt));
}

esp_err_t app_config_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        live_config_defaults(&s_cfg);
        xSemaphoreGive(s_lock);
        s_volume = LIVE_VOLUME_DEFAULT;
    }
    return err;
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EXT:       return "ext";
    default:                return "other";
    }
}

static char s_resets[96];
static char s_last_wdt[120];

const char *app_config_last_wdt(void)
{
    return s_last_wdt;
}

void app_config_note_boot(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    char prev[sizeof(s_resets)] = "";
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof(prev);
    if (nvs_get_str(h, "rsts", prev, &n) != ESP_OK) prev[0] = '\0';
    // Newest first, at most 8 entries: "brownout,brownout,poweron,...".
    snprintf(s_resets, sizeof(s_resets), "%s%s%.80s", reset_name(r), prev[0] ? "," : "", prev);
    int commas = 0;
    for (char *p = s_resets; *p; p++) {
        if (*p == ',' && ++commas == 8) {
            *p = '\0';
            break;
        }
    }
    nvs_set_str(h, "rsts", s_resets);
    char wdt[120];
    if (app_diag_take(wdt, sizeof(wdt))) {
        nvs_set_str(h, "wdt", wdt);
        strlcpy(s_last_wdt, wdt, sizeof(s_last_wdt));
        ESP_LOGW(TAG, "上次看门狗：%s", wdt);
    } else {
        size_t m = sizeof(s_last_wdt);
        if (nvs_get_str(h, "wdt", s_last_wdt, &m) != ESP_OK) s_last_wdt[0] = '\0';
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "重启原因：%s（最近：%s）", reset_name(r), s_resets);
}

const char *app_config_reset_history(void)
{
    return s_resets;
}

bool app_auto_start_enabled(void)
{
    return s_autostart;
}

esp_err_t app_config_set_autostart(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "auto", on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_autostart = on;
    return err;
}

int app_config_volume(void)
{
    return s_volume;
}

esp_err_t app_config_save_volume(int volume)
{
    if (volume < 0 || volume > LIVE_VOLUME_MAX) return ESP_ERR_INVALID_ARG;
    if (volume == s_volume) return ESP_OK;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "volume", (uint8_t)volume);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_volume = volume;
    return err;
}
