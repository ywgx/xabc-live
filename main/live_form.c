// main/live_form.c -- see live_form.h.
#include "live_form.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool live_form_get(const char *body, const char *name, char *out, size_t cap)
{
    if (!body || !name || !out || cap == 0) return false;
    const size_t nl = strlen(name);
    const char *p = body;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
            size_t o = 0;
            for (const char *s = p + nl + 1; s < end; s++) {
                char c = *s;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && s + 2 < end && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
                    c = (char)(hexval(s[1]) * 16 + hexval(s[2]));
                    s += 2;
                }
                if (o + 1 >= cap) return false;
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

bool live_html_append(char *out, size_t cap, size_t *len, const char *s)
{
    size_t o = *len;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        const size_t k = rep ? strlen(rep) : 1;
        if (o + k + 1 > cap) {
            out[*len] = '\0';
            return false;
        }
        if (rep) {
            memcpy(out + o, rep, k);
        } else {
            out[o] = *s;
        }
        o += k;
    }
    out[o] = '\0';
    *len = o;
    return true;
}

size_t live_dns_answer(const unsigned char *q, size_t qlen, const unsigned char ip[4],
                       unsigned char *out, size_t cap)
{
    if (qlen < 12 || qlen > 512) return 0;
    if (q[2] & 0x80) return 0;                       // a response, not a query
    if (((q[4] << 8) | q[5]) != 1) return 0;         // exactly one question
    size_t p = 12;                                   // walk the QNAME
    while (p < qlen && q[p] != 0) {
        if (q[p] & 0xC0) return 0;                   // no compression in questions
        p += (size_t)q[p] + 1;
    }
    if (p + 5 > qlen) return 0;
    const size_t qend = p + 5;                       // zero byte + QTYPE + QCLASS
    const int qtype = (q[p + 1] << 8) | q[p + 2];
    const bool answer = qtype == 1;                  // A record; others: empty reply
    const size_t need = qend + (answer ? 16 : 0);
    if (need > cap) return 0;
    memcpy(out, q, qend);
    out[2] = 0x81;                                   // QR, RD copied semantics
    out[3] = 0x80;                                   // RA, NOERROR
    out[6] = 0;
    out[7] = answer ? 1 : 0;                         // ANCOUNT
    out[8] = out[9] = out[10] = out[11] = 0;         // NSCOUNT, ARCOUNT
    if (!answer) return qend;
    unsigned char *a = out + qend;
    const unsigned char rr[12] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
    memcpy(a, rr, sizeof(rr));                       // name ptr, A, IN, TTL 60, len 4
    memcpy(a + 12, ip, 4);
    return need;
}

static size_t lead_space(const unsigned char *p)
{
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') return 1;
    if (p[0] == 0xC2 && p[1] == 0xA0) return 2;                        // U+00A0
    if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3;        // U+3000
    return 0;
}

void live_text_trim(char *s)
{
    if (!s) return;
    size_t start = 0, k;
    while ((k = lead_space((const unsigned char *)s + start)) > 0) start += k;
    size_t n = strlen(s + start);
    memmove(s, s + start, n + 1);
    // Trailing: walk back over ASCII spaces, U+00A0 and U+3000.
    for (;;) {
        n = strlen(s);
        if (n >= 1 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
            s[n - 1] = '\0';
        } else if (n >= 2 && (unsigned char)s[n - 2] == 0xC2 && (unsigned char)s[n - 1] == 0xA0) {
            s[n - 2] = '\0';
        } else if (n >= 3 && (unsigned char)s[n - 3] == 0xE3 && (unsigned char)s[n - 2] == 0x80 &&
                   (unsigned char)s[n - 1] == 0x80) {
            s[n - 3] = '\0';
        } else {
            break;
        }
    }
}

void live_text_to_ascii(char *s)
{
    if (!s) return;
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    while (*r) {
        if (r[0] >= 0xE0 && r[0] < 0xF0 && r[1] && r[2]) {
            const unsigned cp = ((r[0] & 0x0Fu) << 12) | ((r[1] & 0x3Fu) << 6) | (r[2] & 0x3Fu);
            int a = -1;
            if (cp >= 0xFF01 && cp <= 0xFF5E) a = (int)(cp - 0xFEE0);   // full-width ASCII
            else if (cp == 0x3000) a = ' ';
            else if (cp == 0x3002) a = '.';
            else if (cp == 0x3001) a = ',';
            else if (cp == 0x201C || cp == 0x201D) a = '"';
            else if (cp == 0x2018 || cp == 0x2019) a = '\'';
            else if (cp == 0x3010) a = '[';
            else if (cp == 0x3011) a = ']';
            else if (cp == 0x300A) a = '<';
            else if (cp == 0x300B) a = '>';
            else if (cp == 0x2014 || cp == 0x2013) a = '-';
            if (a >= 0) {
                *w++ = (unsigned char)a;
                r += 3;
                continue;
            }
            if (cp == 0x2026) {   // … -> ...
                *w++ = '.';
                *w++ = '.';
                *w++ = '.';
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
    live_text_trim(s);
}

void live_text_strip_spaces(char *s)
{
    if (!s) return;
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r != ' ' && *r != '\t') *w++ = *r;
    }
    *w = '\0';
}
