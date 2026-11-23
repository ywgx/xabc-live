// main/app_ui.c -- the GPT-Live companion screen (240 x 320, dark theme).
//
// Layout: title + Wi-Fi dot + battery (top, hidden during a conversation), a voice orb and
// one short status line. Deliberately minimal: no captions. The orb is a soft sky-blue
// gradient sphere with drifting light inside (in the style of ChatGPT's voice mode): it
// breathes while waiting, swells with your voice while listening and pulses faster while
// the assistant speaks. Grey-blue while connecting, red on errors.
// Rendering happens only in an LVGL timer (LVGL task context), which polls the
// shared model, so worker tasks never touch LVGL objects.
//
// Chinese text uses live_cjk_16 (GB2312 + punctuation, see tools/gen_live_font.py).
// Transcript characters outside that set render as LVGL placeholder boxes.
#include "app.h"
#include "bsp_display.h"

#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(live_cjk_16);

#define COL_BG      0x0B0F1A
#define COL_TEXT    0xE8ECF4
#define COL_DIM     0x7C879C
#define COL_GREEN   0x3DDC84
#define COL_RED     0xEF4444
#define COL_GRAY    0x5B6475

// Orb palettes (top -> bottom of the vertical gradient).
#define ORB_SKY_TOP    0xF2FAFF
#define ORB_SKY_BOTTOM 0x3E8EF7
#define ORB_MIST_TOP   0xEEF2F7
#define ORB_MIST_BOTTOM 0x8E9BB0
#define ORB_RED_TOP    0xFFE2E2
#define ORB_RED_BOTTOM 0xE5484D
#define ORB_BLOBS      3

#define ORB_CX      120
#define ORB_CY      150
#define ORB_BASE    128
#define QR_SIZE     140

static lv_obj_t *s_scr, *s_halo, *s_orb, *s_title_dot, *s_title, *s_battery;
static lv_obj_t *s_status;
static lv_obj_t *s_qr;            // setup hotspot QR code (in place of the orb)
static lv_obj_t *s_blob[ORB_BLOBS]; // soft light drifting inside the orb
static float s_phase;               // blob motion phase (advances faster while speaking)
static unsigned s_seen_version = ~0u;
static int s_last_volume = -1;
static int64_t s_volume_toast_until;
static app_state_t s_state;
static bool s_ai_speaking;
static int64_t s_session_start;
static float s_level_smooth;

static lv_obj_t *circle(lv_obj_t *parent, int size, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void orb_palette(uint32_t *top, uint32_t *bottom)
{
    switch (s_state) {
    case APP_ST_ERROR:
        *top = ORB_RED_TOP;
        *bottom = ORB_RED_BOTTOM;
        return;
    case APP_ST_SETUP:
    case APP_ST_WIFI:
    case APP_ST_CONNECTING:
    case APP_ST_CLOSING:
        *top = ORB_MIST_TOP;
        *bottom = ORB_MIST_BOTTOM;
        return;
    default:   // ready / listening / speaking
        *top = ORB_SKY_TOP;
        *bottom = ORB_SKY_BOTTOM;
        return;
    }
}

// A blob is a circle with a vertical gradient fading from `opa` to transparent: layered
// on the orb it reads as soft, cloud-like light rather than a hard disc.
static lv_obj_t *blob(lv_obj_t *parent, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = circle(parent, 40, color, LV_OPA_COVER);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(o, opa, 0);
    lv_obj_set_style_bg_grad_opa(o, LV_OPA_TRANSP, 0);
    return o;
}

static void render_model(void)
{
    char status[sizeof(((app_model_t *)0)->status)];
    int volume;
    app_model_lock();
    const app_model_t *m = app_model();
    if (m->version == s_seen_version) {
        app_model_unlock();
        return;
    }
    s_seen_version = m->version;
    s_state = m->state;
    s_ai_speaking = m->ai_speaking;
    s_session_start = m->session_start_ms;
    volume = m->volume;
    memcpy(status, m->status, sizeof(status));
    app_model_unlock();

    // During a conversation only the orb and the status line stay on screen.
    lv_obj_t *const top[] = { s_title_dot, s_title, s_battery };
    for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); i++) {
        if (s_state == APP_ST_LIVE) {
            lv_obj_add_flag(top[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(top[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Phone setup: the hotspot's Wi-Fi QR code replaces the orb (camera scan joins it,
    // then the setup page pops up by itself).
    const char *portal = app_portal_ssid();
    static char s_qr_ssid[33];
    if (s_state == APP_ST_SETUP && portal[0]) {
        if (strcmp(s_qr_ssid, portal) != 0) {
            strlcpy(s_qr_ssid, portal, sizeof(s_qr_ssid));
            char data[64];
            snprintf(data, sizeof(data), "WIFI:T:nopass;S:%s;;", portal);
            lv_qrcode_update(s_qr, data, (uint32_t)strlen(data));
        }
        lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_orb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_halo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_orb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_halo, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(s_status, lv_color_hex(s_state == APP_ST_ERROR ? COL_RED : COL_DIM), 0);
    uint32_t orb_top, orb_bottom;
    orb_palette(&orb_top, &orb_bottom);
    lv_obj_set_style_bg_color(s_orb, lv_color_hex(orb_top), 0);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(orb_bottom), 0);
    lv_obj_set_style_bg_color(s_halo, lv_color_hex(orb_bottom), 0);
    if (s_last_volume >= 0 && volume != s_last_volume) {
        s_volume_toast_until = app_now_ms() + 1500;
        lv_label_set_text_fmt(s_status, LV_SYMBOL_VOLUME_MID " 音量 %d%%", volume);
    } else if (app_now_ms() >= s_volume_toast_until) {
        lv_label_set_text(s_status, status);
    }
    s_last_volume = volume;
}

static void render_frame(void)
{
    const int64_t now = app_now_ms();
    // Orb size: breathing for waiting states, voice level while live.
    float target = 0.0f;
    if (s_state == APP_ST_LIVE) {
        target = (float)(s_ai_speaking ? g_spk_level : g_mic_level);
    }
    s_level_smooth += (target - s_level_smooth) * 0.35f;
    float breath = 0.0f;
    if (s_state == APP_ST_WIFI || s_state == APP_ST_CONNECTING || s_state == APP_ST_CLOSING) {
        breath = 8.0f * sinf((float)(now % 1200) / 1200.0f * 6.2832f);
    } else if (s_state == APP_ST_IDLE || s_state == APP_ST_SETUP) {
        breath = 3.0f * sinf((float)(now % 3000) / 3000.0f * 6.2832f);
    }
    const int orb = ORB_BASE + (int)(s_level_smooth * 0.22f + breath);
    const int halo = orb + 18 + (int)(s_level_smooth * 0.3f);
    lv_obj_set_size(s_orb, orb, orb);
    lv_obj_set_pos(s_orb, ORB_CX - orb / 2, ORB_CY - orb / 2);
    lv_obj_set_size(s_halo, halo, halo);
    lv_obj_set_pos(s_halo, ORB_CX - halo / 2, ORB_CY - halo / 2);

    // Light drifting inside the sphere: slow while waiting, livelier with the voice.
    const float speed = s_state == APP_ST_LIVE ? (s_ai_speaking ? 0.11f : 0.06f) + s_level_smooth * 0.0012f : 0.03f;
    s_phase += speed;
    static const float k_size[ORB_BLOBS] = { 0.62f, 0.52f, 0.46f };   // of the orb diameter
    static const float k_rate[ORB_BLOBS][2] = { { 1.0f, 0.7f }, { -0.8f, 1.1f }, { 0.6f, -1.3f } };
    for (int i = 0; i < ORB_BLOBS; i++) {
        const int d = (int)(orb * k_size[i]);
        // Keep each blob inside the circle: centre offset + radius <= orb radius.
        const float reach = (orb - d) * 0.5f * 0.85f;
        const float x = reach * sinf(s_phase * k_rate[i][0] + i * 2.1f);
        const float y = reach * cosf(s_phase * k_rate[i][1] + i * 1.3f);
        lv_obj_set_size(s_blob[i], d, d);
        lv_obj_set_pos(s_blob[i], orb / 2 - d / 2 + (int)x, orb / 2 - d / 2 + (int)y);
    }

    static int last_soc = -2;
    static bool last_wifi;
    const int soc = g_battery_soc;
    if (soc != last_soc) {
        last_soc = soc;
        if (soc < 0) {
            lv_label_set_text(s_battery, "");   // gauge unavailable: draw nothing
        } else {
            const char *sym = soc > 80 ? LV_SYMBOL_BATTERY_FULL : soc > 55 ? LV_SYMBOL_BATTERY_3
                            : soc > 30 ? LV_SYMBOL_BATTERY_2 : soc > 10 ? LV_SYMBOL_BATTERY_1
                            : LV_SYMBOL_BATTERY_EMPTY;
            lv_label_set_text_fmt(s_battery, "%d%% %s", soc, sym);
        }
    }
    const bool wifi = app_wifi_connected();
    if (wifi != last_wifi) {
        last_wifi = wifi;
        lv_obj_set_style_bg_color(s_title_dot, lv_color_hex(wifi ? COL_GREEN : COL_GRAY), 0);
    }
}

static void ui_timer(lv_timer_t *t)
{
    (void)t;
    render_model();
    render_frame();
}

esp_err_t app_ui_start(void)
{
    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    // CJK font first, Montserrat only for LVGL symbols (battery, Wi-Fi, volume).
    static lv_font_t s_body;
    s_body = live_cjk_16;
    s_body.fallback = &lv_font_montserrat_14;

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_title_dot = circle(s_scr, 8, COL_GRAY, LV_OPA_COVER);
    lv_obj_set_pos(s_title_dot, 30, 25);
    s_title = label(s_scr, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_text(s_title, "ChatGPT");
    lv_obj_set_pos(s_title, 44, 17);
    s_battery = label(s_scr, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -26, 21);

    s_halo = circle(s_scr, ORB_BASE + 18, ORB_SKY_BOTTOM, LV_OPA_20);
    s_orb = circle(s_scr, ORB_BASE, ORB_SKY_TOP, LV_OPA_COVER);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(ORB_SKY_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(s_orb, LV_GRAD_DIR_VER, 0);
    s_blob[0] = blob(s_orb, 0xFFFFFF, LV_OPA_70);    // bright highlight
    s_blob[1] = blob(s_orb, 0x7CC4FF, LV_OPA_60);    // sky blue
    s_blob[2] = blob(s_orb, 0x2563EB, LV_OPA_50);    // deep blue

    s_qr = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);   // quiet zone for scanners
    lv_obj_set_style_border_width(s_qr, 8, 0);
    lv_obj_set_pos(s_qr, ORB_CX - QR_SIZE / 2 - 8, ORB_CY - QR_SIZE / 2 - 8);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

    s_status = label(s_scr, &s_body, COL_DIM);
    lv_obj_set_width(s_status, 216);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_status, 12, 250);

    lv_screen_load(s_scr);
    render_model();
    render_frame();
    lv_timer_create(ui_timer, 40, NULL);
    bsp_lvgl_unlock();
    return ESP_OK;
}
