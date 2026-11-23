// main/app_live.c -- GPT-Live session controller.
//
// The session task owns the lifecycle; one network task (net_task) owns the TLS
// WebSocket and does all socket I/O:
//   wss://<host>/v1/live/sessions, header "Authorization: Bearer <key>"
//   -> session.start (gpt-live-1, audio/pcmu 8 kHz) -> session.started
//   -> session.input_audio.append every 50 ms (mic task, via uplink())
//   <- session.output_audio.delta / *_transcript.delta / usage / closed / error
//   -> session.close on OK long press, idle timeout, or the session cap.
// net_task reads non-blockingly, parses whole frames from s_in (no blocking on a
// partial frame), sends queued control messages and up to 3 uplink chunks per loop.
// Audio deltas are Base64-decoded in place. See live_ws.h for why this replaced
// esp_websocket_client.
#include "app.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "live_g711.h"
#include "live_policy.h"
#include "live_proto.h"
#include "live_ws.h"

#include "driver/usb_serial_jtag.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "lwip/sockets.h"
#include <fcntl.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_live";

#define RX_CAP           8192  // whole WebSocket frames (largest seen: session.started ~2 KB)
#define CONNECT_ATTEMPTS 3
// Uplink queue holds mu-law; HD mode expands each 100 ms chunk to PCM16 when sending.
#define UPLINK_CHUNK_MAX 800   // 100 ms of 8 kHz mu-law
#define STALL_MS        6000   // no downlink at all for this long: connection is dead
#define UPLINK_MAX_PER_DRAIN 3 // chunks per drain call (downlink ~10 msg/s -> up to 30 chunks/s)

#define EV_CONNECTED    BIT0
#define EV_STARTED      BIT1
#define EV_CLOSED       BIT2   // session.closed received
#define EV_DISCONNECTED BIT3
#define EV_ERROR        BIT4

static QueueHandle_t s_cmds;
static EventGroupHandle_t s_ev;
static StreamBufferHandle_t s_up_buf;        // mic mu-law waiting to be sent (read by net_task)
static uint8_t *s_up_frame;                  // LIVE_WS_HEADER_MAX headroom + JSON message
static char *s_up_msg;                       // = s_up_frame + LIVE_WS_HEADER_MAX
static size_t s_up_msg_cap;
static esp_tls_t *s_tls;                     // owned by net_task
static TaskHandle_t s_net;
static volatile bool s_net_run;
static SemaphoreHandle_t s_net_exit;         // given when net_task has released everything
static uint8_t *s_in;                        // raw received bytes (whole frames), RX_CAP
static size_t s_in_len;
static char *s_frag;                         // only for fragmented messages (rare)
static size_t s_frag_len;
static bool s_frag_drop;
// Control messages (session.start / appends / close) for net_task. Each is a heap copy
// with LIVE_WS_HEADER_MAX bytes of headroom that net_task frees after sending, so a
// caller that gives up waiting can never leave net_task reading freed memory.
typedef struct {
    uint8_t *frame;
    size_t len;
    uint32_t seq;                            // matches the completion to its sender
} ctrl_msg_t;
static QueueHandle_t s_ctrl_q;
static SemaphoreHandle_t s_ctrl_done;        // given once per sent message
static SemaphoreHandle_t s_ctrl_lock;        // one sender at a time
static volatile bool s_ctrl_ok;
static volatile uint32_t s_ctrl_done_seq;    // seq of the message s_ctrl_ok belongs to
static uint32_t s_ctrl_seq;
static char s_uri_host[LIVE_CFG_HOST_MAX + 1];
static int s_uri_port;
static char s_auth[LIVE_CFG_KEY_MAX + 32];   // "Authorization: Bearer <key>\r\n", wiped after use
static volatile bool s_streaming;
static volatile bool s_session_busy;   // connecting or live: no other TLS work (RAM)
static volatile int64_t s_last_activity_ms;

// Diagnostics, printed every 2 s while a session runs (no payloads, no secrets).
static struct {
    uint32_t rx_events, rx_msgs, rx_overflow, audio_deltas, audio_bytes, audio_bad;
    uint32_t in_tx, out_tx, other, up_chunks, up_fail, up_drop;
    uint32_t rd_calls, rd_bytes, rd_want, loops;
} s_st;
static int s_logged_events;
static char s_error[128];
#define UTTER_GAP_MS 1200
static char s_utter[256];                 // the user's current/last utterance
static int64_t s_utter_ms;                // time of its last transcript piece
static int64_t s_ai_text_ms;              // time of the assistant's last transcript piece
static volatile int64_t s_bye_ms;         // user said goodbye at this time: close after the reply
static volatile bool s_bye_replied;       // the assistant has spoken since the goodbye
static bool s_volume_dirty;

void app_post(app_cmd_t cmd)
{
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

// ---------------------------------------------------------------------------
// Downlink (runs in net_task; must stay non-blocking)
// ---------------------------------------------------------------------------

static size_t uplink_chunk(void)
{
    return APP_SAMPLE_RATE / 10;   // 100 ms of mu-law
}

static void handle_transcript(const char *json, int len, bool user)
{
    const char *v;
    size_t n;
    if (!live_proto_find_string(json, (size_t)len, "delta", 1, &v, &n)) return;
    char text[256];
    live_proto_unescape(v, n, text, sizeof(text));
    if (!text[0]) return;
    s_last_activity_ms = app_now_ms();   // resets the 5-minute idle close
    if (g_debug) ESP_LOGI(TAG, "%s: %s", user ? "you" : "ai", text);
    const int64_t now = app_now_ms();
    if (!user) {
        s_ai_text_ms = now;
        if (s_bye_ms) s_bye_replied = true;   // the assistant is saying goodbye
        return;
    }
    // One utterance = user transcript pieces without a pause over UTTER_GAP_MS. In full
    // duplex the assistant often starts replying before the user's words are complete,
    // so the reply cannot mark the end of the utterance.
    if (now - s_utter_ms > UTTER_GAP_MS) s_utter[0] = '\0';
    s_utter_ms = now;
    if (strlen(s_utter) + strlen(text) >= sizeof(s_utter)) s_utter[0] = '\0';   // keep the latest words
    strlcat(s_utter, text, sizeof(s_utter));
    // A short goodbye ("拜拜", "关闭吧") ends the conversation once the reply has been
    // spoken; more words (a longer sentence, or talking on) cancel it.
    if (live_is_farewell(s_utter)) {
        if (!s_bye_ms) {
            s_bye_ms = now;
            // In full duplex the goodbye reply may already be under way.
            s_bye_replied = s_ai_text_ms && now - s_ai_text_ms < 2000;
            ESP_LOGI(TAG, "用户告别，回答完后结束对话");
        }
    } else {
        s_bye_ms = 0;
    }
}

static bool send_copy(uint8_t opcode, const void *data, size_t n, int timeout_ms);

// Answers an unwanted delegation (runs in net_task, which owns the socket).
static void answer_delegation(const char *id)
{
    static const char text[] = "这里没有联网查询，直接用你已有的知识回答用户；不确定就如实说。";
    char msg[sizeof(text) * 2 + 192];
    static uint32_t s_seq;
    char ev[24];
    snprintf(ev, sizeof(ev), "deleg_%u", (unsigned)++s_seq);
    const size_t n = live_proto_build_append("session.commentary.append", ev, id, text, msg, sizeof(msg));
    if (n == 0 || !send_copy(LIVE_WS_OP_TEXT, msg, n, 3000)) ESP_LOGW(TAG, "回复委派失败");
}

static void handle_message(char *json, int len)
{
    const char *v;
    size_t n;
    s_st.rx_msgs++;
    const live_event_type_t type = live_proto_event_type(json, (size_t)len);
    if (g_debug && type != LIVE_EV_OUTPUT_AUDIO_DELTA && s_logged_events < 12) {
        s_logged_events++;
        ESP_LOGI(TAG, "event[%d] %.*s", len, len > 180 ? 180 : len, json);
    }
    switch (type) {
    case LIVE_EV_OUTPUT_AUDIO_DELTA:
        s_st.audio_deltas++;
        if (live_proto_find_string(json, (size_t)len, "delta", 1, &v, &n)) {
            int bytes = live_base64_decode(v, n, (uint8_t *)json, (size_t)len);   // in place
            if (bytes > 0) {
                s_st.audio_bytes += (uint32_t)bytes;
                app_audio_play_ulaw((const uint8_t *)json, (size_t)bytes);
            } else {
                s_st.audio_bad++;
            }
        } else {
            s_st.audio_bad++;
        }
        break;
    case LIVE_EV_INPUT_TRANSCRIPT_DELTA:
        s_st.in_tx++;
        handle_transcript(json, len, true);
        break;
    case LIVE_EV_OUTPUT_TRANSCRIPT_DELTA:
        s_st.out_tx++;
        handle_transcript(json, len, false);
        break;
    case LIVE_EV_SESSION_STARTED:
        ESP_LOGI(TAG, "session.started");
        xEventGroupSetBits(s_ev, EV_STARTED);
        break;
    case LIVE_EV_SESSION_CLOSED:
        if (live_proto_find_string(json, (size_t)len, "reason", 1, &v, &n)) {
            ESP_LOGI(TAG, "session.closed reason=%.*s", (int)n, v);
        }
        xEventGroupSetBits(s_ev, EV_CLOSED);
        break;
    case LIVE_EV_USAGE_UPDATED:
        if (g_debug) ESP_LOGI(TAG, "usage: %.*s", len > 160 ? 160 : len, json);
        break;
    case LIVE_EV_DELEGATION_CREATED:
        // This device has no lookup service. gpt-live-1 may still delegate (it cannot be
        // switched off) and would then wait forever: tell it at once to answer by itself.
        if (live_proto_find_string(json, (size_t)len, "id", 0, &v, &n) && n < 64) {
            char id[64];
            memcpy(id, v, n);
            id[n] = '\0';
            s_last_activity_ms = app_now_ms();
            ESP_LOGW(TAG, "模型请求委派查询（设备不支持），让它直接回答");
            answer_delegation(id);
        }
        break;
    case LIVE_EV_ERROR:
        if (live_proto_find_string(json, (size_t)len, "message", 0, &v, &n)) {
            live_proto_unescape(v, n, s_error, sizeof(s_error));
        } else {
            snprintf(s_error, sizeof(s_error), "服务端返回错误");
        }
        ESP_LOGE(TAG, "error event: %.*s", len > 300 ? 300 : len, json);
        xEventGroupSetBits(s_ev, EV_ERROR);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Network task: the only code that touches the TLS connection
// ---------------------------------------------------------------------------

static bool want_io(int r)
{
    return r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE;
}

static int sock_fd(void)
{
    int fd = -1;
    if (s_tls) esp_tls_get_conn_sockfd(s_tls, &fd);
    return fd;
}

// Writes everything or fails; waits for socket writability for at most timeout_ms.
static bool tls_write_all(const uint8_t *d, size_t n, int timeout_ms)
{
    const int64_t deadline = app_now_ms() + timeout_ms;
    while (n > 0) {
        int r = esp_tls_conn_write(s_tls, d, n);
        if (r > 0) {
            d += r;
            n -= (size_t)r;
            continue;
        }
        if (!want_io(r) || app_now_ms() > deadline) return false;
        // Wait for the direction mbedTLS asked for (WANT_READ needs readability).
        fd_set fs;
        FD_ZERO(&fs);
        int fd = sock_fd();
        FD_SET(fd, &fs);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        const int64_t t0 = esp_timer_get_time();
        const int sr = r == ESP_TLS_ERR_SSL_WANT_READ ? select(fd + 1, &fs, NULL, NULL, &tv)
                                                       : select(fd + 1, NULL, &fs, NULL, &tv);
        if (sr < 0) return false;
        if (esp_timer_get_time() - t0 < 2000) vTaskDelay(1);   // never spin
    }
    return true;
}

// Sends one masked client frame. buf has LIVE_WS_HEADER_MAX bytes of headroom before
// the payload, which is masked in place (the caller's copy is consumed).
static volatile bool s_net_broken;   // a frame write failed: the stream is unusable

static bool ws_send(uint8_t opcode, uint8_t *buf, size_t payload_len, int timeout_ms)
{
    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    uint8_t hdr[LIVE_WS_HEADER_MAX];
    size_t hl = live_ws_client_header(hdr, opcode, payload_len, mask);
    uint8_t *start = buf + LIVE_WS_HEADER_MAX - hl;
    memcpy(start, hdr, hl);
    live_ws_mask(buf + LIVE_WS_HEADER_MAX, payload_len, mask, 0);
    // A partly written frame cannot be resumed after another one: end the connection.
    if (!tls_write_all(start, hl + payload_len, timeout_ms)) s_net_broken = true;
    return !s_net_broken;
}

static bool send_copy(uint8_t opcode, const void *data, size_t n, int timeout_ms)
{
    uint8_t *buf = malloc(n + LIVE_WS_HEADER_MAX);
    if (!buf) return false;
    if (n) memcpy(buf + LIVE_WS_HEADER_MAX, data, n);
    bool ok = ws_send(opcode, buf, n, timeout_ms);
    free(buf);
    return ok;
}

// Up to max_chunks queued 100 ms uplink chunks; never blocks on an empty queue.
static void send_uplink(int max_chunks)
{
    static uint8_t ulaw[UPLINK_CHUNK_MAX];
    const size_t chunk = uplink_chunk();
    for (int i = 0; i < max_chunks && s_streaming && xStreamBufferBytesAvailable(s_up_buf) >= chunk; i++) {
        if (xStreamBufferReceive(s_up_buf, ulaw, chunk, 0) != chunk) break;
        size_t n = live_proto_build_audio_append(ulaw, chunk, s_up_msg, s_up_msg_cap);
        if (n == 0 || !ws_send(LIVE_WS_OP_TEXT, s_up_frame, n, 4000)) {   // failure ends the connection
            s_st.up_fail++;
            return;
        }
        s_st.up_chunks++;
    }
}

// Handles one complete frame at s_in. Returns false when the connection must close.
static bool on_frame(const live_ws_frame_t *f)
{
    uint8_t *payload = s_in + f->header_len;
    s_st.rx_events++;
    switch (f->opcode) {
    case LIVE_WS_OP_TEXT:
    case LIVE_WS_OP_CONT:
        if (f->opcode == LIVE_WS_OP_TEXT && f->fin && !s_frag) {
            handle_message((char *)payload, (int)f->payload_len);   // common case: in place
            return true;
        }
        if (f->opcode == LIVE_WS_OP_TEXT) {
            s_frag_len = 0;
            s_frag_drop = false;
        }
        if (s_frag_drop) {   // rest of an oversized message: skip through its FIN
            if (f->fin) s_frag_drop = false;
            return true;
        }
        if (!s_frag) s_frag = malloc(RX_CAP);
        if (!s_frag || s_frag_len + f->payload_len >= RX_CAP) {
            s_st.rx_overflow++;
            free(s_frag);
            s_frag = NULL;
            s_frag_len = 0;
            s_frag_drop = !f->fin;
            return true;   // drop an oversized fragmented message, keep the session
        }
        memcpy(s_frag + s_frag_len, payload, f->payload_len);
        s_frag_len += f->payload_len;
        if (f->fin) {
            s_frag[s_frag_len] = '\0';
            handle_message(s_frag, (int)s_frag_len);
            free(s_frag);
            s_frag = NULL;
        }
        return true;
    case LIVE_WS_OP_PING:
        send_copy(LIVE_WS_OP_PONG, payload, f->payload_len, 1000);
        return true;
    case LIVE_WS_OP_CLOSE:
        if (f->payload_len >= 2) {   // status code + optional UTF-8 reason
            ESP_LOGW(TAG, "server closed the WebSocket: %u %.*s", (unsigned)((payload[0] << 8) | payload[1]),
                     (int)(f->payload_len - 2 > 120 ? 120 : f->payload_len - 2), (const char *)payload + 2);
        } else {
            ESP_LOGW(TAG, "server closed the WebSocket");
        }
        return false;
    default:
        s_st.other++;
        return true;
    }
}

// TLS connect + HTTP upgrade. Leftover bytes after the response head go to s_in.
static bool ws_connect(void)
{
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    s_tls = esp_tls_init();
    if (!s_tls) return false;
    if (esp_tls_conn_new_sync(s_uri_host, (int)strlen(s_uri_host), s_uri_port, &cfg, s_tls) != 1) {
        snprintf(s_error, sizeof(s_error), "TLS 连接失败");
        return false;
    }
    uint8_t keyraw[16];
    char key[25];
    esp_fill_random(keyraw, sizeof(keyraw));
    live_base64_encode(keyraw, sizeof(keyraw), key, sizeof(key));
    char *req = malloc(1024);
    if (!req) return false;
    size_t n = live_ws_handshake_request(req, 1024, s_uri_host, "/v1/live/sessions", key, s_auth);
    bool ok = n > 0 && tls_write_all((const uint8_t *)req, n, 5000);
    memset(req, 0, 1024);   // contained the API key
    free(req);
    memset(s_auth, 0, sizeof(s_auth));
    if (!ok) return false;
    // Read the response head (blocking reads, 10 s socket timeout, 12 s overall).
    s_in_len = 0;
    size_t head = 0;
    const int64_t head_deadline = app_now_ms() + 12000;
    while (!head && s_in_len < RX_CAP && s_net_run && app_now_ms() < head_deadline) {
        int r = esp_tls_conn_read(s_tls, s_in + s_in_len, RX_CAP - s_in_len);
        if (r <= 0 && !want_io(r)) return false;
        if (r > 0) s_in_len += (size_t)r;
        head = live_ws_response_head_len((const char *)s_in, s_in_len);
    }
    int status = 0;
    if (!head || !live_ws_response_ok((const char *)s_in, head, &status)) {
        snprintf(s_error, sizeof(s_error), "握手失败 HTTP %d", status);
        return false;
    }
    memmove(s_in, s_in + head, s_in_len - head);
    s_in_len -= head;
    int fd = sock_fd();
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);   // net_task never blocks on I/O
    // Disable Nagle: each 100 ms uplink frame leaves at once and carries the ACK for the
    // server's last audio delta. With Nagle here plus lwIP's delayed ACK, the server's own
    // Nagle releases one small delta per RTT + ACK delay -> 2-5 deltas/s and stutter.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

// True when a whole TLS record is waiting on the socket (or TLS already holds
// decrypted bytes). mbedTLS dynamic buffers break if a read starts on a record that
// has only partly arrived (it then stalls or fails with -0x7200), so net_task only
// reads complete records; the socket stays non-blocking for writes.
// Returns 1 when a record can be read, 0 when not yet, -1 when the peer closed TCP.
static int tls_record_ready(void)
{
    if (esp_tls_get_bytes_avail(s_tls) > 0) return 1;
    int fd = sock_fd();
    uint8_t h[5];
    int n = recv(fd, h, sizeof(h), MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return -1;              // FIN: the socket stays "readable" forever
    if (n < 0) return 0;
    int avail = 0;
    if (ioctl(fd, FIONREAD, &avail) != 0) return 1;
    // lwIP's MSG_PEEK stops at the first queued packet: if that holds only part of the
    // 5-byte header, the peek never grows. Read anyway once more bytes are queued.
    if (n < (int)sizeof(h)) return avail >= (int)sizeof(h) ? 1 : 0;
    const int need = 5 + ((h[3] << 8) | h[4]);
    // A record larger than the TCP window could never be fully queued; read it anyway.
    return (avail >= need || need > 12000) ? 1 : 0;
}

static void net_task(void *arg)
{
    (void)arg;
    s_net_broken = false;
    ctrl_msg_t stale;
    while (xQueueReceive(s_ctrl_q, &stale, 0) == pdTRUE) free(stale.frame);   // from an old session
    const bool ok = ws_connect();
    xEventGroupSetBits(s_ev, ok ? EV_CONNECTED : EV_DISCONNECTED);
    int streak = 0;   // consecutive iterations without waiting
    while (ok && s_net_run && !s_net_broken) {
        bool busy = false;
        s_st.loops++;
        // 1) Read complete TLS records only; WebSocket frames may still span records.
        const int ready = s_in_len < RX_CAP ? tls_record_ready() : 0;
        if (ready < 0) {
            ESP_LOGW(TAG, "服务器关闭了 TCP 连接");
            break;
        }
        if (ready > 0) {
            int r = esp_tls_conn_read(s_tls, s_in + s_in_len, RX_CAP - s_in_len);
            s_st.rd_calls++;
            if (r > 0) {
                s_st.rd_bytes += (uint32_t)r;
                s_in_len += (size_t)r;
                busy = true;
            } else if (want_io(r)) {
                s_st.rd_want++;
            } else {
                ESP_LOGW(TAG, "连接已断开 (%d)", r);
                break;
            }
        }
        // 2) Handle every complete frame.
        bool alive = true;
        for (;;) {
            live_ws_frame_t f;
            int pr = live_ws_parse(s_in, s_in_len, RX_CAP - LIVE_WS_HEADER_MAX, &f);
            if (pr == 0) {
                if (s_in_len >= RX_CAP) {
                    ESP_LOGE(TAG, "接收缓冲已满但帧未完整（缓冲 %u 字节）", (unsigned)s_in_len);
                    alive = false;
                }
                break;
            }
            if (pr < 0) {
                ESP_LOGE(TAG, "无法解析的 WebSocket 帧 (首字节 %02x %02x %02x %02x)", s_in[0], s_in[1],
                         s_in_len > 2 ? s_in[2] : 0, s_in_len > 3 ? s_in[3] : 0);
                alive = false;
                break;
            }
            alive = on_frame(&f);
            const size_t used = f.header_len + f.payload_len;
            memmove(s_in, s_in + used, s_in_len - used);
            s_in_len -= used;
            if (!alive) break;
        }
        if (!alive) break;
        // 3) Control message from the session task (session.start / session.close).
        ctrl_msg_t m;
        if (xQueueReceive(s_ctrl_q, &m, 0) == pdTRUE) {
            s_ctrl_ok = ws_send(LIVE_WS_OP_TEXT, m.frame, m.len, 3000);
            s_ctrl_done_seq = m.seq;
            free(m.frame);
            xSemaphoreGive(s_ctrl_done);
            busy = true;
        }
        // 4) Uplink at a steady pace (100 ms chunks).
        if (s_streaming && xStreamBufferBytesAvailable(s_up_buf) >= uplink_chunk()) {
            send_uplink(3);
            busy = true;
        }
        // 5) Idle: wait up to 20 ms for the socket (TLS may already hold decrypted bytes).
        //    When the socket is readable but only part of a TLS record has arrived, select()
        //    returns at once and the loop would spin until the rest trickles in (seconds on
        //    a slow link: this task outranks IDLE, so the task watchdog rebooted the device).
        //    Sleep instead, and always yield after a long busy streak.
        if (!busy && esp_tls_get_bytes_avail(s_tls) <= 0) {
            int fd = sock_fd();
            int pending = 0;
            if (ioctl(fd, FIONREAD, &pending) == 0 && pending > 0) {
                vTaskDelay(pdMS_TO_TICKS(10));   // partial record: wait for the rest
            } else {
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(fd, &rf);
                struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
                const int64_t t0 = esp_timer_get_time();
                select(fd + 1, &rf, NULL, NULL, &tv);
                // Readable yet nothing was done this round (error state, odd socket):
                // never let this task (above IDLE and the controller) spin.
                if (esp_timer_get_time() - t0 < 2000) vTaskDelay(pdMS_TO_TICKS(10));
            }
            streak = 0;
        } else if (++streak >= 50) {
            streak = 0;
            vTaskDelay(1);
        }
    }
    xEventGroupSetBits(s_ev, EV_DISCONNECTED);
    ctrl_msg_t m;
    while (xQueueReceive(s_ctrl_q, &m, 0) == pdTRUE) {   // never leave a sender waiting
        free(m.frame);
        s_ctrl_ok = false;
        s_ctrl_done_seq = m.seq;
        xSemaphoreGive(s_ctrl_done);
    }
    if (s_tls) esp_tls_conn_destroy(s_tls);
    s_tls = NULL;
    free(s_frag);
    s_frag = NULL;
    xSemaphoreGive(s_net_exit);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Uplink (runs in the mic task: queue only, never blocks)
// ---------------------------------------------------------------------------

static bool uplink(const uint8_t *ulaw, size_t bytes)
{
    if (!s_streaming) return false;
    bool ok = xStreamBufferSend(s_up_buf, ulaw, bytes, 0) == bytes;
    if (!ok) s_st.up_drop++;
    return ok;
}

// Hands a copy of a text message to net_task and waits until it has been written.
// Safe from any task (serialized); returns false if the session is not connected.
static bool send_text(const char *msg, size_t n)
{
    if (!s_net || !(xEventGroupGetBits(s_ev) & EV_CONNECTED) ||
        (xEventGroupGetBits(s_ev) & EV_DISCONNECTED)) {
        return false;
    }
    ctrl_msg_t m = { .frame = malloc(n + LIVE_WS_HEADER_MAX), .len = n };
    if (!m.frame) return false;
    memcpy(m.frame + LIVE_WS_HEADER_MAX, msg, n);
    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    m.seq = ++s_ctrl_seq;
    bool ok = false;
    if (xQueueSend(s_ctrl_q, &m, 0) != pdTRUE) {
        free(m.frame);
    } else {
        // Accept only the completion of this message; a late one from an earlier
        // timed-out send is skipped. On timeout net_task still owns (and frees) the frame.
        const int64_t deadline = app_now_ms() + 10000;
        for (;;) {
            const int64_t left = deadline - app_now_ms();
            if (left <= 0 || xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(left)) != pdTRUE) break;
            if (s_ctrl_done_seq == m.seq) {
                ok = s_ctrl_ok;
                break;
            }
        }
    }
    xSemaphoreGive(s_ctrl_lock);
    return ok;
}

// ---------------------------------------------------------------------------
// Session lifecycle (session task only)
// ---------------------------------------------------------------------------

static void teardown(void)
{
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // back to power saving between conversations
    s_session_busy = false;
    s_streaming = false;
    app_audio_set_uplink(NULL);
    app_audio_enable(false);
    if (s_net) {
        s_net_run = false;
        // net_task may be inside a blocking TLS connect / handshake read (up to ~20 s).
        // Freeing its buffers while it runs would corrupt memory: wait, and restart as
        // a last resort instead.
        if (xSemaphoreTake(s_net_exit, pdMS_TO_TICKS(25000)) != pdTRUE) {
            ESP_LOGE(TAG, "网络任务 25 秒未退出，重启");
            esp_restart();
        }
        s_net = NULL;
    }
    ctrl_msg_t m;
    while (xQueueReceive(s_ctrl_q, &m, 0) == pdTRUE) free(m.frame);   // never sent to a later session
    free(s_in);
    s_in = NULL;
    s_in_len = 0;
    memset(s_auth, 0, sizeof(s_auth));
    app_set_session_start(0);
    if (s_volume_dirty) {
        s_volume_dirty = false;
        app_model_lock();
        const int v = app_model()->volume;
        app_model_unlock();
        app_config_save_volume(v);   // deferred until playback has stopped
    }
    ESP_LOGI(TAG, "会话已释放, free heap=%u", (unsigned)esp_get_free_heap_size());
}

// lwIP seeds its ephemeral TCP port from esp_random() before RF is up, which on the
// ESP32-C3 repeats across reboots. A reused 5-tuple collides with the router's still-
// ESTABLISHED conntrack entry from the previous boot and the SYN is dropped as invalid
// (18 s connect timeout). Once Wi-Fi is up the RNG is truly random: skip ahead a
// random number of ports, once per boot.
static void randomize_local_port(void)
{
    static bool done;
    if (done) return;
    done = true;
    const int skips = 64 + (int)(esp_random() % 1024);
    for (int i = 0; i < skips; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) break;
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY) };
        bind(s, (struct sockaddr *)&a, sizeof(a));   // port 0: lwIP allocates the next ephemeral port
        close(s);
    }
    ESP_LOGI(TAG, "skipped %d ephemeral ports", skips);
}

bool app_live_busy(void)
{
    return s_session_busy;
}

static bool start_session_inner(void);

// OK presses made while a connection attempt is running would otherwise queue up and
// start another attempt right after this one ("keeps reconnecting"); drop them, keep
// volume presses.
static void drop_queued_toggles(void)
{
    app_cmd_t keep[8];
    int n = 0;
    app_cmd_t c;
    while (n < 8 && xQueueReceive(s_cmds, &c, 0) == pdTRUE) {
        if (c != APP_CMD_TOGGLE) keep[n++] = c;
    }
    for (int i = 0; i < n; i++) xQueueSend(s_cmds, &keep[i], 0);
}

static bool start_session(void)
{
    s_session_busy = true;
    bool ok = start_session_inner();
    s_session_busy = ok;   // stays busy while live; teardown clears it
    drop_queued_toggles();
    return ok;
}

static bool start_session_inner(void)
{
    live_config_t cfg;
    app_config_get(&cfg);
    if (!live_config_ready(&cfg)) {
        app_set_state(APP_ST_SETUP, "需要先配置 Wi-Fi 和 API Key");
        return false;
    }
    app_set_state(APP_ST_CONNECTING, "正在连接 Wi-Fi…");
    if (!app_wifi_wait(15000)) {
        app_set_state(APP_ST_ERROR, "Wi-Fi 未连接，按 OK 重试");
        return false;
    }
    randomize_local_port();
    app_set_state(APP_ST_CONNECTING, "正在检查网络…");
    if (!app_net_ensure(3)) {
        app_set_state(APP_ST_ERROR, "网络不通，按 OK 重试");
        return false;
    }
    app_set_state(APP_ST_CONNECTING, "正在连接 ChatGPT…");

    // host[:port] for TLS; the Authorization header is built per attempt and wiped.
    strlcpy(s_uri_host, cfg.host, sizeof(s_uri_host));
    s_uri_port = 443;
    char *colon = strchr(s_uri_host, ':');
    if (colon) {
        *colon = '\0';
        s_uri_port = atoi(colon + 1);
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi RSSI=%d dBm, free heap=%u", ap.rssi, (unsigned)esp_get_free_heap_size());
    }

    bool connected = false;
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS && !connected; attempt++) {
        if (attempt > 1) {
            char msg[64];
            snprintf(msg, sizeof(msg), "连接超时，第 %d 次重试…", attempt - 1);
            app_set_status(msg);
        }
        xEventGroupClearBits(s_ev, 0xFF);
        s_error[0] = '\0';
        s_in = malloc(RX_CAP);
        if (!s_in) {
            app_set_state(APP_ST_ERROR, "内存不足");
            return false;
        }
        s_in_len = 0;
        snprintf(s_auth, sizeof(s_auth), "Authorization: Bearer %s\r\n", cfg.api_key);
        const int64_t t0 = app_now_ms();
        s_net_run = true;
        xSemaphoreTake(s_net_exit, 0);
        if (xTaskCreate(net_task, "live_net", 6144, NULL, 5, &s_net) != pdPASS) {
            s_net = NULL;
            teardown();
            continue;
        }
        EventBits_t cb = xEventGroupWaitBits(s_ev, EV_CONNECTED | EV_DISCONNECTED, pdFALSE, pdFALSE,
                                             pdMS_TO_TICKS(30000));
        connected = (cb & EV_CONNECTED) && !(cb & EV_DISCONNECTED);
        ESP_LOGI(TAG, "第 %d 次连接%s，用时 %d ms", attempt, connected ? "成功" : "失败",
                 (int)(app_now_ms() - t0));
        if (!connected) {
            teardown();
            if (attempt < CONNECT_ATTEMPTS) app_net_ensure(2);   // clears a stale AP station state
        }
        if (!connected && strstr(s_error, "HTTP")) break;   // e.g. 401 will not succeed on retry
    }
    if (!connected) {
        char msg[160];
        snprintf(msg, sizeof(msg), "连接失败%s%s", s_error[0] ? "：" : "，按 OK 重试", s_error);
        app_set_state(APP_ST_ERROR, msg);
        return false;
    }

    // Instructions = configured prompt + two device facts: there are no tools (so never
    // delegate), and the speaker sits next to the microphone (its own echo is not the user).
    static const char device_rule[] =
        "\n\n你没有任何工具，不能联网或委派查询：所有问题都直接用已有知识回答，不确定就如实说。"
        "\n设备的喇叭紧挨着麦克风，你可能会听到自己声音的回声：和你刚说的话相同的内容是回声，不是用户插话，"
        "继续说下去；只有用户说出新的内容时才停下来回应。";
    const size_t instr_cap = strlen(cfg.prompt) + sizeof(device_rule);
    char *instr = malloc(instr_cap);
    const size_t msg_cap = instr_cap * 2 + 256;
    char *start_msg = malloc(msg_cap);
    size_t n = 0;
    if (instr && start_msg) {
        snprintf(instr, instr_cap, "%s%s", cfg.prompt, device_rule);
        n = live_proto_build_session_start("audio/pcmu", APP_SAMPLE_RATE, cfg.voice, instr,
                                           start_msg, msg_cap);
    }
    const int64_t t_start = app_now_ms();
    const bool sent = n > 0 && send_text(start_msg, n);
    free(instr);
    free(start_msg);
    if (!sent) {
        teardown();
        app_set_state(APP_ST_ERROR, "发送 session.start 失败");
        return false;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_STARTED | EV_ERROR | EV_DISCONNECTED, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(20000));
    if (!(b & EV_STARTED)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "会话未开始：%s", s_error[0] ? s_error : "服务端无响应");
        teardown();
        app_set_state(APP_ST_ERROR, msg);
        return false;
    }

    // Modem sleep makes the AP hold downlink frames until each DTIM beacon, which turns
    // the 10 msg/s audio stream bursty (stutter) on APs with a DTIM period > 1.
    esp_wifi_set_ps(WIFI_PS_NONE);
    const int64_t now = app_now_ms();
    s_last_activity_ms = now;
    app_set_session_start(now);
    xStreamBufferReset(s_up_buf);
    memset(&s_st, 0, sizeof(s_st));   // also restarts the downlink stall watchdog
    s_logged_events = 0;
    s_bye_ms = 0;
    s_utter[0] = '\0';
    s_utter_ms = 0;
    s_ai_text_ms = 0;
    app_audio_set_uplink(uplink);
    app_audio_enable(true);
    s_streaming = true;
    app_set_state(APP_ST_LIVE, "我在听，请说话");
    ESP_LOGI(TAG, "session.start -> started %d ms", (int)(app_now_ms() - t_start));
    ESP_LOGI(TAG, "会话已开始, free heap=%u, largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

static void stop_session(const char *status)
{
    app_set_state(APP_ST_CLOSING, "正在结束…");
    s_streaming = false;
    app_audio_set_uplink(NULL);
    app_audio_enable(false);
    char msg[32];
    size_t n = live_proto_build_close(msg, sizeof(msg));
    if (send_text(msg, n)) {
        // Wait for session.closed so the server reports final usage.
        xEventGroupWaitBits(s_ev, EV_CLOSED | EV_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(4000));
    }
    teardown();
    app_set_state(APP_ST_IDLE, status);
}

static void change_volume(int direction)
{
    app_model_lock();
    int v = live_volume_step(app_model()->volume, direction);
    app_model_unlock();
    app_audio_set_volume(v);
    app_set_volume(v);
    // Avoid NVS writes while audio streams (Flash writes can stall I2S).
    if (s_net) {
        s_volume_dirty = true;
    } else {
        app_config_save_volume(v);
    }
}

static void go_ready(void)
{
    live_config_t cfg;
    app_config_get(&cfg);
    if (!live_config_ready(&cfg) && !app_wifi_pending()) {
        app_portal_start(NULL);   // first boot: set up from a phone
        return;
    }
    if (app_wifi_connected()) {
        app_set_state(APP_ST_IDLE, "按 OK 开始对话");
    } else {
        app_set_state(APP_ST_WIFI, "正在连接 Wi-Fi…");
    }
}

// OK click while the assistant talks: silence the speaker at once (flush playback and
// drop in-flight downlink audio briefly) so the user can speak. Nothing is sent to the
// model: the full-duplex model hears the user and decides itself whether to yield.
static void interrupt_assistant(void)
{
    app_audio_interrupt();
    app_set_status("已打断，请说");
    ESP_LOGI(TAG, "OK 打断：本地静音，等用户开口");
}

static void live_task(void *arg)
{
    (void)arg;
    go_ready();
    int64_t next_battery_ms = 0;
    for (;;) {
        if (app_now_ms() >= next_battery_ms) {
            g_battery_soc = bsp_battery_soc();   // I2C read kept out of the LVGL task
            next_battery_ms = app_now_ms() + 10000;
        }
        app_cmd_t cmd = 0;
        bool got = xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(250)) == pdTRUE;
        app_model_lock();
        const app_state_t st = app_model()->state;
        const bool speaking = app_model()->ai_speaking;
        app_model_unlock();

        // Auto power off: unused (no conversation, no key) for LIVE_POWER_OFF_MS.
        static int64_t s_idle_since;
        const bool in_use = got || st == APP_ST_CONNECTING || st == APP_ST_LIVE || st == APP_ST_CLOSING;
        if (in_use) {
            s_idle_since = 0;
        } else if (!s_idle_since) {
            s_idle_since = app_now_ms();
        }
        if (live_should_power_off(app_now_ms(), s_idle_since, usb_serial_jtag_is_connected())) {
            app_power_off("10 分钟未使用，已自动关机");
        }

        if (got) {
            switch (cmd) {
            case APP_CMD_TOGGLE:
                if (st == APP_ST_IDLE || st == APP_ST_ERROR || st == APP_ST_WIFI) {
                    start_session();
                } else if (st == APP_ST_LIVE) {
                    if (speaking || app_audio_recently_playing()) {
                        interrupt_assistant();
                    } else {
                        app_set_status("我在听，请说话");
                    }
                } else if (st == APP_ST_SETUP && app_portal_active()) {
                    esp_restart();   // leave setup without saving
                }
                break;
            case APP_CMD_STOP:
                if (st == APP_ST_LIVE) stop_session("已结束，按 OK 再聊");
                break;
            case APP_CMD_VOLUME_UP:
                change_volume(+1);
                break;
            case APP_CMD_VOLUME_DOWN:
                change_volume(-1);
                break;
            case APP_CMD_SETUP:
                if (s_net) stop_session("进入设置");
                app_portal_start(app_wifi_setup_error());
                break;
            case APP_CMD_POWER_OFF:
                if (s_net) stop_session("正在关机");
                app_power_off("已关机，按任意键开机");
                break;
            case APP_CMD_CONFIG_CHANGED:
                if (s_net) stop_session("配置已更新");
                app_wifi_reconfigure();
                go_ready();
                break;
            }
            continue;
        }

        // Periodic checks.
        static int64_t next_stats_ms;
        if (g_debug && st == APP_ST_LIVE && app_now_ms() >= next_stats_ms) {
            next_stats_ms = app_now_ms() + 2000;
            ESP_LOGI(TAG, "net rd=%u bytes=%u want=%u loops=%u in=%u", (unsigned)s_st.rd_calls,
                     (unsigned)s_st.rd_bytes, (unsigned)s_st.rd_want, (unsigned)s_st.loops, (unsigned)s_in_len);
            ESP_LOGI(TAG, "stats rx_ev=%u msgs=%u ovf=%u audio=%u/%uB bad=%u in_tx=%u out_tx=%u "
                     "other=%u up=%u fail=%u drop=%u mic=%d spk=%d underrun=%u spk_drop=%u skip=%u heap=%u",
                     (unsigned)s_st.rx_events, (unsigned)s_st.rx_msgs, (unsigned)s_st.rx_overflow,
                     (unsigned)s_st.audio_deltas, (unsigned)s_st.audio_bytes, (unsigned)s_st.audio_bad,
                     (unsigned)s_st.in_tx, (unsigned)s_st.out_tx, (unsigned)s_st.other,
                     (unsigned)s_st.up_chunks, (unsigned)s_st.up_fail, (unsigned)s_st.up_drop,
                     g_mic_level, g_spk_level, (unsigned)g_play_underruns, (unsigned)g_play_dropped, (unsigned)g_play_skipped,
                     (unsigned)esp_get_free_heap_size());
        }
        if (st == APP_ST_LIVE) {
            EventBits_t b = xEventGroupGetBits(s_ev);
            if (b & (EV_DISCONNECTED | EV_CLOSED)) {
                teardown();
                app_set_state(APP_ST_ERROR, s_error[0] ? s_error : "连接已断开，按 OK 重连");
                continue;
            }
            static uint32_t seen_msgs;
            static int64_t last_rx_ms;
            if (s_st.rx_msgs != seen_msgs || last_rx_ms == 0) {
                seen_msgs = s_st.rx_msgs;
                last_rx_ms = app_now_ms();
            } else if (app_now_ms() - last_rx_ms > STALL_MS) {
                ESP_LOGW(TAG, "下行 %d ms 无数据，判定连接中断", STALL_MS);
                last_rx_ms = 0;
                teardown();
                app_set_state(APP_ST_ERROR, "连接中断，按 OK 重连");
                continue;
            }
            if (b & EV_ERROR) {
                xEventGroupClearBits(s_ev, EV_ERROR);
                if (s_error[0]) ESP_LOGW(TAG, "服务端提示：%s", s_error);   // non-fatal unless it also closes
            }
            app_model_lock();
            const int64_t start = app_model()->session_start_ms;
            app_model_unlock();
            const live_close_reason_t r = live_should_autoclose(app_now_ms(), start, s_last_activity_ms);
            const int64_t bye = s_bye_ms;
            const int64_t since_bye = bye ? app_now_ms() - bye : 0;
            // The server streams silence too, so "done" = no audible speech for a moment.
            if (bye && ((s_bye_replied && since_bye > LIVE_FAREWELL_GRACE_MS && app_audio_ms_since_speech() > 1200) ||
                        since_bye > LIVE_FAREWELL_MAX_WAIT_MS)) {
                stop_session("已结束，按 OK 再聊");
            } else if (r == LIVE_CLOSE_IDLE) {
                stop_session("五分钟没有对话，已自动结束");
            } else if (r == LIVE_CLOSE_MAX_DURATION) {
                stop_session("单次会话已满 30 分钟，已结束");
            } else {
                app_model_lock();
                const bool spk = app_model()->ai_speaking;
                app_model_unlock();
                static bool last_spk;
                if (spk != last_spk) {
                    last_spk = spk;
                    app_set_status(spk ? "正在回答" : "我在听，请说话");
                }
            }
        } else if (st == APP_ST_WIFI && app_wifi_connected()) {
            app_set_state(APP_ST_WIFI, "正在检查网络…");
            const bool net_ok = app_net_ensure(3);
            app_set_state(net_ok ? APP_ST_IDLE : APP_ST_ERROR,
                          net_ok ? "按 OK 开始对话" : "网络不通，按 OK 重试");
            // First time the network is ready after power-on: go straight into a
            // conversation (it closes itself after LIVE_IDLE_CLOSE_MS of silence).
            // The IDLE branch below performs the one-time power-on auto start.
        } else if (st == APP_ST_IDLE && app_wifi_connected()) {
            static bool s_auto_started_idle;
            if (!s_auto_started_idle && app_auto_start_enabled()) {
                s_auto_started_idle = true;   // power-on: go straight into a conversation
                start_session();
                continue;
            }
        } else if (st == APP_ST_WIFI && app_wifi_failed_rounds() >= 2) {
            static int64_t s_hint_ms;
            if (app_now_ms() - s_hint_ms > 5000) {   // avoid redrawing every 250 ms
                s_hint_ms = app_now_ms();
                app_set_status("连不上 Wi-Fi，长按 ▲ 重新设置");
            }
        } else if (st == APP_ST_IDLE && !app_wifi_connected()) {
            app_set_state(APP_ST_WIFI, "正在连接 Wi-Fi…");
        }
    }
}

esp_err_t app_live_start(void)
{
    s_cmds = xQueueCreate(8, sizeof(app_cmd_t));
    s_ev = xEventGroupCreate();
    s_net_exit = xSemaphoreCreateBinary();
    s_ctrl_done = xSemaphoreCreateBinary();
    s_ctrl_q = xQueueCreate(4, sizeof(ctrl_msg_t));
    s_ctrl_lock = xSemaphoreCreateMutex();
    // Buffers sized for the boot-time audio mode so standard mode does not pay for HD.
    const size_t chunk = uplink_chunk();
    s_up_buf = xStreamBufferCreate(chunk * 5, 1);                       // 0.5 s backlog
    s_up_msg_cap = live_proto_audio_append_size(chunk);
    s_up_frame = malloc(s_up_msg_cap + LIVE_WS_HEADER_MAX);
    s_up_msg = s_up_frame ? (char *)s_up_frame + LIVE_WS_HEADER_MAX : NULL;
    if (!s_cmds || !s_ev || !s_net_exit || !s_ctrl_done || !s_ctrl_q || !s_ctrl_lock || !s_up_buf || !s_up_msg) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(live_task, "live_ctrl", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
