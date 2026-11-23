// main/live_ws.h -- minimal RFC 6455 WebSocket client framing (pure C, host-tested).
//
// Used by app_live.c's single network task instead of esp_websocket_client, whose
// task holds a client lock while waiting for the rest of a partially received frame;
// on a jittery link that starves the uplink sender ("Could not lock ws-client") and,
// because gpt-live-1 paces its output by the input audio, makes replies stutter.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_WS_OP_CONT  0x0
#define LIVE_WS_OP_TEXT  0x1
#define LIVE_WS_OP_BIN   0x2
#define LIVE_WS_OP_CLOSE 0x8
#define LIVE_WS_OP_PING  0x9
#define LIVE_WS_OP_PONG  0xA

#define LIVE_WS_HEADER_MAX 14   // 2 + 8 (64-bit length) + 4 (mask)

// Writes a client frame header (FIN set, masked) for `payload_len` bytes; returns its size.
size_t live_ws_client_header(uint8_t *out, uint8_t opcode, size_t payload_len, const uint8_t mask[4]);

// XORs data with the mask; `offset` is the payload position of data[0].
void live_ws_mask(uint8_t *data, size_t n, const uint8_t mask[4], size_t offset);

typedef struct {
    uint8_t opcode;
    bool fin;
    size_t header_len;
    size_t payload_len;
} live_ws_frame_t;

// Examines buffered bytes. Returns 1 and fills *f when a whole frame is present,
// 0 when more bytes are needed, -1 for a frame this client cannot accept (masked
// server frame, reserved bits, or payload larger than max_payload).
int live_ws_parse(const uint8_t *buf, size_t len, size_t max_payload, live_ws_frame_t *f);

// Builds the HTTP upgrade request. key_b64 is the 24-character Sec-WebSocket-Key;
// extra_headers is inserted verbatim (each line ending in \r\n) or may be NULL.
// Returns the request length, or 0 if out_cap is too small.
size_t live_ws_handshake_request(char *out, size_t out_cap, const char *host, const char *path,
                                 const char *key_b64, const char *extra_headers);

// Length of a complete HTTP response head ending in \r\n\r\n, or 0 if not complete yet.
size_t live_ws_response_head_len(const char *buf, size_t len);

// True when the response head is "HTTP/1.1 101" (switching protocols). *status gets
// the numeric status code when it can be parsed (else 0).
bool live_ws_response_ok(const char *head, size_t len, int *status);
