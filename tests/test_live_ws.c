// Host tests for main/live_ws.c (WebSocket client framing).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "live_ws.h"

int main(void)
{
    const uint8_t mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
    uint8_t h[LIVE_WS_HEADER_MAX];
    // RFC 6455 5.7 example: masked "Hello".
    size_t n = live_ws_client_header(h, LIVE_WS_OP_TEXT, 5, mask);
    assert(n == 6 && h[0] == 0x81 && h[1] == 0x85 && memcmp(h + 2, mask, 4) == 0);
    uint8_t hello[5] = { 'H', 'e', 'l', 'l', 'o' };
    live_ws_mask(hello, 5, mask, 0);
    const uint8_t expect[5] = { 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    assert(memcmp(hello, expect, 5) == 0);
    // Masking in two pieces equals masking at once (offset continues the key).
    uint8_t a[8] = "abcdefg", b[8] = "abcdefg";
    live_ws_mask(a, 7, mask, 0);
    live_ws_mask(b, 3, mask, 0);
    live_ws_mask(b + 3, 4, mask, 3);
    assert(memcmp(a, b, 7) == 0);
    // 16-bit and 64-bit lengths.
    n = live_ws_client_header(h, LIVE_WS_OP_TEXT, 4318, mask);
    assert(n == 8 && h[1] == (0x80 | 126) && h[2] == 0x10 && h[3] == 0xDE);
    n = live_ws_client_header(h, LIVE_WS_OP_BIN, 70000, mask);
    assert(n == 14 && h[1] == (0x80 | 127) && h[7] == 0x01 && h[8] == 0x11 && h[9] == 0x70);

    // Server frames: unmasked text "Hi", incomplete, 126-length, too large, masked, ping.
    live_ws_frame_t f;
    const uint8_t hi[] = { 0x81, 0x02, 'H', 'i', 0x89 };
    assert(live_ws_parse(hi, 1, 100, &f) == 0);
    assert(live_ws_parse(hi, 3, 100, &f) == 0);
    assert(live_ws_parse(hi, 5, 100, &f) == 1 && f.opcode == LIVE_WS_OP_TEXT && f.fin &&
           f.header_len == 2 && f.payload_len == 2);
    uint8_t big[4 + 300] = { 0x01, 126, 0x01, 0x2C };   // non-FIN text, 300 bytes
    assert(live_ws_parse(big, 4 + 299, 8192, &f) == 0);
    assert(live_ws_parse(big, sizeof(big), 8192, &f) == 1 && !f.fin && f.header_len == 4 &&
           f.payload_len == 300);
    assert(live_ws_parse(big, sizeof(big), 200, &f) == -1);
    const uint8_t masked[] = { 0x81, 0x82, 1, 2, 3, 4, 0, 0 };
    assert(live_ws_parse(masked, sizeof(masked), 100, &f) == -1);
    const uint8_t rsv[] = { 0xC1, 0x00 };
    assert(live_ws_parse(rsv, 2, 100, &f) == -1);
    const uint8_t ping[] = { 0x89, 0x00 };
    assert(live_ws_parse(ping, 2, 100, &f) == 1 && f.opcode == LIVE_WS_OP_PING && f.payload_len == 0);

    // Handshake request and response handling.
    char req[512];
    n = live_ws_handshake_request(req, sizeof(req), "api.xsirouter.com", "/v1/live/sessions",
                                  "dGhlIHNhbXBsZSBub25jZQ==", "Authorization: Bearer sk-test\r\n");
    assert(n == strlen(req));
    assert(strncmp(req, "GET /v1/live/sessions HTTP/1.1\r\nHost: api.xsirouter.com\r\n", 57) == 0);
    assert(strstr(req, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"));
    assert(strstr(req, "Authorization: Bearer sk-test\r\n\r\n") == req + n - strlen("Authorization: Bearer sk-test\r\n\r\n"));
    assert(live_ws_handshake_request(req, 20, "h", "/", "k", NULL) == 0);
    const char resp[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n\x81\x02Hi";
    size_t hl = live_ws_response_head_len(resp, sizeof(resp) - 1);
    assert(hl == sizeof(resp) - 1 - 4);
    assert(live_ws_response_head_len(resp, 20) == 0);
    int status;
    assert(live_ws_response_ok(resp, hl, &status) && status == 101);
    const char denied[] = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    assert(!live_ws_response_ok(denied, sizeof(denied) - 1, &status) && status == 401);
    assert(!live_ws_response_ok("garbage", 7, &status) && status == 0);
    printf("test_live_ws: PASS\n");
    return 0;
}
