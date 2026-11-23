// main/main.c -- FoloToy AI Passport · ChatGPT voice companion.
//
// Talks to gpt-live-1 through the OpenAI Live API offered by XAI Router
// (wss://api.xairouter.com/v1/live/sessions). Wi-Fi and the API key are
// provisioned into NVS over USB (tools/live_provision.py).
//
// Buttons:
//   OK click        start a conversation / interrupt the assistant / retry
//   OK long press   end the conversation
//   UP / DOWN click volume +/- 10 %
#include "app.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "main";

static volatile bool s_input_ready;

// A key held through a deep-sleep wake would otherwise register as a long press
// right after boot; drop key actions until it is released (see key_guard.h).
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// Runs on the shared esp_timer task: translate to a command and return at once.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Consume the guard before the readiness check so an early release disarms it.
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready) return;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        app_post(APP_CMD_TOGGLE);
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        app_post(APP_CMD_STOP);
    } else if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
        app_post(APP_CMD_SETUP);
    } else if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        app_post(APP_CMD_VOLUME_UP);
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        app_post(APP_CMD_VOLUME_DOWN);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport · ChatGPT 启动");
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // transport_ws prints the whole upgrade request (incl. "Authorization: Bearer <key>")
    // on handshake write errors; never let the API key reach the serial log.
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    ESP_ERROR_CHECK(app_model_init());
    ESP_ERROR_CHECK(app_config_init());
    app_config_note_boot();
    app_set_volume(app_config_volume());

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败 (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(10);   // dim by default (user choice): longer battery life
    ESP_ERROR_CHECK(app_ui_start());

    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用，右上角不显示电量");

    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) err = app_audio_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(err));
        app_set_state(APP_ST_ERROR, "音频初始化失败");
        return;
    }

    ESP_ERROR_CHECK(app_wifi_start());
    ESP_ERROR_CHECK(app_live_start());
    if (app_console_start() != ESP_OK) ESP_LOGW(TAG, "USB 配置控制台未启动");

    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    } else if (s_key_guard.armed) {
        key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪, free heap=%u", (unsigned)esp_get_free_heap_size());
}
