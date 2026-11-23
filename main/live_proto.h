// main/live_proto.h -- GPT-Live (OpenAI Live API, gpt-live-1) WebSocket message helpers.
//
// Pure C with no ESP-IDF or LVGL dependency so it can be covered by host tests.
// Protocol reference: XSI Router "gpt-live-1 接入指南" (wss://<host>/v1/live/sessions):
//   client -> server: session.start, session.input_audio.append, session.close
//   server -> client: session.started, session.output_audio.delta,
//                     session.input_transcript.delta, session.output_transcript.delta,
//                     session.usage.updated, session.closed, error,
//                     session.delegation.created (answered with session.commentary.append:
//                     the device has no lookup service and the model must not wait)
// Audio is Base64 PCM16 (audio/pcm) or G.711 (audio/pcmu, 8 kHz); this app uses pcmu.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LIVE_EV_UNKNOWN = 0,
    LIVE_EV_SESSION_STARTED,
    LIVE_EV_SESSION_CLOSED,
    LIVE_EV_OUTPUT_AUDIO_DELTA,
    LIVE_EV_INPUT_TRANSCRIPT_DELTA,
    LIVE_EV_OUTPUT_TRANSCRIPT_DELTA,
    LIVE_EV_USAGE_UPDATED,
    LIVE_EV_ERROR,
    LIVE_EV_DELEGATION_CREATED,
} live_event_type_t;

// Finds the first string value of `key`. depth 1 searches only top-level object
// members; depth 0 searches every nesting level. On success *value points at the
// raw (still escaped) characters between the quotes.
bool live_proto_find_string(const char *json, size_t len, const char *key, int depth,
                            const char **value, size_t *value_len);

// Classifies a server event by its top-level "type".
live_event_type_t live_proto_event_type(const char *json, size_t len);

// Decodes JSON string escapes (including \uXXXX surrogate pairs) to UTF-8.
// Always NUL-terminates when out_cap > 0 and truncates only at a UTF-8 character
// boundary. Returns the number of bytes written, excluding the terminator.
size_t live_proto_unescape(const char *raw, size_t raw_len, char *out, size_t out_cap);

// Standard Base64 (RFC 4648) with padding. Encode returns the output length
// (excluding the NUL it writes) or 0 when out_cap is too small.
size_t live_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap);
// Returns decoded bytes or -1 on malformed input / insufficient space. Decoding in
// place (out == (uint8_t *)in) is supported because the output never overtakes the input.
int live_base64_decode(const char *in, size_t len, uint8_t *out, size_t out_cap);

// Output size needed for live_proto_build_audio_append() for `pcm_bytes` of audio.
size_t live_proto_audio_append_size(size_t pcm_bytes);
// {"type":"session.input_audio.append","audio":"<base64>"}; returns length or 0.
size_t live_proto_build_audio_append(const uint8_t *pcm, size_t pcm_bytes, char *out,
                                     size_t out_cap);
// session.start with model gpt-live-1, audio format `format_type` ("audio/pcm",
// "audio/pcmu", "audio/pcma") at `rate`, voice and instructions (JSON-escaped here).
// Returns length or 0 when out_cap is too small.
size_t live_proto_build_session_start(const char *format_type, int rate, const char *voice,
                                      const char *instructions, char *out, size_t out_cap);
// {"type":"<type>","event_id":"<event_id>","delegation_id":"<id>","content":"<content>"}
// for session.commentary.append / session.thinking.append / session.instructions.append.
// delegation_id NULL or "" sends null. Returns length or 0 when out_cap is too small.
size_t live_proto_build_append(const char *type, const char *event_id, const char *delegation_id,
                               const char *content, char *out, size_t out_cap);
// {"type":"session.close"}; returns length or 0.
size_t live_proto_build_close(char *out, size_t out_cap);
