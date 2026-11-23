// main/live_policy.h -- timing and decision rules for the voice session.
// Pure C (no ESP-IDF/LVGL), host-tested by tests/test_live_policy.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Full duplex: the microphone always streams; the model tells the user's voice from its
// own echo. An OK click flushes local playback and drops the in-flight downlink audio for
// LIVE_BARGE_IN_MS (extended while the user keeps talking) so the user can speak up.
#define LIVE_LOUD_PEAK          900   // playback peak (int16) regarded as audible speech
#define LIVE_BARGE_IN_MS       2500

// Cost control: gpt-live-1 is billed by session time, including silence.
#define LIVE_IDLE_CLOSE_MS   300000   // close after 5 min without any transcript
#define LIVE_MAX_SESSION_MS 1800000   // hard cap for one session (30 min)
// Battery: power off (deep sleep, any key wakes) after this long without a conversation
// or a key press. Not while a computer is attached over USB (development, flashing).
#define LIVE_POWER_OFF_MS    600000

#define LIVE_VOLUME_STEP         10
#define LIVE_VOLUME_DEFAULT     150   // loudest: the small speaker is quiet otherwise
// 0..100 drives the ES8311 output volume; 110..150 keeps the codec at 100 and adds
// digital gain, 1.4x..3.0x. gpt-live-1 speech is quiet (RMS about -26 dBFS, 99% of
// peaks below -8 dBFS), so 3x (+9.5 dB) fits; a soft limiter rounds off the rare
// louder peaks instead of hard clipping.
#define LIVE_VOLUME_MAX         150
#define LIVE_GAIN_MAX_Q8        768   // 3.0x at LIVE_VOLUME_MAX
#define LIVE_LIMIT_KNEE       24000   // samples above this are compressed toward full scale

// True while downlink audio must be discarded after a user interrupt.
bool live_barge_in_active(int64_t now_ms, int64_t barge_until_ms);

// Keeps the window open for LIVE_BARGE_HOLD_MS after the user's last speech-level peak,
// so the old answer stays muted while the user is still talking.
#define LIVE_USER_SPEECH_PEAK  1200
#define LIVE_BARGE_HOLD_MS     1200
int64_t live_barge_extend(int64_t now_ms, int64_t barge_until_ms, uint16_t mic_peak);

// True when the device has been unused for LIVE_POWER_OFF_MS (idle_since_ms: start of
// the current unused stretch, 0 = in use now) and no USB host is attached.
bool live_should_power_off(int64_t now_ms, int64_t idle_since_ms, bool usb_host);

typedef enum {
    LIVE_CLOSE_NONE = 0,
    LIVE_CLOSE_IDLE,
    LIVE_CLOSE_MAX_DURATION,
} live_close_reason_t;

live_close_reason_t live_should_autoclose(int64_t now_ms, int64_t session_start_ms,
                                          int64_t last_activity_ms);

// Volume after one UP (+1) / DOWN (-1) step, clamped to 0..LIVE_VOLUME_MAX.
int live_volume_step(int volume, int direction);
// Codec volume (0..100) and digital gain in Q8 (256 = 1.0x) for a volume setting.
int live_volume_codec(int volume);
int live_volume_gain_q8(int volume);
// Multiplies samples by gain_q8/256; magnitudes above LIVE_LIMIT_KNEE are compressed
// smoothly so the result never exceeds the int16 range.
void live_pcm_apply_gain(int16_t *samples, size_t count, int gain_q8);

// Playback latency control. The server streams audio in real time (silence included),
// so every jitter underrun adds delay that never drains on its own. Above
// LIVE_LAG_SKIP_SILENCE_MS of queued audio, quiet frames are skipped to catch up;
// above LIVE_LAG_SKIP_ANY_MS any frame is skipped. Speech below that is never cut.
#define LIVE_LAG_SKIP_SILENCE_MS 1200   // normal pauses inside a reply are never skipped
#define LIVE_LAG_SKIP_ANY_MS     2400
#define LIVE_SILENCE_PEAK         400
bool live_play_should_skip(int queued_ms, uint16_t frame_peak);

// Largest absolute sample value (|INT16_MIN| saturates to 32767).
uint16_t live_pcm_peak(const int16_t *samples, size_t count);

// Mean absolute sample value (a cheap loudness measure).
uint16_t live_pcm_mean_abs(const int16_t *samples, size_t count);

// Recent playback loudness, for telling the assistant's own echo from the user's voice.
// The play task pushes one value per 20 ms frame; the mic task asks for the loudest
// value within a window (speaker -> DMA -> room -> mic takes up to a few hundred ms).
#define LIVE_SPK_HIST 24
typedef struct {
    uint16_t energy[LIVE_SPK_HIST];
    int64_t at_ms[LIVE_SPK_HIST];
    uint8_t next;
} live_spk_hist_t;
void live_spk_hist_push(live_spk_hist_t *h, int64_t now_ms, uint16_t energy);
uint16_t live_spk_hist_max(const live_spk_hist_t *h, int64_t now_ms, int window_ms);

// Echo suppressor (no acoustic echo cancellation on the board). Measured on the device at
// full volume: the assistant's own voice reaches the microphone at 0.1-0.8x the speaker
// level (91% of frames below 0.8, 98% below 1.6), loud enough for the model to transcribe
// and react to it ("are you playing something back?"). While the assistant is audible,
// microphone frames quieter than the recent speaker level are treated as echo and
// attenuated; two consecutive louder frames mean the user is talking over it, which opens
// the microphone for LIVE_ECHO_HOLD_MS so interrupting by voice keeps working.
#define LIVE_ECHO_SPK_MIN        200   // recent speaker mean-abs below this: nothing to suppress
#define LIVE_ECHO_WINDOW_MS      400   // speaker -> DMA -> room -> mic delay covered
#define LIVE_ECHO_PASS_X1000    1000   // mic louder than 1.0x speaker counts as the user
#define LIVE_ECHO_PASS_FRAMES      2   // ... for this many 50 ms frames in a row
#define LIVE_ECHO_HOLD_MS        300   // then stay open this long
#define LIVE_ECHO_DUCK_Q8         26   // echo frames are scaled by ~0.1
typedef struct {
    uint8_t loud_frames;
    int64_t open_until_ms;
} live_echo_sup_t;
// Gain (Q8, 256 = unchanged) for one microphone frame.
int live_echo_gain_q8(live_echo_sup_t *s, int64_t now_ms, uint16_t mic_energy, uint16_t spk_recent);

// Maps a peak to a 0..100 UI level (roughly logarithmic so speech moves the meter).
int live_level_percent(uint16_t peak);

// Voice goodbye: true when a whole user utterance is a short farewell ("拜拜", "好的再见",
// "关闭吧", "bye"). Only short utterances count, so "怎么关闭电脑的自动更新" does not end
// the conversation. Characters are counted without spaces and punctuation.
#define LIVE_FAREWELL_MAX_CHARS 12
#define LIVE_FAREWELL_GRACE_MS 1500    // let the assistant start its goodbye
#define LIVE_FAREWELL_MAX_WAIT_MS 10000 // close even if the goodbye never goes quiet
bool live_is_farewell(const char *utterance);
