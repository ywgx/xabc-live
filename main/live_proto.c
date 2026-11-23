// main/live_proto.c -- see live_proto.h. Pure C, host-tested by tests/test_live_proto.c.
#include "live_proto.h"

#include <stdio.h>
#include <string.h>

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Returns the index just past a JSON string that starts at json[i] == '"'.
static size_t skip_string(const char *json, size_t len, size_t i)
{
    for (i++; i < len; i++) {
        if (json[i] == '\\') {
            i++;
        } else if (json[i] == '"') {
            return i + 1;
        }
    }
    return len;
}

bool live_proto_find_string(const char *json, size_t len, const char *key, int depth,
                            const char **value, size_t *value_len)
{
    if (!json || !key || !value || !value_len) return false;
    const size_t key_len = strlen(key);
    int level = 0;
    size_t i = 0;
    while (i < len) {
        char c = json[i];
        if (c == '{' || c == '[') {
            level++;
            i++;
        } else if (c == '}' || c == ']') {
            level--;
            i++;
        } else if (c == '"') {
            const size_t start = i + 1;
            const size_t end = skip_string(json, len, i);   // index past closing quote
            i = end;
            // Is this string an object key? Look for ':' after optional whitespace.
            size_t j = end;
            while (j < len && is_ws(json[j])) j++;
            if (j >= len || json[j] != ':') continue;
            const bool match = (depth == 0 || level == depth) && end >= start + 1 &&
                               end - 1 - start == key_len &&
                               memcmp(json + start, key, key_len) == 0;
            j++;
            while (j < len && is_ws(json[j])) j++;
            if (!match) {
                i = j;
                continue;
            }
            if (j >= len || json[j] != '"') {
                i = j;
                continue;   // same key with a non-string value; keep searching
            }
            const size_t vend = skip_string(json, len, j);
            if (vend > len || vend < j + 2 || json[vend - 1] != '"') return false;
            *value = json + j + 1;
            *value_len = vend - j - 2;
            return true;
        } else {
            i++;
        }
    }
    return false;
}

live_event_type_t live_proto_event_type(const char *json, size_t len)
{
    const char *v;
    size_t n;
    if (!live_proto_find_string(json, len, "type", 1, &v, &n)) return LIVE_EV_UNKNOWN;
    static const struct {
        const char *name;
        live_event_type_t type;
    } map[] = {
        { "session.output_audio.delta", LIVE_EV_OUTPUT_AUDIO_DELTA },
        { "session.input_transcript.delta", LIVE_EV_INPUT_TRANSCRIPT_DELTA },
        { "session.output_transcript.delta", LIVE_EV_OUTPUT_TRANSCRIPT_DELTA },
        { "session.started", LIVE_EV_SESSION_STARTED },
        { "session.closed", LIVE_EV_SESSION_CLOSED },
        { "session.usage.updated", LIVE_EV_USAGE_UPDATED },
        { "error", LIVE_EV_ERROR },
        { "session.delegation.created", LIVE_EV_DELEGATION_CREATED },
    };
    for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
        if (strlen(map[k].name) == n && memcmp(map[k].name, v, n) == 0) return map[k].type;
    }
    return LIVE_EV_UNKNOWN;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(const char *p, size_t avail, uint32_t *out)
{
    if (avail < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hex_value(p[k]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Length of the UTF-8 sequence introduced by lead byte c (1 for invalid bytes).
static size_t utf8_seq_len(unsigned char c)
{
    if (c >= 0xF0 && c <= 0xF4) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC2 && c < 0xE0) return 2;
    return 1;
}

size_t live_proto_unescape(const char *raw, size_t raw_len, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    size_t o = 0;
    size_t i = 0;
    while (i < raw_len) {
        char enc[4];
        size_t n = 0;
        size_t consumed = 1;
        char c = raw[i];
        if (c != '\\') {
            // Copy a whole UTF-8 sequence at once so truncation never splits it.
            n = utf8_seq_len((unsigned char)c);
            if (i + n > raw_len) break;
            memcpy(enc, raw + i, n);
            consumed = n;
        } else if (i + 1 < raw_len) {
            char e = raw[i + 1];
            consumed = 2;
            n = 1;
            switch (e) {
            case 'n': enc[0] = '\n'; break;
            case 't': enc[0] = '\t'; break;
            case 'r': enc[0] = '\r'; break;
            case 'b': enc[0] = '\b'; break;
            case 'f': enc[0] = '\f'; break;
            case '/': enc[0] = '/'; break;
            case '"': enc[0] = '"'; break;
            case '\\': enc[0] = '\\'; break;
            case 'u': {
                uint32_t cp;
                if (!parse_hex4(raw + i + 2, raw_len - (i + 2), &cp)) break;
                consumed = 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (i + 12 <= raw_len && raw[i + 6] == '\\' && raw[i + 7] == 'u' &&
                        parse_hex4(raw + i + 8, raw_len - (i + 8), &lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        consumed = 12;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                n = utf8_encode(cp, enc);
                break;
            }
            default: enc[0] = e; break;
            }
            if (e == 'u' && consumed == 2) break;   // malformed \u escape
        } else {
            break;   // dangling backslash
        }
        if (o + n + 1 > out_cap) break;   // keep room for NUL; stop at a char boundary
        memcpy(out + o, enc, n);
        o += n;
        i += consumed;
    }
    out[o] = '\0';
    return o;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t live_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap)
{
    const size_t need = ((len + 2) / 3) * 4;
    if (!out || out_cap < need + 1) return 0;
    size_t o = 0;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int live_base64_decode(const char *in, size_t len, uint8_t *out, size_t out_cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t pad = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') {
            pad++;
            continue;
        }
        if (c == '\\' && i + 1 < len && in[i + 1] == '/') {
            continue;   // JSON may escape '/' as "\/"; the '/' itself is decoded next
        }
        int v = b64_value(c);
        if (v < 0 || pad) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (pad > 2) return -1;
    return (int)o;
}

size_t live_proto_audio_append_size(size_t pcm_bytes)
{
    static const size_t overhead = sizeof("{\"type\":\"session.input_audio.append\",\"audio\":\"\"}");
    return overhead + ((pcm_bytes + 2) / 3) * 4 + 1;
}

size_t live_proto_build_audio_append(const uint8_t *pcm, size_t pcm_bytes, char *out,
                                     size_t out_cap)
{
    static const char head[] = "{\"type\":\"session.input_audio.append\",\"audio\":\"";
    static const char tail[] = "\"}";
    if (!out || out_cap < live_proto_audio_append_size(pcm_bytes)) return 0;
    memcpy(out, head, sizeof(head) - 1);
    size_t o = sizeof(head) - 1;
    size_t n = live_base64_encode(pcm, pcm_bytes, out + o, out_cap - o);
    if (n == 0 && pcm_bytes) return 0;
    o += n;
    memcpy(out + o, tail, sizeof(tail));   // includes NUL
    return o + sizeof(tail) - 1;
}

// Appends `s` as JSON string content (without quotes). Returns false when full.
static bool append_escaped(char *out, size_t cap, size_t *o, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char buf[7];
        size_t n;
        if (*p == '"' || *p == '\\') {
            buf[0] = '\\';
            buf[1] = (char)*p;
            n = 2;
        } else if (*p == '\n') {
            memcpy(buf, "\\n", 2);
            n = 2;
        } else if (*p < 0x20) {
            n = (size_t)snprintf(buf, sizeof(buf), "\\u%04x", *p);
        } else {
            buf[0] = (char)*p;
            n = 1;
        }
        if (*o + n + 1 > cap) return false;
        memcpy(out + *o, buf, n);
        *o += n;
    }
    return true;
}

static bool append_raw(char *out, size_t cap, size_t *o, const char *s)
{
    size_t n = strlen(s);
    if (*o + n + 1 > cap) return false;
    memcpy(out + *o, s, n);
    *o += n;
    return true;
}

size_t live_proto_build_session_start(const char *format_type, int rate, const char *voice,
                                      const char *instructions, char *out, size_t out_cap)
{
    if (!out || out_cap == 0 || rate <= 0 || !format_type || !format_type[0]) return 0;
    char rate_buf[16];
    snprintf(rate_buf, sizeof(rate_buf), "%d", rate);
    size_t o = 0;
    bool ok = append_raw(out, out_cap, &o,
                         "{\"type\":\"session.start\",\"session\":{\"model\":\"gpt-live-1\"");
    if (ok && instructions && instructions[0]) {
        ok = append_raw(out, out_cap, &o, ",\"instructions\":\"") &&
             append_escaped(out, out_cap, &o, instructions) &&
             append_raw(out, out_cap, &o, "\"");
    }
    ok = ok && append_raw(out, out_cap, &o, ",\"audio\":{\"format\":{\"type\":\"") &&
         append_escaped(out, out_cap, &o, format_type) && append_raw(out, out_cap, &o, "\",\"rate\":") &&
         append_raw(out, out_cap, &o, rate_buf) && append_raw(out, out_cap, &o, "}");
    if (ok && voice && voice[0]) {
        ok = append_raw(out, out_cap, &o, ",\"output\":{\"voice\":\"") &&
             append_escaped(out, out_cap, &o, voice) && append_raw(out, out_cap, &o, "\"}");
    }
    ok = ok && append_raw(out, out_cap, &o, "}}}");
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    out[o] = '\0';
    return o;
}

size_t live_proto_build_append(const char *type, const char *event_id, const char *delegation_id,
                               const char *content, char *out, size_t out_cap)
{
    if (!out || out_cap == 0 || !type || !type[0] || !event_id || !content) return 0;
    size_t o = 0;
    bool ok = append_raw(out, out_cap, &o, "{\"type\":\"") && append_escaped(out, out_cap, &o, type) &&
              append_raw(out, out_cap, &o, "\",\"event_id\":\"") && append_escaped(out, out_cap, &o, event_id) &&
              append_raw(out, out_cap, &o, "\",\"delegation_id\":");
    if (ok && delegation_id && delegation_id[0]) {
        ok = append_raw(out, out_cap, &o, "\"") && append_escaped(out, out_cap, &o, delegation_id) &&
             append_raw(out, out_cap, &o, "\"");
    } else {
        ok = ok && append_raw(out, out_cap, &o, "null");
    }
    ok = ok && append_raw(out, out_cap, &o, ",\"content\":\"") && append_escaped(out, out_cap, &o, content) &&
         append_raw(out, out_cap, &o, "\"}");
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    out[o] = '\0';
    return o;
}

size_t live_proto_build_close(char *out, size_t out_cap)
{
    static const char msg[] = "{\"type\":\"session.close\"}";
    if (!out || out_cap < sizeof(msg)) return 0;
    memcpy(out, msg, sizeof(msg));
    return sizeof(msg) - 1;
}
