// Host tests for main/live_form.c (setup page helpers).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "live_form.h"

int main(void)
{
    char v[64];
    const char body[] = "ssid=My+Home%21&pass=p%40ss+w%2Bd&key=sk-abc_123&empty=";
    assert(live_form_get(body, "ssid", v, sizeof(v)) && strcmp(v, "My Home!") == 0);
    assert(live_form_get(body, "pass", v, sizeof(v)) && strcmp(v, "p@ss w+d") == 0);
    assert(live_form_get(body, "key", v, sizeof(v)) && strcmp(v, "sk-abc_123") == 0);
    assert(live_form_get(body, "empty", v, sizeof(v)) && v[0] == '\0');
    assert(!live_form_get(body, "ss", v, sizeof(v)));                 // prefix is not a match
    assert(!live_form_get(body, "nope", v, sizeof(v)));
    assert(!live_form_get(body, "ssid", v, 4));                       // too long for cap
    assert(live_form_get("a=%E4%BD%A0%E5%A5%BD", "a", v, sizeof(v)) && strcmp(v, "你好") == 0);
    assert(live_form_get("a=100%", "a", v, sizeof(v)) && strcmp(v, "100%") == 0);   // bad escape kept

    char h[32];
    size_t n = 0;
    h[0] = '\0';
    assert(live_html_append(h, sizeof(h), &n, "a<b>&\"'") && strcmp(h, "a&lt;b&gt;&amp;&quot;&#39;") == 0);
    assert(!live_html_append(h, sizeof(h), &n, "0123456789") && n == strlen(h));   // unchanged

    // DNS: query for "a.b" type A -> answer 192.168.4.1.
    const unsigned char q[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                1, 'a', 1, 'b', 0, 0, 1, 0, 1 };
    const unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char r[64];
    size_t rl = live_dns_answer(q, sizeof(q), ip, r, sizeof(r));
    assert(rl == sizeof(q) + 16);
    assert(r[0] == 0x12 && r[1] == 0x34 && (r[2] & 0x80) && r[7] == 1);
    assert(memcmp(r + rl - 4, ip, 4) == 0);
    unsigned char q6[sizeof(q)];
    memcpy(q6, q, sizeof(q));
    q6[sizeof(q6) - 3] = 28;                                          // AAAA: empty answer
    rl = live_dns_answer(q6, sizeof(q6), ip, r, sizeof(r));
    assert(rl == sizeof(q6) && r[7] == 0);
    assert(live_dns_answer(q, 8, ip, r, sizeof(r)) == 0);             // truncated
    assert(live_dns_answer(q, sizeof(q), ip, r, 10) == 0);            // no room

    // Normalization of passwords / keys typed on a Chinese keyboard.
    char t[64];
    strcpy(t, "  12345678，abcd \t");
    live_text_to_ascii(t);
    assert(strcmp(t, "12345678,abcd") == 0);
    strcpy(t, "\xE3\x80\x80ＡＢＣ１２３！＠＃\xE3\x80\x80");   // full-width space, letters, digits, symbols
    live_text_to_ascii(t);
    assert(strcmp(t, "ABC123!@#") == 0);
    strcpy(t, "a。b、c“d”e‘f’【g】《h》i—j…");
    live_text_to_ascii(t);
    assert(strcmp(t, "a.b,c\"d\"e'f'[g]<h>i-j...") == 0);
    strcpy(t, "密码中文保留");                                         // other CJK unchanged
    live_text_to_ascii(t);
    assert(strcmp(t, "密码中文保留") == 0);
    strcpy(t, "  my wifi  ");
    live_text_trim(t);
    assert(strcmp(t, "my wifi") == 0);                                // inner space kept
    strcpy(t, "\xC2\xA0x\xC2\xA0");
    live_text_trim(t);
    assert(strcmp(t, "x") == 0);
    strcpy(t, "   ");
    live_text_trim(t);
    assert(t[0] == '\0');
    strcpy(t, " sk-ab cd\tef ");
    live_text_strip_spaces(t);
    assert(strcmp(t, "sk-abcdef") == 0);

    printf("test_live_form: PASS\n");
    return 0;
}
