// main/app_model.c -- shared state, command queue, and time helper.
#include "app.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

volatile int g_mic_level;
volatile int g_spk_level;
volatile int g_battery_soc = -1;
volatile bool g_debug;

static app_model_t s_model;
static SemaphoreHandle_t s_lock;

esp_err_t app_model_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_model, 0, sizeof(s_model));
    s_model.volume = 70;
    return ESP_OK;
}

void app_model_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void app_model_unlock(void)
{
    xSemaphoreGive(s_lock);
}

app_model_t *app_model(void)
{
    return &s_model;
}

void app_set_state(app_state_t state, const char *status)
{
    app_model_lock();
    s_model.state = state;
    if (status) snprintf(s_model.status, sizeof(s_model.status), "%s", status);
    if (state != APP_ST_LIVE) s_model.ai_speaking = false;
    s_model.version++;
    app_model_unlock();
}

void app_set_status(const char *status)
{
    app_model_lock();
    snprintf(s_model.status, sizeof(s_model.status), "%s", status ? status : "");
    s_model.version++;
    app_model_unlock();
}

void app_set_ai_speaking(bool speaking)
{
    app_model_lock();
    if (s_model.ai_speaking != speaking) {
        s_model.ai_speaking = speaking;
        s_model.version++;
    }
    app_model_unlock();
}

void app_set_volume(int volume)
{
    app_model_lock();
    s_model.volume = volume;
    s_model.version++;
    app_model_unlock();
}

void app_set_session_start(int64_t start_ms)
{
    app_model_lock();
    s_model.session_start_ms = start_ms;
    s_model.version++;
    app_model_unlock();
}

int64_t app_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}
