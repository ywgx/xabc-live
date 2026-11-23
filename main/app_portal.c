// main/app_portal.c -- first-time setup from a phone, no computer or app needed.
//
// The device opens an open hotspot "XABC-Live-XXXX" and shows its Wi-Fi QR code on the
// screen. Phones that join get the setup page automatically (captive portal: every DNS
// name resolves to the device, every unknown URL redirects to it). Step 1 picks a
// Wi-Fi network from a scan and takes its password, step 2 takes the XSI Router API key
// (the API host stays the default api.xsirouter.com). Saving writes NVS and restarts.
#include "app.h"
#include "live_config.h"
#include "live_form.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_portal";

#define PORTAL_IP      "192.168.4.1"
#define SCAN_MAX       12
#define PAGE_CAP       6144
#define BODY_MAX       1024

static volatile bool s_active;
static char s_ssid[33];
static char s_nets[SCAN_MAX][33];
static int s_net_count;
static httpd_handle_t s_httpd;
static char s_error[96];          // why the last attempt failed (shown on the page)

bool app_portal_active(void)
{
    return s_active;
}

const char *app_portal_ssid(void)
{
    return s_active ? s_ssid : "";
}

// ---------------------------------------------------------------------------
// Web page
// ---------------------------------------------------------------------------

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=zh><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>XABC Live 设置</title>"
    "<style>body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;margin:24px auto;"
    "padding:0 16px;background:#0b0f14;color:#e6edf3}h1{font-size:22px;margin-bottom:4px}"
    "label{display:block;margin:16px 0 6px;color:#9aa4ae}"
    "select,input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #30363d;"
    "background:#161b22;color:#e6edf3;font-size:16px}"
    "button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:10px;background:#22c55e;"
    "color:#04120a;font-size:17px;font-weight:600}.hint{color:#9aa4ae;font-size:13px}#s2{display:none}"
    "</style><h1>XABC Live 设置</h1>";

static const char PAGE_FORM_1[] =
    "<form method=post action=/save><div id=s1>"
    "<p class=hint>第 1 步，共 2 步：连接 Wi-Fi（仅支持 2.4 GHz）</p>"
    "<label>Wi-Fi</label><select id=sel name=ssid>";

static const char PAGE_FORM_2[] =
    "<option value=''>其他网络（手动输入）</option></select>"
    "<input id=other name=ssid2 placeholder='Wi-Fi 名称' style='display:none;margin-top:8px'>"
    "<label>密码</label><input name=pass type=text placeholder='开放网络留空' autocomplete=off "
    "autocapitalize=off autocorrect=off spellcheck=false>"
    "<p class=hint>注意大小写。中文标点会自动转成英文半角，首尾空格会自动去掉。</p>"
    "<button type=button onclick=next()>下一步</button></div>"
    "<div id=s2><p class=hint>第 2 步，共 2 步：XSI Router API Key</p>"
    "<label>API Key</label><input name=key placeholder='sk-...' autocomplete=off "
    "autocapitalize=off spellcheck=false><p class=hint>在 xsirouter.com 控制台获取。";

static const char PAGE_FORM_3[] =
    "</p><button>保存并重启</button></div></form>"
    "<script>var s=document.getElementById('sel'),o=document.getElementById('other');"
    "s.onchange=function(){o.style.display=s.value?'none':'block'};s.onchange();"
    "function next(){if(!s.value&&!o.value){alert('请选择或输入 Wi-Fi');return}"
    "document.getElementById('s1').style.display='none';document.getElementById('s2').style.display='block'}"
    "</script>";

static bool put(char *page, size_t *len, const char *s)
{
    const size_t n = strlen(s);
    if (*len + n + 1 > PAGE_CAP) return false;
    memcpy(page + *len, s, n + 1);
    *len += n;
    return true;
}

static esp_err_t send_page(httpd_req_t *req, const char *body_html)
{
    char *page = malloc(PAGE_CAP);
    if (!page) return httpd_resp_send_500(req);
    size_t len = 0;
    page[0] = '\0';
    put(page, &len, PAGE_HEAD);
    put(page, &len, body_html);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, (ssize_t)len);
    free(page);
    return err;
}

static esp_err_t on_form(httpd_req_t *req)
{
    char *page = malloc(PAGE_CAP);
    if (!page) return httpd_resp_send_500(req);
    size_t len = 0;
    page[0] = '\0';
    put(page, &len, PAGE_HEAD);
    if (s_error[0]) {
        put(page, &len, "<p style='color:#f87171'>上次设置失败：");
        live_html_append(page, PAGE_CAP, &len, s_error);
        put(page, &len, "。请重新填写。</p>");
    }
    put(page, &len, PAGE_FORM_1);
    for (int i = 0; i < s_net_count; i++) {
        put(page, &len, "<option value='");
        live_html_append(page, PAGE_CAP, &len, s_nets[i]);
        put(page, &len, "'>");
        live_html_append(page, PAGE_CAP, &len, s_nets[i]);
        put(page, &len, "</option>");
    }
    put(page, &len, PAGE_FORM_2);
    live_config_t cfg;
    app_config_get(&cfg);
    if (live_config_valid_api_key(cfg.api_key)) put(page, &len, "设备已保存 Key，留空则保持不变。");
    put(page, &len, PAGE_FORM_3);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, (ssize_t)len);
    free(page);
    return err;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the success page reach the phone
    esp_restart();
}

static esp_err_t on_save(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    char *body = calloc(1, BODY_MAX + 1);
    if (!body) return httpd_resp_send_500(req);
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    char ssid[LIVE_CFG_SSID_MAX + 1] = "";
    char pass[LIVE_CFG_WIFI_PASS_MAX + 1] = "";
    char key[LIVE_CFG_KEY_MAX + 1] = "";
    if (!live_form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        live_form_get(body, "ssid2", ssid, sizeof(ssid));
    }
    live_form_get(body, "pass", pass, sizeof(pass));
    live_form_get(body, "key", key, sizeof(key));
    free(body);
    // Forgive phone keyboards: trim everywhere; password and key become plain ASCII
    // (full-width / Chinese punctuation -> half-width); keys never contain spaces.
    // SSIDs keep their characters: many router names really are Chinese/full-width.
    live_text_trim(ssid);
    live_text_to_ascii(pass);
    live_text_to_ascii(key);
    live_text_strip_spaces(key);

    live_config_t cfg;
    app_config_get(&cfg);
    const char *error = NULL;
    if (!live_config_valid_ssid(ssid)) {
        error = "Wi-Fi 名称无效";
    } else if (!live_config_valid_wifi_pass(pass)) {
        error = "Wi-Fi 密码需要 8–63 个字符（开放网络留空）";
    } else if (!key[0] && !live_config_valid_api_key(cfg.api_key)) {
        error = "请填写 API Key";
    } else if (key[0] && !live_config_valid_api_key(key)) {
        error = "API Key 格式不对（应为 sk- 开头的一整串）";
    }
    if (!error && (app_config_set_pending_wifi(ssid, pass) != ESP_OK ||
                   (key[0] && app_config_set_key(key) != ESP_OK))) {
        error = "保存失败，请重试";
    }
    char msg[512];
    if (error) {
        snprintf(msg, sizeof(msg), "<p>%s</p><p><a href='/' style='color:#22c55e'>返回重新填写</a></p>", error);
        return send_page(req, msg);
    }
    char esc[80];
    size_t el = 0;
    esc[0] = '\0';
    live_html_append(esc, sizeof(esc), &el, ssid);
    snprintf(msg, sizeof(msg), "<p>已保存 ✔</p><p class=hint>设备正在重启并连接「%s」，成功后自动进入对话；"
             "如果连不上，屏幕会重新显示二维码。可以关闭此页面。</p>", esc);
    ESP_LOGI(TAG, "设置已保存（Wi-Fi %s%s），重启", ssid, key[0] ? "，新 Key" : "");
    esp_err_t err = send_page(req, msg);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return err;
}

// Everything else (OS connectivity checks like /generate_204, /hotspot-detect.html):
// redirect to the form, which makes phones pop the page up by themselves.
static esp_err_t on_any(httpd_req_t *req)
{
    if (strcmp(req->uri, "/") == 0) return on_form(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// Captive DNS: every name -> 192.168.4.1
// ---------------------------------------------------------------------------

static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "DNS 端口绑定失败");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
    }
    static const unsigned char ip[4] = { 192, 168, 4, 1 };
    static unsigned char q[512], r[528];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        size_t rl = live_dns_answer(q, (size_t)n, ip, r, sizeof(r));
        if (rl) sendto(s, r, rl, 0, (struct sockaddr *)&from, fl);
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

static void scan_networks(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    uint16_t n = 0;
    s_net_count = 0;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_num(&n) != ESP_OK || n == 0) return;
    if (n > 24) n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        esp_wifi_clear_ap_list();
        return;
    }
    for (int i = 0; i < n && s_net_count < SCAN_MAX; i++) {   // strongest first, no duplicates
        const char *name = (const char *)recs[i].ssid;
        if (!name[0]) continue;
        bool dup = false;
        for (int k = 0; k < s_net_count && !dup; k++) dup = strcmp(s_nets[k], name) == 0;
        if (!dup) strlcpy(s_nets[s_net_count++], name, sizeof(s_nets[0]));
    }
    free(recs);
}

esp_err_t app_portal_start(const char *error)
{
    strlcpy(s_error, error ? error : "", sizeof(s_error));
    if (s_active) return ESP_OK;
    app_set_state(APP_ST_SETUP, "正在搜索附近的 Wi-Fi…");
    app_wifi_pause();
    scan_networks();

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "XABC-Live-%02X%02X", mac[4], mac[5]);
    static bool s_netif_done;
    if (!s_netif_done) {
        esp_netif_create_default_wifi_ap();
        s_netif_done = true;
    }
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ssid);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;   // shown as a QR code; setup ends with a restart
    ap.ap.max_connection = 2;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(err));
        app_set_state(APP_ST_ERROR, "设置热点启动失败");
        return err;
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_uri_handlers = 4;
    hc.stack_size = 6144;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        app_set_state(APP_ST_ERROR, "设置网页启动失败");
        return ESP_FAIL;
    }
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = on_save };
    const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = on_any };
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &any);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);

    s_active = true;
    char status[64];
    if (s_error[0]) {
        snprintf(status, sizeof(status), "设置失败，请重新扫码");
    } else {
        snprintf(status, sizeof(status), "扫码连接 %s", s_ssid);
    }
    app_set_state(APP_ST_SETUP, status);
    ESP_LOGI(TAG, "设置热点 %s 已开启（%d 个附近网络），访问 http://" PORTAL_IP "/", s_ssid, s_net_count);
    return ESP_OK;
}
