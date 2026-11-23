// main/app.h -- shared application state for the GPT-Live voice companion.
//
// Ownership: worker tasks (Wi-Fi events, live session, audio) write the model
// through the setters below; the LVGL UI timer is the only reader that renders it.
// All fields are protected by one mutex; the meters are plain atomics-sized ints.
#pragma once

#include "esp_err.h"
#include "live_config.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_ST_SETUP = 0,       // Wi-Fi or API key missing: show provisioning help
    APP_ST_WIFI,            // connecting to Wi-Fi
    APP_ST_IDLE,            // ready, waiting for OK
    APP_ST_CONNECTING,      // opening the Live session
    APP_ST_LIVE,            // session running (listening / assistant speaking)
    APP_ST_CLOSING,         // session.close sent
    APP_ST_ERROR,           // last action failed; OK retries
} app_state_t;

typedef enum {
    APP_CMD_TOGGLE = 1,     // OK click: start, interrupt, or retry
    APP_CMD_STOP,           // OK long press: end the session
    APP_CMD_VOLUME_UP,
    APP_CMD_VOLUME_DOWN,
    APP_CMD_CONFIG_CHANGED, // console updated NVS settings
    APP_CMD_POWER_OFF,      // console `poweroff`
    APP_CMD_SETUP,          // ▲ long press: open the phone setup portal
} app_cmd_t;

typedef struct {
    app_state_t state;
    bool ai_speaking;
    int volume;
    int64_t session_start_ms; // 0 when no session is running
    char status[96];        // short UTF-8 status line
    unsigned version;       // bumps on every change except meters
} app_model_t;

esp_err_t app_model_init(void);
void app_model_lock(void);
void app_model_unlock(void);
app_model_t *app_model(void);   // call only between lock/unlock

// Convenience setters (each takes the lock internally).
void app_set_state(app_state_t state, const char *status);
void app_set_status(const char *status);
void app_set_ai_speaking(bool speaking);
void app_set_volume(int volume);
void app_set_session_start(int64_t start_ms);

// Meters for the UI (0..100), written by the audio tasks without locking.
extern volatile int g_mic_level;
extern volatile int g_spk_level;
extern volatile uint32_t g_play_underruns;   // playback stalls (network jitter)
extern volatile uint32_t g_play_dropped;     // downlink bytes dropped on a full buffer
extern volatile uint32_t g_play_skipped;     // quiet frames skipped to cut accumulated delay
extern volatile uint32_t g_echo_ducked;     // mic frames attenuated as the assistant's own echo
extern volatile int g_battery_soc;
extern volatile bool g_debug;   // console `debug on`: per-session stats and transcripts in the log   // -1 when unavailable; refreshed by the session task

// Commands from buttons / console to the session task (non-blocking).
void app_post(app_cmd_t cmd);

// ---- configuration (app_config.c) ----
esp_err_t app_config_init(void);                 // NVS init + load
void app_config_get(live_config_t *out);         // copy of the current settings
esp_err_t app_config_set_wifi(const char *ssid, const char *pass);   // add or update a saved network
esp_err_t app_config_set_wifi_first(const char *ssid, const char *pass);   // ... and try it first
esp_err_t app_config_set_pending_wifi(const char *ssid, const char *pass);  // setup page: verify first
bool app_config_get_pending_wifi(live_wifi_net_t *out);                     // read (kept until cleared)
void app_config_clear_pending_wifi(void);                                   // after commit / definite failure
esp_err_t app_config_del_wifi(const char *ssid);
esp_err_t app_config_set_key(const char *key);
esp_err_t app_config_set_host(const char *host);
esp_err_t app_config_set_voice(const char *voice);
esp_err_t app_config_set_prompt(const char *prompt);
esp_err_t app_config_clear(void);
int app_config_volume(void);
esp_err_t app_config_set_autostart(bool on);       // default on
void app_config_note_boot(void);                    // log + keep the last 8 reset reasons in NVS
bool app_diag_take(char *out, size_t cap);          // task names seen by the last watchdog reset
// Deep sleep after showing `reason`; any key restarts the device. Call only while no
// session runs (audio tasks idle). Does not return.
void app_power_off(const char *reason);
const char *app_config_reset_history(void);         // "brownout,poweron,..." newest first
const char *app_config_last_wdt(void);              // tasks seen by the latest watchdog reset
esp_err_t app_config_save_volume(int volume);

// ---- services ----
esp_err_t app_console_start(void);               // USB serial provisioning commands
esp_err_t app_wifi_start(void);                  // STA with stored credentials
void app_wifi_reconfigure(void);                 // apply changed credentials
bool app_wifi_connected(void);
bool app_wifi_wait(int timeout_ms);
void app_wifi_pause(void);                       // stop joining networks (setup portal owns the radio)
bool app_wifi_pending(void);
int app_wifi_failed_rounds(void);                // consecutive failed scan+join rounds                     // a network from the setup page is being verified
const char *app_wifi_setup_error(void);          // why it failed ("" if none); shown on the page
// Setup portal (app_portal.c): open hotspot + captive web page to enter Wi-Fi and the
// API key; the device restarts after saving. Call only while no session runs.
esp_err_t app_portal_start(const char *error);  // error: shown on screen and page, or NULL
bool app_portal_active(void);
const char *app_portal_ssid(void);               // hotspot name while the portal runs, else ""
// Plain TCP connect probe (no data sent); returns connect time in ms or -1.
int app_net_tcp_check(const char *host, const char *port, int timeout_ms);
// Verifies TCP to the API host; on failure performs a clean Wi-Fi disassociate +
// reassociate (clears stale AP-side station state) and retries. Returns true when OK.
bool app_net_ensure(int attempts);
esp_err_t app_audio_start(void);                 // mic + speaker workers, 8 kHz
void app_audio_test_mic(bool on);                // uplink uses fed audio instead of the mic
bool app_audio_test_mic_on(void);
size_t app_audio_test_feed(const uint8_t *ulaw, size_t n);
// Test-only: stream mu-law from a LAN URL into the test microphone.
esp_err_t app_testplay_start(const char *url);
void app_audio_enable(bool on);                  // start/stop streaming
void app_audio_interrupt(void);                  // flush playback, open the microphone
bool app_audio_recently_playing(void);           // assistant audible within 3 s or audio queued
int64_t app_audio_ms_since_speech(void);         // since the assistant was last audible (INT64_MAX: never)
void app_audio_set_volume(int volume);           // 0..150: codec volume + digital gain
esp_err_t app_live_start(void);                  // session controller task
bool app_auto_start_enabled(void);               // start a conversation right after boot
esp_err_t app_ui_start(void);                    // builds the screen + UI timer

// Downlink audio from the live session (8 kHz mono mu-law).
void app_audio_play_ulaw(const uint8_t *ulaw, size_t n);
// Uplink sink installed by the live session: G.711 mu-law bytes, one per sample.
typedef bool (*app_uplink_fn)(const uint8_t *ulaw, size_t bytes);
void app_audio_set_uplink(app_uplink_fn fn);

int64_t app_now_ms(void);

// Wire format: 8 kHz G.711 mu-law (audio/pcmu) both ways.
#define APP_SAMPLE_RATE 8000
