// main/live_ws.c -- see live_ws.h.
#include "live_ws.h"

#include <stdio.h>
#include <string.h>

size_t live_ws_client_header(uint8_t *out, uint8_t opcode, size_t payload_len, const uint8_t mask[4])
{
    size_t n = 0;
    out[n++] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (payload_len < 126) {
        out[n++] = (uint8_t)(0x80 | payload_len);
    } else if (payload_len <= 0xFFFF) {
        out[n++] = 0x80 | 126;
        out[n++] = (uint8_t)(payload_len >> 8);
        out[n++] = (uint8_t)payload_len;
    } else {
        out[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) out[n++] = (uint8_t)((uint64_t)payload_len >> (8 * i));
    }
    memcpy(out + n, mask, 4);
    return n + 4;
}

void live_ws_mask(uint8_t *data, size_t n, const uint8_t mask[4], size_t offset)
{
    for (size_t i = 0; i < n; i++) data[i] ^= mask[(offset + i) & 3];
}

int live_ws_parse(const uint8_t *buf, size_t len, size_t max_payload, live_ws_frame_t *f)
{
    if (len < 2) return 0;
    if (buf[0] & 0x70) return -1;                 // RSV bits: no extensions negotiated
    if (buf[1] & 0x80) return -1;                 // servers must not mask
    size_t hl = 2;
    uint64_t pl = buf[1] & 0x7F;
    if (pl == 126) {
        if (len < 4) return 0;
        pl = ((uint64_t)buf[2] << 8) | buf[3];
        hl = 4;
    } else if (pl == 127) {
        if (len < 10) return 0;
        pl = 0;
        for (int i = 0; i < 8; i++) pl = (pl << 8) | buf[2 + i];
        hl = 10;
    }
    if (pl > max_payload) return -1;
    if (len < hl + pl) return 0;
    f->opcode = buf[0] & 0x0F;
    f->fin = (buf[0] & 0x80) != 0;
    f->header_len = hl;
    f->payload_len = (size_t)pl;
    return 1;
}

size_t live_ws_handshake_request(char *out, size_t out_cap, const char *host, const char *path,
                                 const char *key_b64, const char *extra_headers)
{
    int n = snprintf(out, out_cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "User-Agent: XABC-Live/1\r\n"
                     "%s"
                     "\r\n",
                     path, host, key_b64, extra_headers ? extra_headers : "");
    return (n > 0 && (size_t)n < out_cap) ? (size_t)n : 0;
}

size_t live_ws_response_head_len(const char *buf, size_t len)
{
    for (size_t i = 3; i < len; i++) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
            return i + 1;
        }
    }
    return 0;
}

bool live_ws_response_ok(const char *head, size_t len, int *status)
{
    if (status) *status = 0;
    if (len < 12 || memcmp(head, "HTTP/1.", 7) != 0) return false;
    int code = 0;
    for (size_t i = 9; i < 12; i++) {
        if (head[i] < '0' || head[i] > '9') return false;
        code = code * 10 + (head[i] - '0');
    }
    if (status) *status = code;
    return code == 101;
}
