// main/live_policy.c -- see live_policy.h.
#include "live_policy.h"

#include <ctype.h>
#include <string.h>

bool live_barge_in_active(int64_t now_ms, int64_t barge_until_ms)
{
    return now_ms < barge_until_ms;
}

int64_t live_barge_extend(int64_t now_ms, int64_t barge_until_ms, uint16_t mic_peak)
{
    if (!live_barge_in_active(now_ms, barge_until_ms) || mic_peak < LIVE_USER_SPEECH_PEAK) {
        return barge_until_ms;
    }
    const int64_t hold = now_ms + LIVE_BARGE_HOLD_MS;
    return hold > barge_until_ms ? hold : barge_until_ms;
}

live_close_reason_t live_should_autoclose(int64_t now_ms, int64_t session_start_ms,
                                          int64_t last_activity_ms)
{
    if (now_ms - session_start_ms >= LIVE_MAX_SESSION_MS) return LIVE_CLOSE_MAX_DURATION;
    int64_t since = last_activity_ms > session_start_ms ? last_activity_ms : session_start_ms;
    if (now_ms - since >= LIVE_IDLE_CLOSE_MS) return LIVE_CLOSE_IDLE;
    return LIVE_CLOSE_NONE;
}

int live_volume_step(int volume, int direction)
{
    int v = volume + (direction > 0 ? LIVE_VOLUME_STEP : direction < 0 ? -LIVE_VOLUME_STEP : 0);
    if (v < 0) v = 0;
    if (v > LIVE_VOLUME_MAX) v = LIVE_VOLUME_MAX;
    return v;
}

int live_volume_codec(int volume)
{
    return volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

int live_volume_gain_q8(int volume)
{
    if (volume <= 100) return 256;
    if (volume > LIVE_VOLUME_MAX) volume = LIVE_VOLUME_MAX;
    return 256 + (volume - 100) * (LIVE_GAIN_MAX_Q8 - 256) / (LIVE_VOLUME_MAX - 100);
}

void live_pcm_apply_gain(int16_t *samples, size_t count, int gain_q8)
{
    if (gain_q8 == 256) return;
    const int32_t room = 32767 - LIVE_LIMIT_KNEE;
    for (size_t i = 0; i < count; i++) {
        int32_t v = ((int32_t)samples[i] * gain_q8) / 256;
        int32_t m = v < 0 ? -v : v;
        if (m > LIVE_LIMIT_KNEE) {
            // Soft knee: the excess x maps to room * x / (x + room), approaching full scale.
            const int32_t x = m - LIVE_LIMIT_KNEE;
            m = LIVE_LIMIT_KNEE + (int32_t)((int64_t)room * x / (x + room));
        }
        samples[i] = (int16_t)(v < 0 ? -m : m);
    }
}

bool live_play_should_skip(int queued_ms, uint16_t frame_peak)
{
    if (queued_ms > LIVE_LAG_SKIP_ANY_MS) return true;
    return queued_ms > LIVE_LAG_SKIP_SILENCE_MS && frame_peak < LIVE_SILENCE_PEAK;
}

uint16_t live_pcm_peak(const int16_t *samples, size_t count)
{
    uint16_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        uint16_t a = (uint16_t)(s < 0 ? (s == INT16_MIN ? 32767 : -s) : s);
        if (a > peak) peak = a;
    }
    return peak;
}

int live_level_percent(uint16_t peak)
{
    // Piecewise mapping: below 200 is noise, 200..32767 spans ~7.4 octaves.
    if (peak < 200) return 0;
    int bits = 0;
    uint32_t v = peak / 200u;
    while (v > 1) {
        v >>= 1;
        bits++;
    }
    int level = 15 + bits * 12;   // 200 -> 15, ~25600 -> 99
    return level > 100 ? 100 : level;
}

bool live_is_farewell(const char *utterance)
{
    if (!utterance) return false;
    // Lower-case ASCII copy without spaces/punctuation; count characters (UTF-8 code points).
    char t[160];
    size_t n = 0;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)utterance; *p && n + 4 < sizeof(t);) {
        if (*p < 0x80) {
            if (isalnum(*p)) {
                t[n++] = (char)tolower(*p);
                chars++;
            }
            p++;
            continue;
        }
        size_t len = (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 1;
        // Skip full-width punctuation: U+3000..U+303F (。、「」…) and U+FF00..U+FF0F/1A..20 (，！？：；).
        const bool cjk_punct = len == 3 && ((p[0] == 0xE3 && p[1] == 0x80) ||
                                            (p[0] == 0xEF && p[1] == 0xBC &&
                                             (p[2] <= 0x8F || (p[2] >= 0x9A && p[2] <= 0xA0))) ||
                                            (p[0] == 0xE2 && p[1] == 0x80));   // “”…—
        for (size_t k = 0; k < len && p[k]; k++) {
            if (!cjk_punct) t[n++] = (char)p[k];
        }
        if (!cjk_punct) chars++;
        p += len;
    }
    t[n] = '\0';
    if (chars == 0 || chars > LIVE_FAREWELL_MAX_CHARS) return false;
    static const char *const words[] = {
        "拜拜", "再见", "回见", "关闭", "关掉", "结束", "挂了", "挂断", "不聊了", "退下", "bye",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(t, words[i])) return true;
    }
    return false;
}

bool live_should_power_off(int64_t now_ms, int64_t idle_since_ms, bool usb_host)
{
    return !usb_host && idle_since_ms > 0 && now_ms - idle_since_ms >= LIVE_POWER_OFF_MS;
}
