// main/live_g711.h -- ITU-T G.711 mu-law codec (audio/pcmu) for the Live session.
// 8 kHz mu-law needs a quarter of the bandwidth of 16 kHz PCM16, which keeps the
// Base64 JSON stream within what one TLS WebSocket on the ESP32-C3 can carry.
// Pure C, host-tested by tests/test_live_proto.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

uint8_t live_ulaw_encode(int16_t sample);
int16_t live_ulaw_decode(uint8_t code);

void live_ulaw_encode_buf(const int16_t *pcm, size_t count, uint8_t *out);
void live_ulaw_decode_buf(const uint8_t *in, size_t count, int16_t *pcm);
