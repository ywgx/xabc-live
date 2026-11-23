// main/live_g711.c -- see live_g711.h (classic Sun g711.c algorithm, BIAS 0x84, CLIP 32635).
#include "live_g711.h"

#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635

uint8_t live_ulaw_encode(int16_t sample)
{
    int s = sample;
    int sign = (s >> 8) & 0x80;
    if (sign) s = -s;
    if (s > ULAW_CLIP) s = ULAW_CLIP;
    s += ULAW_BIAS;
    int exponent = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exponent > 0; mask >>= 1) exponent--;
    int mantissa = (s >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

int16_t live_ulaw_decode(uint8_t code)
{
    code = (uint8_t)~code;
    int sign = code & 0x80;
    int exponent = (code >> 4) & 0x07;
    int mantissa = code & 0x0F;
    int s = ((mantissa << 3) + ULAW_BIAS) << exponent;
    s -= ULAW_BIAS;
    return (int16_t)(sign ? -s : s);
}

void live_ulaw_encode_buf(const int16_t *pcm, size_t count, uint8_t *out)
{
    for (size_t i = 0; i < count; i++) out[i] = live_ulaw_encode(pcm[i]);
}

void live_ulaw_decode_buf(const uint8_t *in, size_t count, int16_t *pcm)
{
    for (size_t i = 0; i < count; i++) pcm[i] = live_ulaw_decode(in[i]);
}
