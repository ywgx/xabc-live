// main/app_audio.c -- full-duplex 8 kHz audio for the Live session.
//
// Two workers share the ES8311 (bsp_audio_read/write block, so neither may run
// in a button callback or the LVGL task):
//   mic task:  50 ms capture -> mu-law -> uplink callback (the mic is never muted)
//   play task: 20 ms frames from a 2.4 s buffer -> speaker; feeds silence on underrun
//              so the I2S clock stays continuous, and re-prebuffers 600 ms.
// The playback queue holds G.711 mu-law exactly as received (one byte per sample).
// Echo control (no AEC on this board): while playback is audibly loud the uplink
// carries zeros; app_audio_interrupt() opens the mic and drops downlink audio for
// LIVE_BARGE_IN_MS so the full-duplex model hears the user and yields.
#include "app.h"
#include "bsp_audio.h"
#include "live_g711.h"
#include "live_policy.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "app_audio";

#define MIC_SAMPLES     (APP_SAMPLE_RATE / 20)   // 50 ms
#define FRAME_SAMPLES   (APP_SAMPLE_RATE / 50)   // 20 ms
#define PLAY_BUF_BYTES  19200                    // 2.4 s of 8 kHz mu-law
#define PREBUFFER_MS    600                      // jitter cushion before playback starts
#define BIT_ENABLED     BIT0

static StreamBufferHandle_t s_play_buf;
static EventGroupHandle_t s_events;
static volatile bool s_enabled;
static volatile bool s_flush_req;
static volatile int64_t s_last_loud_ms;
static volatile int64_t s_barge_until_ms;
static volatile app_uplink_fn s_uplink;
static volatile uint32_t s_dropped_bytes;
static volatile int s_gain_q8 = 256;
// Test microphone: when enabled, the uplink carries audio fed over the USB console
// (tools/live_autotest.py) instead of the real microphone, still paced by I2S capture.
static StreamBufferHandle_t s_test_buf;
static volatile bool s_test_mic;
volatile uint32_t g_play_underruns;
volatile uint32_t g_play_dropped;
volatile uint32_t g_play_skipped;

// The 1 s test buffer is created by the first `testmic on` (automated tests only) and
// then kept: deleting it could race a feeder blocked in xStreamBufferSend().
void app_audio_test_mic(bool on)
{
    if (on && !s_test_buf) s_test_buf = xStreamBufferCreate(APP_SAMPLE_RATE, 1);
    s_test_mic = on && s_test_buf;
}

bool app_audio_test_mic_on(void)
{
    return s_test_mic;
}

size_t app_audio_test_feed(const uint8_t *ulaw, size_t n)
{
    if (!s_test_buf || !s_test_mic) return 0;
    return xStreamBufferSend(s_test_buf, ulaw, n, pdMS_TO_TICKS(200));
}

void app_audio_set_volume(int volume)
{
    bsp_audio_set_volume((uint8_t)live_volume_codec(volume));
    s_gain_q8 = live_volume_gain_q8(volume);
}

void app_audio_set_uplink(app_uplink_fn fn)
{
    s_uplink = fn;
}

void app_audio_enable(bool on)
{
    if (on) {
        s_last_loud_ms = 0;
        s_barge_until_ms = 0;
        s_flush_req = true;
        s_enabled = true;
        xEventGroupSetBits(s_events, BIT_ENABLED);
    } else {
        s_enabled = false;
        xEventGroupClearBits(s_events, BIT_ENABLED);
        g_mic_level = 0;
        g_spk_level = 0;
    }
}

void app_audio_interrupt(void)
{
    s_barge_until_ms = app_now_ms() + LIVE_BARGE_IN_MS;
    s_last_loud_ms = 0;
    s_flush_req = true;
}

int64_t app_audio_ms_since_speech(void)
{
    const int64_t loud = s_last_loud_ms;
    return loud > 0 ? app_now_ms() - loud : INT64_MAX;
}

bool app_audio_recently_playing(void)
{
    // Speech within the last 3 s (pauses between sentences included) or audio queued.
    const int64_t loud = s_last_loud_ms;
    return (loud > 0 && app_now_ms() - loud < 3000) ||
           (s_play_buf && xStreamBufferBytesAvailable(s_play_buf) > 0);
}

static bool play_accepting(void)
{
    return s_enabled && !live_barge_in_active(app_now_ms(), s_barge_until_ms);
}

static void queue_ulaw(const uint8_t *ulaw, size_t n)
{
    // Called from the WebSocket task, which holds the client lock: never block long.
    size_t sent = xStreamBufferSend(s_play_buf, ulaw, n, pdMS_TO_TICKS(10));
    if (sent < n) {
        s_dropped_bytes += n - sent;
        g_play_dropped = s_dropped_bytes;
    }
}

void app_audio_play_ulaw(const uint8_t *ulaw, size_t n)
{
    if (play_accepting()) queue_ulaw(ulaw, n);
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t pcm[MIC_SAMPLES];
    static uint8_t ulaw[MIC_SAMPLES];
    const size_t n = MIC_SAMPLES;
    for (;;) {
        xEventGroupWaitBits(s_events, BIT_ENABLED, pdFALSE, pdTRUE, portMAX_DELAY);
        if (bsp_audio_read(pcm, n * sizeof(int16_t)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_enabled) continue;
        if (s_test_mic && s_test_buf) {
            size_t got = xStreamBufferReceive(s_test_buf, ulaw, n, 0);
            memset(ulaw + got, 0xFF, n - got);   // mu-law silence
            live_ulaw_decode_buf(ulaw, n, pcm);
        }
        const int64_t now = app_now_ms();
        const uint16_t mic_peak = live_pcm_peak(pcm, n);
        s_barge_until_ms = live_barge_extend(now, s_barge_until_ms, mic_peak);
        g_mic_level = live_level_percent(mic_peak);
        live_ulaw_encode_buf(pcm, n, ulaw);
        app_uplink_fn fn = s_uplink;
        if (fn) fn(ulaw, n);
    }
}

static void play_task(void *arg)
{
    (void)arg;
    static uint8_t ulaw[FRAME_SAMPLES];
    static int16_t frame[FRAME_SAMPLES];
    const size_t n = FRAME_SAMPLES;
    const size_t prebuffer = (size_t)APP_SAMPLE_RATE * PREBUFFER_MS / 1000;
    bool prebuffering = true;
    bool speaking = false;
    uint32_t reported_drops = 0;
    for (;;) {
        if (!s_enabled) {
            // Leave the DMA ring holding silence, then sleep until the next session.
            memset(frame, 0, sizeof(frame));
            for (int i = 0; i < 6; i++) bsp_audio_write(frame, n * sizeof(int16_t));
            if (speaking) {
                speaking = false;
                app_set_ai_speaking(false);
            }
            xEventGroupWaitBits(s_events, BIT_ENABLED, pdFALSE, pdTRUE, portMAX_DELAY);
            prebuffering = true;
        }
        if (s_flush_req) {
            s_flush_req = false;
            while (xStreamBufferReceive(s_play_buf, ulaw, n, 0) > 0) {
            }
            prebuffering = true;
        }
        size_t got = 0;
        if (prebuffering && xStreamBufferBytesAvailable(s_play_buf) >= prebuffer) {
            prebuffering = false;
        }
        if (!prebuffering) {
            // Catch up on accumulated delay by dropping quiet frames (see live_policy.h).
            for (;;) {
                const int queued_ms = (int)(xStreamBufferBytesAvailable(s_play_buf) * 1000 / APP_SAMPLE_RATE);
                if (queued_ms <= LIVE_LAG_SKIP_SILENCE_MS) break;
                if (xStreamBufferReceive(s_play_buf, ulaw, n, 0) != n) break;
                live_ulaw_decode_buf(ulaw, n, frame);
                if (!live_play_should_skip(queued_ms, live_pcm_peak(frame, n))) {
                    got = n;   // keep this frame: it is speech
                    break;
                }
                g_play_skipped++;
            }
            if (got == 0) {
                got = xStreamBufferReceive(s_play_buf, ulaw, n, 0);
                live_ulaw_decode_buf(ulaw, got, frame);
            }
            if (got == 0) {
                prebuffering = true;   // underrun: rebuild the jitter cushion
                g_play_underruns++;
            }
        }
        if (got < n) memset(frame + got, 0, (n - got) * sizeof(int16_t));
        live_pcm_apply_gain(frame, n, s_gain_q8);

        const uint16_t peak = live_pcm_peak(frame, n);
        const int64_t now = app_now_ms();
        if (peak >= LIVE_LOUD_PEAK) s_last_loud_ms = now;
        g_spk_level = live_level_percent(peak);
        const bool now_speaking = s_last_loud_ms > 0 && now - s_last_loud_ms < 600;
        if (now_speaking != speaking) {
            speaking = now_speaking;
            app_set_ai_speaking(speaking);
        }
        if (s_dropped_bytes != reported_drops) {
            reported_drops = s_dropped_bytes;
            ESP_LOGW(TAG, "播放缓冲已满，累计丢弃 %u 字节", (unsigned)reported_drops);
        }
        if (bsp_audio_write(frame, n * sizeof(int16_t)) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t app_audio_start(void)
{
    s_events = xEventGroupCreate();
    s_play_buf = xStreamBufferCreate(PLAY_BUF_BYTES, 1);
    if (!s_events || !s_play_buf) return ESP_ERR_NO_MEM;
    esp_err_t err = bsp_audio_set_format(APP_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频格式设置失败: %s", esp_err_to_name(err));
        return err;
    }
    app_audio_set_volume(app_config_volume());
    // Priority above LVGL rendering so redraws cannot starve PCM delivery.
    if (xTaskCreate(play_task, "live_play", 3072, NULL, 7, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(mic_task, "live_mic", 4096, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
