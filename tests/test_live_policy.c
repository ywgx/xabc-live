// Host tests for main/live_policy.c and main/live_config.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "live_config.h"
#include "live_policy.h"

static void test_barge_window(void)
{
    assert(live_barge_in_active(5000, 5000 + LIVE_BARGE_IN_MS));
    assert(!live_barge_in_active(5000 + LIVE_BARGE_IN_MS, 5000 + LIVE_BARGE_IN_MS));
    // Speech during the window keeps it open; silence lets it expire; no effect when closed.
    assert(live_barge_extend(5000, 7500, LIVE_USER_SPEECH_PEAK) == 7500);         // still later
    assert(live_barge_extend(7000, 7500, LIVE_USER_SPEECH_PEAK) == 7000 + LIVE_BARGE_HOLD_MS);
    assert(live_barge_extend(7000, 7500, LIVE_USER_SPEECH_PEAK - 1) == 7500);     // quiet
    assert(live_barge_extend(8000, 7500, 30000) == 7500);                         // window over
}

static void test_autoclose(void)
{
    const int64_t start = 10000;
    assert(live_should_autoclose(start + 1000, start, 0) == LIVE_CLOSE_NONE);
    // No activity at all: idle timeout counts from session start.
    assert(live_should_autoclose(start + LIVE_IDLE_CLOSE_MS, start, 0) == LIVE_CLOSE_IDLE);
    // Recent transcript activity keeps the session open.
    assert(live_should_autoclose(start + LIVE_IDLE_CLOSE_MS, start, start + 30000) == LIVE_CLOSE_NONE);
    assert(live_should_autoclose(start + 30000 + LIVE_IDLE_CLOSE_MS, start, start + 30000) ==
           LIVE_CLOSE_IDLE);
    // Hard cap wins even with constant activity.
    assert(live_should_autoclose(start + LIVE_MAX_SESSION_MS, start, start + LIVE_MAX_SESSION_MS - 1) ==
           LIVE_CLOSE_MAX_DURATION);
}

static void test_volume_and_level(void)
{
    assert(live_volume_step(70, +1) == 80);
    assert(live_volume_step(95, +1) == 105);
    assert(live_volume_step(100, +1) == 110);
    assert(live_volume_step(145, +1) == LIVE_VOLUME_MAX && live_volume_step(150, +1) == 150);
    assert(live_volume_codec(70) == 70 && live_volume_codec(130) == 100 && live_volume_codec(-5) == 0);
    assert(live_volume_gain_q8(100) == 256 && live_volume_gain_q8(70) == 256);
    assert(live_volume_gain_q8(150) == LIVE_GAIN_MAX_Q8 && live_volume_gain_q8(200) == LIVE_GAIN_MAX_Q8);
    assert(live_volume_gain_q8(110) > 256 && live_volume_gain_q8(110) < live_volume_gain_q8(120));
    int16_t g[6] = { 1000, -1000, 8000, 30000, -32768, 32767 };
    live_pcm_apply_gain(g, 6, 768);   // 3x
    assert(g[0] == 3000 && g[1] == -3000 && g[2] == 24000);   // below the knee: exact
    assert(g[3] > LIVE_LIMIT_KNEE && g[3] < 32767);            // 90000 compressed, no wrap
    assert(g[4] < -LIVE_LIMIT_KNEE && g[4] >= -32767 && g[5] == -g[4]);
    // Monotonic: louder input never gives quieter output.
    int16_t a[1] = { 9000 }, b[1] = { 10000 };
    live_pcm_apply_gain(a, 1, 768);
    live_pcm_apply_gain(b, 1, 768);
    assert(b[0] > a[0]);
    int16_t same[1] = { 1234 };
    live_pcm_apply_gain(same, 1, 256);
    assert(same[0] == 1234);
    assert(live_volume_step(5, -1) == 0);
    assert(live_volume_step(0, -1) == 0);
    assert(live_volume_step(40, 0) == 40);

    const int16_t quiet[] = { 0, 10, -20, 5 };
    const int16_t loud[] = { 100, -32768, 2000 };
    assert(live_pcm_peak(quiet, 4) == 20);
    assert(live_pcm_peak(loud, 3) == 32767);
    assert(live_pcm_peak(NULL, 0) == 0);
    assert(live_level_percent(0) == 0 && live_level_percent(199) == 0);
    assert(live_level_percent(200) == 15);
    int prev = 0;
    for (uint32_t p = 200; p <= 32767; p += 997) {
        int l = live_level_percent((uint16_t)p);
        assert(l >= prev && l <= 100);
        prev = l;
    }
    assert(live_level_percent(32767) >= 90);
}

static void test_config(void)
{
    live_config_t c;
    live_config_defaults(&c);
    assert(strcmp(c.host, "api.xsirouter.com") == 0 && strcmp(c.voice, "marin") == 0);
    assert(c.prompt[0] && !live_config_ready(&c));
    assert(live_wifi_upsert(&c, "home", "12345678"));
    assert(!live_config_ready(&c));
    strcpy(c.api_key, "sk-0123456789abcdef");
    assert(live_config_ready(&c));

    assert(live_config_valid_ssid("我的WiFi") && !live_config_valid_ssid(""));
    assert(!live_config_valid_ssid("012345678901234567890123456789012"));   // 33 bytes
    assert(live_config_valid_wifi_pass("") && live_config_valid_wifi_pass("12345678"));
    assert(!live_config_valid_wifi_pass("1234567"));
    assert(live_config_valid_wifi_pass("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    assert(!live_config_valid_wifi_pass("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"));
    assert(live_config_valid_api_key("sk-0123456789abcdef"));
    assert(!live_config_valid_api_key("sk-short"));
    assert(!live_config_valid_api_key("sk-0123456789 abcdef"));
    assert(!live_config_valid_api_key("sk-0123456789\"abcdef"));
    assert(live_config_valid_host("api.xsirouter.com") && live_config_valid_host("10.0.0.2:8443"));
    assert(!live_config_valid_host("https://api.xsirouter.com"));
    assert(!live_config_valid_host("api.xsirouter.com/v1") && !live_config_valid_host("host:99999"));
    assert(!live_config_valid_host("host:") && !live_config_valid_host(".x.com"));
    assert(live_config_valid_voice("marin") && live_config_valid_voice("cove"));
    assert(!live_config_valid_voice("Marin") && !live_config_valid_voice(""));
    assert(live_config_valid_prompt("") && live_config_valid_prompt("第一行\n第二行"));
    assert(!live_config_valid_prompt("tab\there"));

    // Saved network list: update, append, drop oldest when full, remove.
    live_config_defaults(&c);
    assert(live_wifi_upsert(&c, "HomeWiFi", "password1") && c.wifi_count == 1);
    assert(live_wifi_upsert(&c, "Phone", "hotspot88") && c.wifi_count == 2);
    assert(live_wifi_upsert(&c, "Phone", "newpass99") && c.wifi_count == 2);
    assert(strcmp(c.wifi[1].pass, "newpass99") == 0);
    assert(!live_wifi_upsert(&c, "", "password1") && !live_wifi_upsert(&c, "bad", "short"));
    assert(live_wifi_upsert(&c, "open-net", "") && c.wifi_count == 3);
    assert(live_wifi_upsert(&c, "D", "password4") && c.wifi_count == 4);
    assert(live_wifi_upsert(&c, "E", "password5") && c.wifi_count == 4);    // oldest dropped
    assert(live_wifi_find(&c, "HomeWiFi") == -1 && live_wifi_find(&c, "E") == 3);
    assert(live_wifi_remove(&c, "open-net") && c.wifi_count == 3 && !live_wifi_remove(&c, "nope"));
    assert(strcmp(c.wifi[0].ssid, "Phone") == 0 && strcmp(c.wifi[2].ssid, "E") == 0);

    // Picking: strongest visible saved network, skipping ones already tried.
    live_config_defaults(&c);
    live_wifi_upsert(&c, "HomeWiFi", "password1");
    live_wifi_upsert(&c, "Phone", "hotspot88");
    const live_scan_ap_t scan[] = {
        { "Neighbour", -20 }, { "Phone", -60 }, { "HomeWiFi", -45 }, { "HomeWiFi", -70 }, { NULL, 0 },
    };
    assert(live_wifi_pick(&c, scan, 5, 0) == 0);              // HomeWiFi, first saved
    // The phone hotspot next to the device is stronger, but home is saved first and usable.
    const live_scan_ap_t phone_near[] = { { "Phone", -25 }, { "HomeWiFi", -60 } };
    assert(live_wifi_pick(&c, phone_near, 2, 0) == 0);
    // Home too weak to be usable: take the strongest visible saved network instead.
    const live_scan_ap_t home_weak[] = { { "Phone", -40 }, { "HomeWiFi", -88 } };
    assert(live_wifi_pick(&c, home_weak, 2, 0) == 1);
    const live_scan_ap_t both_weak[] = { { "Phone", -85 }, { "HomeWiFi", -90 } };
    assert(live_wifi_pick(&c, both_weak, 2, 0) == 1);
    assert(live_wifi_pick(&c, scan, 5, 1u << 0) == 1);        // it failed: fall back to the hotspot
    assert(live_wifi_pick(&c, scan, 5, 3u) == -1);            // both tried
    const live_scan_ap_t only_hotspot[] = { { "Phone", -70 } };
    assert(live_wifi_pick(&c, only_hotspot, 1, 0) == 1);      // away from home
    assert(live_wifi_pick(&c, scan, 0, 0) == -1);

    char m[32];
    live_config_mask_key("sk-TESTfake0000000000000000000000000000Fake", m, sizeof(m));
    assert(strcmp(m, "sk-TEST...Fake") == 0);
    live_config_mask_key("short", m, sizeof(m));
    assert(strcmp(m, "****") == 0);
    live_config_mask_key("", m, sizeof(m));
    assert(strcmp(m, "(未设置)") == 0);
}

static void test_latency_control(void)
{
    // Normal cushion: never skip, even silence.
    assert(!live_play_should_skip(300, 0));
    assert(!live_play_should_skip(LIVE_LAG_SKIP_SILENCE_MS, 0));
    // Lagging: skip silence, keep speech.
    assert(live_play_should_skip(LIVE_LAG_SKIP_SILENCE_MS + 1, 0));
    assert(live_play_should_skip(LIVE_LAG_SKIP_SILENCE_MS + 300, LIVE_SILENCE_PEAK - 1));
    assert(!live_play_should_skip(LIVE_LAG_SKIP_SILENCE_MS + 300, LIVE_SILENCE_PEAK));
    assert(!live_play_should_skip(LIVE_LAG_SKIP_ANY_MS, 20000));
    // Far behind: skip anything to bound the delay.
    assert(live_play_should_skip(LIVE_LAG_SKIP_ANY_MS + 1, 20000));
}

static void test_farewell(void)
{
    assert(live_is_farewell("拜拜"));
    assert(live_is_farewell("好的，拜拜！"));
    assert(live_is_farewell("那就先这样吧，再见。"));
    assert(live_is_farewell("关闭吧"));
    assert(live_is_farewell("结束对话"));
    assert(live_is_farewell("OK, bye bye"));
    assert(live_is_farewell("不聊了，挂了"));
    // Questions that merely contain the words are not goodbyes.
    assert(!live_is_farewell("怎么关闭电脑的自动更新功能呢"));
    assert(!live_is_farewell("这部电影的结束部分是什么意思，能给我讲讲吗"));
    assert(!live_is_farewell("你好"));
    assert(!live_is_farewell(""));
    assert(!live_is_farewell("，。！"));
    assert(!live_is_farewell(NULL));
}

int main(void)
{
    test_farewell();
    test_latency_control();
    test_barge_window();
    test_autoclose();
    {
        live_config_t c;
        live_config_defaults(&c);
        live_wifi_upsert(&c, "Home", "password1");
        live_wifi_upsert(&c, "Phone", "password2");
        live_wifi_upsert(&c, "Office", "password3");
        assert(live_wifi_prefer(&c, "Office") && !strcmp(c.wifi[0].ssid, "Office"));
        assert(!strcmp(c.wifi[1].ssid, "Home") && !strcmp(c.wifi[2].ssid, "Phone") && c.wifi_count == 3);
        assert(live_wifi_prefer(&c, "Office") && !strcmp(c.wifi[0].ssid, "Office"));   // already first
        assert(!live_wifi_prefer(&c, "Nope"));
    }
    assert(!live_should_power_off(1000 + LIVE_POWER_OFF_MS - 1, 1000, false));
    assert(live_should_power_off(1000 + LIVE_POWER_OFF_MS, 1000, false));
    assert(!live_should_power_off(1000 + LIVE_POWER_OFF_MS, 1000, true));   // computer attached
    assert(!live_should_power_off(1000 + LIVE_POWER_OFF_MS, 0, false));     // in use
    test_volume_and_level();
    test_config();
    printf("test_live_policy: PASS\n");
    return 0;
}
