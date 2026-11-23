// main/app_console.c -- USB Serial/JTAG provisioning console.
//
// Commands (quotes allowed, see tools/live_provision.py):
//   wifi <ssid> [password]   (add/update; up to 4 networks)   wifi_del <ssid>   wifi_list
//   key <api-key>            host <host[:port]>
//   voice <name>             prompt "<text>"      config
//   clear                    restart              poweroff             nettest
//   debug on|off             per-session stats and transcripts in the log (not persisted)
//   autostart on|off
//   talk                     same as OK click (start / interrupt)   hangup  same as OK long press
//   testmic on|off  testplay <url>  volume <0..150>   (automated testing, not persisted)
// Every command prints a final "OK <cmd>" or "ERR <cmd>: <reason>" line so a
// script can wait for completion. Secrets are never echoed back by `config`.
#include "app.h"
#include "live_policy.h"
#include "live_proto.h"
#include <stdlib.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_console";

static int report(const char *cmd, esp_err_t err)
{
    if (err == ESP_OK) {
        printf("OK %s\n", cmd);
        return 0;
    }
    printf("ERR %s: %s\n", cmd, err == ESP_ERR_INVALID_ARG ? "invalid value" : esp_err_to_name(err));
    return 1;
}

// Drop the line from linenoise history so a secret does not linger in RAM.
static void forget_line(void)
{
    linenoiseHistoryFree();
}

static int cmd_wifi(int argc, char **argv)
{
    forget_line();
    if (argc < 2 || argc > 3) {
        printf("ERR wifi: usage wifi <ssid> [password]\n");
        return 1;
    }
    esp_err_t err = app_config_set_wifi(argv[1], argc == 3 ? argv[2] : "");
    if (err == ESP_OK) app_post(APP_CMD_CONFIG_CHANGED);
    return report("wifi", err);
}

static int cmd_wifi_del(int argc, char **argv)
{
    if (argc != 2) {
        printf("ERR wifi_del: usage wifi_del <ssid>\n");
        return 1;
    }
    esp_err_t err = app_config_del_wifi(argv[1]);
    if (err == ESP_OK) app_post(APP_CMD_CONFIG_CHANGED);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("ERR wifi_del: not saved\n");
        return 1;
    }
    return report("wifi_del", err);
}

static int cmd_wifi_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    live_config_t cfg;
    app_config_get(&cfg);
    for (int i = 0; i < cfg.wifi_count; i++) {
        printf("%d: %s (%s)\n", i + 1, cfg.wifi[i].ssid, cfg.wifi[i].pass[0] ? "****" : "open");
    }
    printf("OK wifi_list\n");
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    forget_line();
    if (argc != 2) {
        printf("ERR key: usage key <api-key>\n");
        return 1;
    }
    esp_err_t err = app_config_set_key(argv[1]);
    if (err == ESP_OK) app_post(APP_CMD_CONFIG_CHANGED);
    return report("key", err);
}

static int cmd_host(int argc, char **argv)
{
    if (argc != 2) {
        printf("ERR host: usage host <host[:port]>\n");
        return 1;
    }
    return report("host", app_config_set_host(argv[1]));
}

static int cmd_voice(int argc, char **argv)
{
    if (argc != 2) {
        printf("ERR voice: usage voice <name>\n");
        return 1;
    }
    return report("voice", app_config_set_voice(argv[1]));
}

static int cmd_prompt(int argc, char **argv)
{
    if (argc != 2) {
        printf("ERR prompt: usage prompt \"<text>\"\n");
        return 1;
    }
    return report("prompt", app_config_set_prompt(argv[1]));
}

static int cmd_talk(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_post(APP_CMD_TOGGLE);
    printf("OK talk\n");
    return 0;
}

static int cmd_hangup(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_post(APP_CMD_STOP);
    printf("OK hangup\n");
    return 0;
}

static int cmd_testmic(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("ERR testmic: usage testmic on|off\n");
        return 1;
    }
    app_audio_test_mic(strcmp(argv[1], "on") == 0);
    printf("OK testmic\n");
    return 0;
}

static int cmd_testplay(int argc, char **argv)
{
    if (argc != 2 || strncmp(argv[1], "http://", 7) != 0) {
        printf("ERR testplay: usage testplay http://host:port/file.ulaw\n");
        return 1;
    }
    return report("testplay", app_testplay_start(argv[1]));
}

static int cmd_volume(int argc, char **argv)
{
    int v = argc == 2 ? atoi(argv[1]) : -1;
    if (v < 0 || v > LIVE_VOLUME_MAX) {
        printf("ERR volume: usage volume <0..%d>\n", LIVE_VOLUME_MAX);
        return 1;
    }
    app_audio_set_volume(v);   // runtime only; buttons and NVS keep their own value
    printf("OK volume\n");
    return 0;
}

static int cmd_autostart(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("ERR autostart: usage autostart on|off\n");
        return 1;
    }
    return report("autostart", app_config_set_autostart(strcmp(argv[1], "on") == 0));
}

static int cmd_debug(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("ERR debug: usage debug on|off\n");
        return 1;
    }
    g_debug = strcmp(argv[1], "on") == 0;
    printf("OK debug\n");
    return 0;
}

static int cmd_setup(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("OK setup (hotspot + setup page; OK key or `restart` leaves)\n");
    app_post(APP_CMD_SETUP);
    return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("OK poweroff (deep sleep; any key wakes)\n");
    app_post(APP_CMD_POWER_OFF);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    live_config_t cfg;
    app_config_get(&cfg);
    char masked[32];
    live_config_mask_key(cfg.api_key, masked, sizeof(masked));
    char nets[LIVE_CFG_WIFI_MAX * (LIVE_CFG_SSID_MAX + 2) + 1] = "";
    for (int i = 0; i < cfg.wifi_count; i++) {
        strlcat(nets, i ? "," : "", sizeof(nets));
        strlcat(nets, cfg.wifi[i].ssid, sizeof(nets));
    }
    printf("autostart=%s\n", app_auto_start_enabled() ? "on" : "off");
    printf("resets=%s\n", app_config_reset_history());
    printf("last_wdt=%s\n", app_config_last_wdt());
    printf("ssid=%s\nkey=%s\nhost=%s\nvoice=%s\nvolume=%d\nwifi=%s\nprompt=%s\n",
           nets[0] ? nets : "(未设置)", masked, cfg.host,
           cfg.voice, app_config_volume(),
           app_wifi_connected() ? "connected" : "offline",
           cfg.prompt);
    printf("OK config\n");
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = app_config_clear();
    if (err == ESP_OK) app_post(APP_CMD_CONFIG_CHANGED);
    return report("clear", err);
}

// Read-only network diagnosis: DNS + plain TCP connect timing (no TLS, no data sent).
static void tcp_probe(const char *host, const char *port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int64_t t0 = esp_timer_get_time();
    int rc = getaddrinfo(host, port, &hints, &res);
    int dns_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (rc != 0 || !res) {
        printf("%s: DNS failed rc=%d (%d ms)\n", host, rc, dns_ms);
        return;
    }
    char ip[16];
    inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof(ip));
    int s = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    t0 = esp_timer_get_time();
    connect(s, res->ai_addr, res->ai_addrlen);
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv = { .tv_sec = 20 };
    int sel = select(s + 1, NULL, &wf, NULL, &tv);
    int err = 0;
    socklen_t len = sizeof(err);
    if (sel > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    struct sockaddr_in local = { 0 };
    socklen_t llen = sizeof(local);
    getsockname(s, (struct sockaddr *)&local, &llen);
    printf("%s (%s): DNS %d ms, TCP %s in %d ms%s, local port %u\n", host, ip, dns_ms,
           sel > 0 && err == 0 ? "connected" : "FAILED", ms, sel == 0 ? " (timeout)" : "",
           (unsigned)ntohs(local.sin_port));
    close(s);
    freeaddrinfo(res);
}

static int cmd_nettest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("Wi-Fi: ssid=%s channel=%d rssi=%d dBm\n", (const char *)ap.ssid, ap.primary, ap.rssi);
    } else {
        printf("Wi-Fi: not connected\n");
    }
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
    for (int i = 0; i < 3; i++) tcp_probe(host, port);
    tcp_probe("www.baidu.com", "443");
    printf("OK nettest\n");
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("OK restart\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

#define CONSOLE_LINE_MAX 2048   // prompt text can be ~1.8 KB of UTF-8

static void console_task(void *arg)
{
    (void)arg;
    setvbuf(stdin, NULL, _IONBF, 0);
    linenoiseSetDumbMode(1);   // driven by scripts; no escape-sequence probing
    linenoiseSetMaxLineLen(CONSOLE_LINE_MAX);
    linenoiseHistorySetMaxLen(1);
    for (;;) {
        if (!usb_serial_jtag_is_connected()) {   // on battery: nobody to talk to
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        char *line = linenoise("passport> ");
        if (!line) {   // no input or a read error: never spin
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int ret;
        const esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) printf("Unrecognized command\n");
        linenoiseFree(line);
    }
}

esp_err_t app_console_start(void)
{

    static const esp_console_cmd_t cmds[] = {
        { .command = "wifi", .help = "wifi <ssid> [password] (add/update)", .func = cmd_wifi },
        { .command = "wifi_del", .help = "wifi_del <ssid>", .func = cmd_wifi_del },
        { .command = "wifi_list", .help = "list saved Wi-Fi networks", .func = cmd_wifi_list },
        { .command = "key", .help = "key <api-key>", .func = cmd_key },
        { .command = "host", .help = "host <host[:port]> (default api.xsirouter.com)", .func = cmd_host },
        { .command = "voice", .help = "voice <name> (default marin)", .func = cmd_voice },
        { .command = "prompt", .help = "prompt \"<instructions>\"", .func = cmd_prompt },
        { .command = "config", .help = "show settings (secrets masked)", .func = cmd_config },
        { .command = "clear", .help = "erase all settings", .func = cmd_clear },
        { .command = "setup", .help = "open the phone setup hotspot (= long press UP)", .func = cmd_setup },
        { .command = "poweroff", .help = "power off now (deep sleep; any key wakes)", .func = cmd_poweroff },
        { .command = "restart", .help = "reboot the device", .func = cmd_restart },
        { .command = "talk", .help = "start a conversation / interrupt (= OK click)", .func = cmd_talk },
        { .command = "hangup", .help = "end the conversation (= OK long press)", .func = cmd_hangup },
        { .command = "testmic", .help = "testmic on|off (uplink from fed audio)", .func = cmd_testmic },
        { .command = "testplay", .help = "testplay <http url of raw mu-law> (test audio)", .func = cmd_testplay },
        { .command = "volume", .help = "volume <0..150> (runtime only)", .func = cmd_volume },
        { .command = "autostart", .help = "autostart on|off (start a conversation after boot)", .func = cmd_autostart },
        { .command = "debug", .help = "debug on|off (session stats + transcripts in the log)", .func = cmd_debug },
        { .command = "nettest", .help = "DNS + TCP connect timing to the API host", .func = cmd_nettest },
    };

    // Same setup as esp_console_new_repl_usb_serial_jtag(), but with our own read loop:
    // ESP-IDF's REPL task retries linenoise() at once when it returns NULL, which it does
    // immediately while no USB host is attached (on battery). That busy loop starved the
    // idle task and the task watchdog rebooted the device every ~7 s after power-on.
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    usb_serial_jtag_driver_config_t drv = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&drv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_console_config_t con = ESP_CONSOLE_CONFIG_DEFAULT();
    con.max_cmdline_length = CONSOLE_LINE_MAX;
    con.hint_color = -1;
    err = esp_console_init(&con);
    if (err != ESP_OK) return err;
    esp_console_register_help_command();
    usb_serial_jtag_vfs_use_driver();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) return err;
    }
    if (xTaskCreate(console_task, "console", 4096, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
