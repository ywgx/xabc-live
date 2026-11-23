// main/live_form.h -- tiny helpers for the setup web page (pure C, host-tested).
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Extracts and URL-decodes field `name` from an application/x-www-form-urlencoded body
// ('+' -> space, %XX -> byte). Returns false if absent or longer than cap - 1 bytes.
bool live_form_get(const char *body, const char *name, char *out, size_t cap);

// Removes leading/trailing whitespace in place (ASCII, U+3000 ideographic space, U+00A0).
void live_text_trim(char *s);

// In place: full-width forms (U+FF01..U+FF5E, U+3000) and common Chinese punctuation
// (。、“”‘’【】《》—…) become their ASCII counterparts, so a password or key typed with a
// Chinese keyboard matches what the router / server expects. Then trims. Other
// non-ASCII text is kept.
void live_text_to_ascii(char *s);

// Removes every ASCII space (API keys never contain spaces).
void live_text_strip_spaces(char *s);

// Appends `s` HTML-escaped (& < > " ') to out at *len (out stays NUL-terminated).
// Returns false, leaving out unchanged, when it does not fit.
bool live_html_append(char *out, size_t cap, size_t *len, const char *s);

// DNS answer for the captive portal: given a query packet, writes a response that maps
// any A query to `ip` (network byte order bytes). Returns the response length, 0 to drop.
size_t live_dns_answer(const unsigned char *q, size_t qlen, const unsigned char ip[4],
                       unsigned char *out, size_t cap);
