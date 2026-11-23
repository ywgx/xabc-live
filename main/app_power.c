// main/app_power.c -- automatic power off: deep sleep with any key as the wake source.
//
// The board has no software power switch; deep sleep draws next to nothing and a key
// press restarts the application (which then opens a conversation again). The order
// follows the BSP contract (see demo_low_power.c): key pad hand-off, wake source, CW2017,
// ES8311, I2S, shared I2C, then the LCD with LVGL stopped.
#include "app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_power";

static void warn(const char *step, esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGW(TAG, "关机继续：%s 失败: %s", step, esp_err_to_name(err));
}

void app_power_off(const char *reason)
{
    ESP_LOGI(TAG, "%s -> 深度睡眠", reason);
    app_set_status(reason);
    vTaskDelay(pdMS_TO_TICKS(2000));   // let the user read it
    // A clean disassociation: APs keep no stale station state for the next boot.
    esp_wifi_disconnect();
    esp_wifi_stop();
    int level = 1;
    if (bsp_button_prepare_deep_sleep(&level) != ESP_OK) esp_restart();   // keys are gone: no way back
    warn("GPIO wake", esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
    warn("CW2017 suspend", bsp_battery_sleep());
    warn("ES8311 suspend", bsp_audio_sleep());
    warn("I2S pin release", bsp_audio_prepare_deep_sleep());
    warn("shared I2C pin release", bsp_i2c_prepare_deep_sleep());
    if (!bsp_lvgl_lock(1000)) esp_restart();   // never sleep the LCD under a running flush
    warn("ST7789 suspend", bsp_display_prepare_deep_sleep());
    esp_deep_sleep_start();
    esp_restart();   // not reached
}
