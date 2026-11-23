// main/app_wifi.c -- Wi-Fi station with up to 4 saved networks (NVS).
// A manager task scans and joins the strongest visible saved network, falling back
// to the next one (e.g. home IoT SSID, else phone hotspot), with capped backoff.
#include "app.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_wifi";

#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_events;
static TimerHandle_t s_retry_timer;   // kept for reassociate(); the manager task owns connecting
static TaskHandle_t s_mgr;
static int s_backoff_ms = 1000;
static bool s_started;
static volatile bool s_connecting;
static volatile bool s_disconnected;
static volatile bool s_paused;   // setup portal: never scan/join (the AP must stay up)
static volatile int s_last_reason;
static live_wifi_net_t s_pending;           // from the setup page, not yet verified
static volatile bool s_has_pending;
static char s_setup_error[96];
static volatile int s_failed_rounds;      // consecutive scan+join rounds without success

#define BIT_KICK BIT1

// Joins saved network `i` and waits for an IP (or a disconnect) up to 15 s.
static bool try_join(const live_wifi_net_t *net)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, net->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, net->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = net->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    ESP_LOGI(TAG, "连接 Wi-Fi：%s", net->ssid);
    s_disconnected = false;
    s_connecting = true;
    esp_wifi_connect();
    for (int t = 0; t < 150; t++) {   // 15 s
        if (xEventGroupGetBits(s_events) & BIT_CONNECTED) break;
        if (s_disconnected) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_connecting = false;
    return xEventGroupGetBits(s_events) & BIT_CONNECTED;
}

// Scan, then try saved networks from the strongest visible one down; repeat with
// backoff while nothing works. Covers "home IoT SSID or phone hotspot".
static void wifi_mgr_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_events, BIT_KICK, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(app_wifi_connected() ? portMAX_DELAY : s_backoff_ms));
        if (app_wifi_connected() || s_paused) continue;
        if (s_has_pending) {
            // Verify the setup-page network before it replaces anything.
            esp_wifi_disconnect();
            bool ok = false;
            for (int i = 0; i < 2 && !ok; i++) ok = try_join(&s_pending);   // one retry
            if (ok) {
                const esp_err_t err = app_config_set_wifi_first(s_pending.ssid, s_pending.pass);
                if (err == ESP_OK) app_config_clear_pending_wifi();   // only once saved for good
                ESP_LOGI(TAG, "新 Wi-Fi %s 验证成功，%s（栈余量 %u B）", s_pending.ssid,
                         err == ESP_OK ? "已保存" : "保存失败，下次开机重试",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            } else {
                app_config_clear_pending_wifi();   // definitely wrong: drop it, keep the old list
                const int r = s_last_reason;
                snprintf(s_setup_error, sizeof(s_setup_error),
                         r == WIFI_REASON_NO_AP_FOUND ? "没找到 Wi-Fi「%s」（需 2.4 GHz）" : "连不上「%s」，密码可能不对",
                         s_pending.ssid);
                ESP_LOGW(TAG, "新 Wi-Fi %s 验证失败 reason=%d，回到设置", s_pending.ssid, r);
                app_post(APP_CMD_SETUP);
            }
            memset(&s_pending, 0, sizeof(s_pending));
            s_has_pending = false;
            if (!ok) continue;
        }
        static live_config_t cfg;   // ~1.3 KB: static, this task only (keeps the stack small)
        app_config_get(&cfg);
        if (cfg.wifi_count == 0) continue;

        esp_wifi_disconnect();
        wifi_scan_config_t sc = { .show_hidden = true };
        uint16_t n = 0;
        wifi_ap_record_t *recs = NULL;
        if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_num(&n) == ESP_OK && n) {
            if (n > 24) n = 24;
            recs = calloc(n, sizeof(*recs));
            if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
                n = 0;
                esp_wifi_clear_ap_list();   // otherwise the driver keeps the list allocated
            }
        }
        live_scan_ap_t aps[24];
        for (int i = 0; i < n && recs; i++) {
            aps[i].ssid = (const char *)recs[i].ssid;
            aps[i].rssi = recs[i].rssi;
        }
        if (!recs) n = 0;
        unsigned tried = 0;
        bool ok = false;
        for (int round = 0; round < LIVE_CFG_WIFI_MAX && !ok; round++) {
            int pick = live_wifi_pick(&cfg, aps, n, tried);
            if (pick < 0) break;
            tried |= 1u << pick;
            ok = try_join(&cfg.wifi[pick]);
            if (!ok) ESP_LOGW(TAG, "%s 连接失败，换下一个", cfg.wifi[pick].ssid);
        }
        free(recs);
        if (!ok) {
            s_failed_rounds++;
            ESP_LOGW(TAG, "附近没有可用的已保存 Wi-Fi（共 %d 个），%d ms 后重新扫描", cfg.wifi_count, s_backoff_ms);
            s_backoff_ms = s_backoff_ms * 2 > 15000 ? 15000 : s_backoff_ms * 2;
        } else {
            s_backoff_ms = 1000;
            s_failed_rounds = 0;
        }
    }
}

static void kick(void)
{
    if (s_events) xEventGroupSetBits(s_events, BIT_KICK);
}

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    kick();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        kick();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        s_disconnected = true;
        s_last_reason = d ? d->reason : -1;
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%d", s_last_reason);
        if (!s_connecting && !s_paused) kick();   // the manager rescans and picks a network
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        wifi_ap_record_t ap;
        const char *ssid = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? (const char *)ap.ssid : "?";
        ESP_LOGI(TAG, "已联网 %s IP=" IPSTR, ssid, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t app_wifi_start(void)
{
    if (s_started) return ESP_OK;
    s_events = xEventGroupCreate();
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_cb);
    if (!s_events || !s_retry_timer) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // credentials come from our NVS
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_has_pending = app_config_get_pending_wifi(&s_pending);
    // 6 KB: joining + NVS writes + logging measured ~4.3 KB at peak on the first save.
    if (xTaskCreate(wifi_mgr_task, "wifi_mgr", 6144, NULL, 4, &s_mgr) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_wifi_start());
    // The device sits near its AP (RSSI around -30 dBm). Full 20 dBm TX bursts on battery
    // can pull the supply below the brownout threshold right after association (seen as
    // a reboot loop every ~7 s after power-on); 15 dBm halves the peak RF power.
    esp_wifi_set_max_tx_power(60);   // unit 0.25 dBm
    s_started = true;
    return ESP_OK;
}

int app_wifi_failed_rounds(void)
{
    return s_failed_rounds;
}

bool app_wifi_pending(void)
{
    return s_has_pending;
}

const char *app_wifi_setup_error(void)
{
    return s_setup_error;
}

void app_wifi_pause(void)
{
    s_paused = true;
    xTimerStop(s_retry_timer, 0);
    esp_wifi_disconnect();
}

void app_wifi_reconfigure(void)
{
    if (!s_started) return;
    s_backoff_ms = 1000;
    esp_wifi_disconnect();   // DISCONNECTED -> manager rescans with the new list
    kick();
}

bool app_wifi_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
}

bool app_wifi_wait(int timeout_ms)
{
    if (!s_events) return false;
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) &
           BIT_CONNECTED;
}

int app_net_tcp_check(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    const int64_t t0 = esp_timer_get_time();
    connect(s, res->ai_addr, res->ai_addrlen);
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int sel = select(s + 1, NULL, &wf, NULL, &tv);
    int err = -1;
    socklen_t len = sizeof(err);
    if (sel > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
    close(s);
    freeaddrinfo(res);
    return (sel > 0 && err == 0) ? (int)((esp_timer_get_time() - t0) / 1000) : -1;
}

// Some APs (seen on a Wi-Fi 7 router) keep a broken station context after the device
// resets without a clean disassociation: DHCP/DNS still work but every TCP SYN is
// dropped. A clean disconnect + reconnect clears it (verified on the device).
static bool reassociate(void)
{
    ESP_LOGW(TAG, "TCP 不通，干净地断开并重新关联 Wi-Fi");
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    s_backoff_ms = 1000;
    esp_wifi_disconnect();   // the DISCONNECTED handler kicks the manager to reconnect
    return app_wifi_wait(20000);
}

bool app_net_ensure(int attempts)
{
    // Serialized: a second caller waits for the running check and
    // reuses a recent pass instead of probing while a reassociation is in progress.
    static SemaphoreHandle_t s_check_lock;
    static int64_t s_verified_ms;
    if (!s_check_lock) s_check_lock = xSemaphoreCreateMutex();
    if (!s_check_lock || xSemaphoreTake(s_check_lock, pdMS_TO_TICKS(60000)) != pdTRUE) return false;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_verified_ms && now_ms - s_verified_ms < 10000 && app_wifi_connected()) {
        xSemaphoreGive(s_check_lock);
        return true;
    }
    bool ok = false;
    live_config_t cfg;
    app_config_get(&cfg);
    char host[LIVE_CFG_HOST_MAX + 1];
    strlcpy(host, cfg.host, sizeof(host));
    char *colon = strchr(host, ':');
    const char *port = "443";
    if (colon) {
        *colon = '\0';
        port = colon + 1;
    }
    for (int i = 0; i < attempts; i++) {
        if (!app_wifi_wait(15000)) continue;
        int ms = app_net_tcp_check(host, port, 3000);
        if (ms >= 0) {
            ESP_LOGI(TAG, "网络自检通过：TCP %s:%s %d ms", host, port, ms);
            ok = true;
            break;
        }
        if (i + 1 < attempts) reassociate();
    }
    if (ok) {
        s_verified_ms = esp_timer_get_time() / 1000;
    } else {
        s_verified_ms = 0;
        ESP_LOGE(TAG, "网络自检失败（%d 次）", attempts);
    }
    xSemaphoreGive(s_check_lock);
    return ok;
}
