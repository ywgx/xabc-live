// main/app_testplay.c -- test-only audio source for automated end-to-end tests.
//
// `testplay <url>` downloads raw G.711 mu-law at the session rate (served by
// tools/live_autotest.py on the LAN) and feeds it into the test microphone at the
// pace the mic task consumes it, so the uplink carries real speech without a
// loudspeaker. Bulk audio deliberately avoids the USB console: ESP-IDF 5.5.3's
// linenoise dumb mode overruns its buffer on lines >= max_cmdline_length.
#include "app.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_testplay";
static volatile bool s_running;

static void testplay_task(void *arg)
{
    char *url = arg;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 5000, .buffer_size = 1024 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    size_t total = 0;
    if (c && esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        uint8_t buf[400];
        for (;;) {
            int n = esp_http_client_read(c, (char *)buf, sizeof(buf));
            if (n <= 0) break;
            size_t off = 0;
            int64_t last_progress = app_now_ms();
            while (off < (size_t)n && app_audio_test_mic_on()) {   // paced by the mic task
                size_t k = app_audio_test_feed(buf + off, (size_t)n - off);
                off += k;
                if (k) {
                    last_progress = app_now_ms();
                } else if (app_now_ms() - last_progress > 5000) {
                    break;   // nobody consumes the test audio (no session): give up
                }
            }
            total += off;
            if (off < (size_t)n) {
                ESP_LOGW(TAG, "testplay stopped early (consumer idle or test mic off)");
                break;
            }
        }
        ESP_LOGI(TAG, "testplay done: %u bytes", (unsigned)total);
    } else {
        ESP_LOGE(TAG, "testplay: cannot fetch %s", url);
    }
    if (c) esp_http_client_cleanup(c);
    free(url);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t app_testplay_start(const char *url)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    char *copy = strdup(url);
    if (!copy) return ESP_ERR_NO_MEM;
    s_running = true;
    if (xTaskCreate(testplay_task, "testplay", 4096, copy, 5, NULL) != pdPASS) {
        free(copy);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
