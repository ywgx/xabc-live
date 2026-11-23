// Host tests for main/live_proto.c (GPT-Live WebSocket message helpers).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "live_g711.h"
#include "live_proto.h"

#define S(x) x, (sizeof(x) - 1)

static void test_event_type(void)
{
    // Real server shapes observed on api.xairouter.com (payloads shortened).
    assert(live_proto_event_type(S("{\"type\":\"session.output_audio.delta\",\"delta\":\"AAAA\"}")) ==
           LIVE_EV_OUTPUT_AUDIO_DELTA);
    // session.started carries event_id first and nested "type" keys later.
    const char started[] =
        "{\"event_id\":\"event_1\",\"type\":\"session.started\",\"session\":{\"id\":\"live_x\","
        "\"audio\":{\"output\":{\"voice\":\"marin\"},\"format\":{\"type\":\"audio/pcm\",\"rate\":16000}}}}";
    assert(live_proto_event_type(started, sizeof(started) - 1) == LIVE_EV_SESSION_STARTED);
    // A nested "type" must not be mistaken for the top-level one.
    const char nested_first[] = "{\"error\":{\"type\":\"invalid_request_error\"},\"type\":\"error\"}";
    assert(live_proto_event_type(nested_first, sizeof(nested_first) - 1) == LIVE_EV_ERROR);
    assert(live_proto_event_type(S("{\"type\":\"session.input_transcript.delta\",\"delta\":\"x\"}")) ==
           LIVE_EV_INPUT_TRANSCRIPT_DELTA);
    assert(live_proto_event_type(S("{\"type\":\"session.output_transcript.delta\"}")) ==
           LIVE_EV_OUTPUT_TRANSCRIPT_DELTA);
    assert(live_proto_event_type(S("{\"type\":\"session.closed\",\"reason\":\"close_requested\"}")) ==
           LIVE_EV_SESSION_CLOSED);
    assert(live_proto_event_type(S("{\"type\":\"session.usage.updated\"}")) == LIVE_EV_USAGE_UPDATED);
    // Shape observed on api.xairouter.com: the id sits inside "delegation", after a
    // top-level event_id in some orders; "id" must not match "event_id".
    const char deleg[] =
        "{\"event_id\":\"event_E\",\"type\":\"session.delegation.created\",\"offset_ms\":2400,"
        "\"delegation\":{\"id\":\"item_EnHq\",\"type\":\"delegation\",\"target\":\"client\"}}";
    assert(live_proto_event_type(deleg, sizeof(deleg) - 1) == LIVE_EV_DELEGATION_CREATED);
    {
        const char *dv;
        size_t dn;
        assert(live_proto_find_string(deleg, sizeof(deleg) - 1, "id", 0, &dv, &dn));
        assert(dn == 9 && memcmp(dv, "item_EnHq", 9) == 0);
        assert(live_proto_find_string(deleg, sizeof(deleg) - 1, "target", 0, &dv, &dn) &&
               dn == 6 && memcmp(dv, "client", 6) == 0);
    }
    assert(live_proto_event_type(S("{\"type\":\"session.something_new\"}")) == LIVE_EV_UNKNOWN);
    assert(live_proto_event_type(S("{\"kind\":\"session.started\"}")) == LIVE_EV_UNKNOWN);
    assert(live_proto_event_type(S("{\"type\":")) == LIVE_EV_UNKNOWN);   // truncated
    assert(live_proto_event_type(S("")) == LIVE_EV_UNKNOWN);
    // Escaped quotes inside strings do not end the string early.
    assert(live_proto_event_type(S("{\"note\":\"a\\\"type\\\":\\\"error\",\"type\":\"session.closed\"}")) ==
           LIVE_EV_SESSION_CLOSED);
}

static void test_find_string(void)
{
    const char *v;
    size_t n;
    const char err[] =
        "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"code\":\"credit_balance_exhausted\","
        "\"message\":\"You have no credits\"}}";
    assert(live_proto_find_string(err, sizeof(err) - 1, "message", 0, &v, &n));
    assert(n == strlen("You have no credits") && memcmp(v, "You have no credits", n) == 0);
    assert(!live_proto_find_string(err, sizeof(err) - 1, "message", 1, &v, &n));   // nested only
    assert(live_proto_find_string(err, sizeof(err) - 1, "code", 2, &v, &n));
    assert(memcmp(v, "credit_balance_exhausted", n) == 0);
    // Non-string value for the key is skipped, a later string match is found.
    const char mixed[] = "{\"rate\":16000,\"x\":{\"rate\":\"fast\"}}";
    assert(live_proto_find_string(mixed, sizeof(mixed) - 1, "rate", 0, &v, &n));
    assert(n == 4 && memcmp(v, "fast", 4) == 0);
    // Value strings that look like keys are not keys.
    const char tricky[] = "{\"a\":\"delta\",\"delta\":\"ok\"}";
    assert(live_proto_find_string(tricky, sizeof(tricky) - 1, "delta", 1, &v, &n));
    assert(n == 2 && memcmp(v, "ok", 2) == 0);
    // Unterminated value fails cleanly.
    assert(!live_proto_find_string(S("{\"delta\":\"abc"), "delta", 1, &v, &n));
    assert(live_proto_find_string(S("{ \"delta\" : \"\" }"), "delta", 1, &v, &n) && n == 0);
}

static void test_unescape(void)
{
    char out[64];
    // Real server transcript delta: plain UTF-8 Chinese with an ASCII comma.
    const char raw[] = "你好,我是";
    size_t n = live_proto_unescape(raw, sizeof(raw) - 1, out, sizeof(out));
    assert(n == sizeof(raw) - 1 && strcmp(out, raw) == 0);
    // \uXXXX escapes, including a surrogate pair (U+1F600).
    n = live_proto_unescape(S("\\u4f60\\u597d\\n\\\"q\\\" \\ud83d\\ude00"), out, sizeof(out));
    assert(strcmp(out, "你好\n\"q\" \xF0\x9F\x98\x80") == 0);
    assert(n == strlen(out));
    // A lone surrogate becomes U+FFFD instead of invalid UTF-8.
    live_proto_unescape(S("a\\udc00b"), out, sizeof(out));
    assert(strcmp(out, "a\xEF\xBF\xBD" "b") == 0);
    // Truncation keeps whole characters: 3-byte chars into a 7-byte buffer -> 2 chars.
    n = live_proto_unescape(S("你好吗"), out, 7);
    assert(n == 6 && strcmp(out, "你好") == 0);
    n = live_proto_unescape(S("你好吗"), out, 6);
    assert(n == 3 && strcmp(out, "你") == 0);
    // Malformed \u stops without overrun.
    n = live_proto_unescape(S("ok\\u12"), out, sizeof(out));
    assert(n == 2 && strcmp(out, "ok") == 0);
    assert(live_proto_unescape(S("x"), out, 1) == 0 && out[0] == '\0');
}

static void test_base64(void)
{
    char enc[32];
    uint8_t dec[32];
    const struct { const char *plain; const char *b64; } cases[] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t len = strlen(cases[i].plain);
        size_t n = live_base64_encode((const uint8_t *)cases[i].plain, len, enc, sizeof(enc));
        assert(n == strlen(cases[i].b64) && strcmp(enc, cases[i].b64) == 0);
        int d = live_base64_decode(cases[i].b64, strlen(cases[i].b64), dec, sizeof(dec));
        assert(d == (int)len && memcmp(dec, cases[i].plain, len) == 0);
    }
    // Binary PCM round trip, decoded in place.
    uint8_t pcm[300];
    for (size_t i = 0; i < sizeof(pcm); i++) pcm[i] = (uint8_t)(i * 37 + 11);
    char big[512];
    size_t n = live_base64_encode(pcm, sizeof(pcm), big, sizeof(big));
    assert(n == 400);
    int d = live_base64_decode(big, n, (uint8_t *)big, sizeof(big));
    assert(d == (int)sizeof(pcm) && memcmp(big, pcm, sizeof(pcm)) == 0);
    // JSON-escaped slash "\/" is accepted.
    assert(live_base64_decode(S("\\/w=="), dec, sizeof(dec)) == 1 && dec[0] == 0xFF);
    // Errors: bad characters, data after padding, insufficient output, small encode buffer.
    assert(live_base64_decode(S("Zm9v!"), dec, sizeof(dec)) == -1);
    assert(live_base64_decode(S("Zg==Zg"), dec, sizeof(dec)) == -1);
    assert(live_base64_decode(S("Zm9vYmFy"), dec, 5) == -1);
    assert(live_base64_encode((const uint8_t *)"foo", 3, enc, 4) == 0);
}

static void test_builders(void)
{
    char buf[1024];
    size_t n = live_proto_build_close(buf, sizeof(buf));
    assert(n == strlen("{\"type\":\"session.close\"}") && strcmp(buf, "{\"type\":\"session.close\"}") == 0);
    assert(live_proto_build_close(buf, 10) == 0);

    const uint8_t pcm[] = { 0, 0, 0xFF, 0x7F };
    n = live_proto_build_audio_append(pcm, sizeof(pcm), buf, sizeof(buf));
    assert(strcmp(buf, "{\"type\":\"session.input_audio.append\",\"audio\":\"AAD/fw==\"}") == 0);
    assert(n == strlen(buf));
    assert(live_proto_audio_append_size(sizeof(pcm)) >= n + 1);
    assert(live_proto_build_audio_append(pcm, sizeof(pcm), buf, n) == 0);   // too small
    // 50 ms of 16 kHz PCM fits the size estimate exactly.
    static uint8_t chunk[1600];
    static char msg[2400];
    size_t need = live_proto_audio_append_size(sizeof(chunk));
    assert(need <= sizeof(msg));
    n = live_proto_build_audio_append(chunk, sizeof(chunk), msg, need);
    assert(n > 0 && n + 1 <= need);

    n = live_proto_build_session_start("audio/pcm", 16000, "marin", "你好\"引号\"\n第二行", buf, sizeof(buf));
    assert(n > 0 && n == strlen(buf));
    assert(strcmp(buf,
                  "{\"type\":\"session.start\",\"session\":{\"model\":\"gpt-live-1\","
                  "\"instructions\":\"你好\\\"引号\\\"\\n第二行\","
                  "\"audio\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":16000},"
                  "\"output\":{\"voice\":\"marin\"}}}}") == 0);
    // Parsing our own message back finds the top-level type.
    assert(live_proto_event_type(buf, n) == LIVE_EV_UNKNOWN);   // client event, not a server one
    const char *v;
    size_t vn;
    assert(live_proto_find_string(buf, n, "type", 1, &v, &vn) && memcmp(v, "session.start", vn) == 0);
    // Optional fields omitted.
    n = live_proto_build_session_start("audio/pcmu", 8000, "", NULL, buf, sizeof(buf));
    assert(strcmp(buf, "{\"type\":\"session.start\",\"session\":{\"model\":\"gpt-live-1\","
                       "\"audio\":{\"format\":{\"type\":\"audio/pcmu\",\"rate\":8000}}}}") == 0);
    n = live_proto_build_append("session.commentary.append", "ev1", "item_9", "新闻：\"A\"\n", buf, sizeof(buf));
    assert(n == strlen(buf) && strcmp(buf, "{\"type\":\"session.commentary.append\",\"event_id\":\"ev1\",\"delegation_id\":\"item_9\",\"content\":\"新闻：\\\"A\\\"\\n\"}") == 0);
    n = live_proto_build_append("session.thinking.append", "ev2", NULL, "", buf, sizeof(buf));
    assert(strcmp(buf, "{\"type\":\"session.thinking.append\",\"event_id\":\"ev2\",\"delegation_id\":null,\"content\":\"\"}") == 0);
    assert(live_proto_build_append("session.thinking.append", "ev2", NULL, "x", buf, 20) == 0 && buf[0] == '\0');
    assert(live_proto_build_session_start("audio/pcm", 16000, "marin", "x", buf, 40) == 0 && buf[0] == '\0');
    assert(live_proto_build_session_start("audio/pcm", 0, "marin", "x", buf, sizeof(buf)) == 0);
    assert(live_proto_build_session_start("", 8000, "marin", "x", buf, sizeof(buf)) == 0);
}

static void test_g711(void)
{
    // Reference points of the standard mu-law table.
    assert(live_ulaw_encode(0) == 0xFF);
    assert(live_ulaw_decode(0xFF) == 0);
    assert(live_ulaw_decode(0x7F) == 0);
    assert(live_ulaw_decode(0x00) == -32124);
    assert(live_ulaw_decode(0x80) == 32124);
    assert(live_ulaw_encode(32767) == 0x80 && live_ulaw_encode(-32768) == 0x00);
    // Every code decodes and re-encodes to itself (0x7F/-0 maps to +0 = 0xFF).
    for (int c = 0; c < 256; c++) {
        uint8_t back = live_ulaw_encode(live_ulaw_decode((uint8_t)c));
        assert(back == c || (c == 0x7F && back == 0xFF));
    }
    // Round-trip error stays within the segment step (~3 % of the magnitude).
    for (int v = -32768; v <= 32767; v += 37) {
        int d = live_ulaw_decode(live_ulaw_encode((int16_t)v));
        int err = d > v ? d - v : v - d;
        int mag = v < 0 ? -v : v;
        assert(err <= mag / 16 + 8);
    }
    const int16_t pcm[4] = { 0, 1000, -1000, 30000 };
    uint8_t enc[4];
    int16_t dec[4];
    live_ulaw_encode_buf(pcm, 4, enc);
    live_ulaw_decode_buf(enc, 4, dec);
    assert(dec[0] == 0 && dec[1] > 900 && dec[1] < 1100 && dec[2] < -900 && dec[3] > 28000);
}

int main(void)
{
    test_g711();
    test_event_type();
    test_find_string();
    test_unescape();
    test_base64();
    test_builders();
    printf("test_live_proto: PASS\n");
    return 0;
}
