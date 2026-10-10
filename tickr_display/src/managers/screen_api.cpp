#include "screen_api.h"
#include "web_auth.h"
#include "peer_manager.h"
#include "../hal/hal_display.h"
#include "../logic/screen_bmp.h"
#include <Arduino.h>
#include <esp_system.h>

static const char CT_OCTET[] = "application/octet-stream";
static const char CT_BMP[] = "image/bmp";
static const char CT_JSON[] = "application/json";
static const uint32_t MIN_FREE_BLOCK = 32 * 1024;   // never starve the pull path

// ETags restart with the sequence counter on every boot; a boot id keeps a
// browser cache from confusing frame 3 of this boot with frame 3 of the last.
static uint16_t s_boot_id = 0;
static volatile uint8_t  s_power_switch = 0;    // PowerSwitch, written by the main task
static volatile uint32_t s_power_grace_s = 0;

void screen_api_set_power_switch(uint8_t sw, uint32_t grace_s) {
    s_power_switch = sw;
    s_power_grace_s = grace_s;
}

static const char* s_source = "none";
static const char* s_symbol = "";

void screen_api_set_source(const char* source, const char* symbol) {
    s_source = source ? source : "none";
    s_symbol = symbol ? symbol : "";
}

static void etag_for(uint32_t seq, char* out, size_t len) {
    snprintf(out, len, "\"%04x-%lu\"", (unsigned)s_boot_id, (unsigned long)seq);
}

// Shared preamble of raw/bmp: heap guard, conditional GET. Returns false when
// a response has already been sent. Public (the frame is what hangs on the
// wall; the pairing code is still withheld below).
static bool screen_precheck(AsyncWebServerRequest* request, char* etag, size_t etag_len) {
#ifndef TICKR_PAIR_DEBUG_SCREEN
    // The pairing code must be read off the panel, not off the network
    // (docs/MULTI_DEVICE.md "Screen, state machine, limits"): no frame while a pairing screen is shown.
    // tickr_dev builds with -DTICKR_PAIR_DEBUG_SCREEN keep it readable for tests.
    if (display_overlay_active()) {
        AsyncWebServerResponse* r = request->beginResponse(503, CT_JSON, "{\"error\":\"pairing in progress\"}");
        r->addHeader("Retry-After", "10");
        request->send(r);
        return false;
    }
#endif
    if (ESP.getMaxAllocHeap() < MIN_FREE_BLOCK) {
        AsyncWebServerResponse* r = request->beginResponse(503, CT_JSON, "{\"error\":\"low memory, retry later\"}");
        r->addHeader("Retry-After", "5");
        request->send(r);
        return false;
    }
    etag_for(display_render_seq(), etag, etag_len);
    if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == etag) {
        AsyncWebServerResponse* r = request->beginResponse(304);
        r->addHeader("ETag", etag);
        r->addHeader("Cache-Control", "no-cache");
        request->send(r);
        return false;
    }
    return true;
}

static void add_frame_headers(AsyncWebServerResponse* r, const char* etag) {
    r->addHeader("ETag", etag);
    r->addHeader("Cache-Control", "no-cache");
    r->addHeader("X-Screen-Width", String(SCREEN_W));
    r->addHeader("X-Screen-Height", String(SCREEN_H));
    r->addHeader("X-Screen-Format", "1bpp-msb");
}

static void handle_raw(AsyncWebServerRequest* request) {
    char etag[24];
    if (!screen_precheck(request, etag, sizeof(etag))) return;
    const uint8_t* frame = display_frame_buffer();
    AsyncWebServerResponse* r = request->beginResponse(CT_OCTET, SCREEN_FRAME_LEN,
        [frame](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            if (index >= SCREEN_FRAME_LEN) return 0;
            size_t n = SCREEN_FRAME_LEN - index;
            if (n > maxLen) n = maxLen;
            memcpy(buf, frame + index, n);
            return n;
        });
    add_frame_headers(r, etag);
    request->send(r);
}

static void handle_bmp(AsyncWebServerRequest* request) {
    char etag[24];
    if (!screen_precheck(request, etag, sizeof(etag))) return;
    const uint8_t* frame = display_frame_buffer();
    AsyncWebServerResponse* r = request->beginResponse(CT_BMP, BMP_FILE_LEN,
        [frame](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            return screen_bmp_read(frame, index, buf, maxLen);
        });
    add_frame_headers(r, etag);
    r->addHeader("Content-Disposition", "inline; filename=\"screen.bmp\"");
    request->send(r);
}

static void json_str(String& out, const char* s) {
    out += '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { out += '\\'; out += *s; }
        else if ((unsigned char)*s < 0x20) out += ' ';
        else out += *s;
    }
    out += '"';
}

static void handle_state(AsyncWebServerRequest* request) {
    DisplayState st;
    display_get_state(&st);
    String body;
    body.reserve(400);
    body += "{\"title\":"; json_str(body, st.title);
    body += ",\"value\":"; json_str(body, st.value);
    // state: boot|setup|waiting|content|pairing|recovery|ota|identify|
    // offline|battery_empty|power_to_battery (docs/DEVICE_UI.md "The panel shows the same frame"; the
    // shelf tile badge keys on it); stale_s: seconds since the content was
    // shown, null before any payload; card: the condition card set as
    // the base frame - none|offline|ota|battery_empty|power_to_battery - even
    // while a service frame is on top (state then names the service frame).
    // Refresh policy v2 (docs/DEVICE_UI.md "E-ink refresh rules", additive): refreshes_partial counts the
    // differential refreshes, last_full_s is the uptime of the last full
    // (0 = none on this boot), partials_since_full the forced-full counter.
    char num[280], age[12] = "null";
    if (st.has_content) snprintf(age, sizeof(age), "%lu", (unsigned long)st.content_age_s);
    snprintf(num, sizeof(num), ",\"state\":\"%s\",\"card\":\"%s\",\"stale_s\":%s,\"render_seq\":%lu,\"rendered_at_s\":%lu,\"refreshes_full\":%lu,\"refreshes_partial\":%lu,\"refreshes_skipped\":%lu,\"last_full_s\":%lu,\"partials_since_full\":%u",
             screen_state_str(st.state), screen_card_str(st.card), age, (unsigned long)st.render_seq, (unsigned long)st.rendered_at_s,
             (unsigned long)st.refreshes_full, (unsigned long)st.refreshes_partial,
             (unsigned long)st.refreshes_skipped, (unsigned long)st.last_full_s, (unsigned)st.partials_since_full);
    body += num;
    // Ticker fields (additive): layout text|ticker|grid; for a ticker frame also change, dir
    // (-1|0|1) and age_s (the quote's total age: payload age_s + time on the panel);
    // for a grid frame symbols[] and shorts[] in cell order (docs/TICKERS.md
    // "Several tickers on one panel: the 2x2 grid") and age_s of the oldest quote.
    body += ",\"layout\":\"";
    body += st.grid ? "grid" : st.ticker ? "ticker" : "text";
    body += '"';
    if (st.grid) {
        body += ",\"symbols\":[";
        for (uint8_t i = 0; i < st.grid->n; i++) { if (i) body += ','; json_str(body, st.grid->cells[i].symbol); }
        body += "],\"shorts\":[";
        for (uint8_t i = 0; i < st.grid->n; i++) { if (i) body += ','; json_str(body, st.grid->cells[i].short_label); }
        snprintf(num, sizeof(num), "],\"age_s\":%lu", (unsigned long)st.ticker_age_s);
        body += num;
    } else if (st.ticker) {
        body += ",\"change\":"; json_str(body, st.ticker->change);
        body += ",\"short\":"; json_str(body, st.ticker->short_label);   // the badge name, "" = no badge
        snprintf(num, sizeof(num), ",\"dir\":%d,\"age_s\":%lu", (int)st.ticker->dir, (unsigned long)st.ticker_age_s);
        body += num;
    }
    // power_switch / power_grace_s: the runtime USB -> battery mode switch
    // (off|idle|grace|blocked|restart, seconds left in the grace).
    snprintf(num, sizeof(num), ",\"power_switch\":\"%s\",\"power_grace_s\":%lu",
             power_switch_str(s_power_switch), (unsigned long)s_power_grace_s);
    body += num;
    // source / symbol (additive): the configured pull target and the ticker's symbol.
    body += ",\"source\":\"";
    body += s_source;
    body += "\",\"symbol\":";
    json_str(body, s_symbol);
    // status.batt_low: the BATTERY LOW badge is on the frame and the LED is off.
    static const char* const kPower[] = { "unknown", "usb", "battery" };
    snprintf(num, sizeof(num), ",\"width\":%d,\"height\":%d,\"status\":{\"wifi\":%s,\"bars\":%d,\"usb\":%s,\"power\":\"%s\",\"batt_pct\":%u,\"batt_low\":%s,\"ip\":\"%s\"}}",
             SCREEN_W, SCREEN_H, st.status.wifi_connected ? "true" : "false", (int)st.status.wifi_bars,
             st.status.usb ? "true" : "false", kPower[st.status.power < 3 ? st.status.power : 0],
             (unsigned)st.status.batt_pct,
             device_battery_low(st.status.power, st.status.batt_known, st.status.batt_pct) ? "true" : "false",
             st.status.ip);
    body += num;
    char etag[24];
    etag_for(st.render_seq, etag, sizeof(etag));
    AsyncWebServerResponse* r = request->beginResponse(200, CT_JSON, body);
    r->addHeader("ETag", etag);
    r->addHeader("Cache-Control", "no-cache");
    request->send(r);
}

// POST /api/screen/identify {"n":k,"ttl_s":30} (JSON body, <= 96 B):
// the panel numbers the devices like a multi-monitor "Identify" and every
// device shows its number as large as the panel allows for ttl_s seconds;
// n = 0 brings the content back at once. The handler only records the
// request; the main task draws (screen_api_loop) through the temporary-frame
// mechanism shared with the pairing outcome frame. 409 while a pairing code
// is on the screen. No secret is on the frame, so /api/screen.* serve it.
static volatile int  s_ident_n = -1;      // -1 = nothing pending
static uint16_t      s_ident_ttl_s = 30;

static long body_int(const char* body, const char* key, long def) {
    const char* k = body ? strstr(body, key) : nullptr;
    const char* c = k ? strchr(k, ':') : nullptr;
    return c ? atol(c + 1) : def;
}

static void collect_small_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (total == 0 || total > 96 || !web_auth_ok(request)) return;
    if (index == 0) request->_tempObject = calloc(total + 1, 1);   // freed by ~AsyncWebServerRequest
    if (request->_tempObject && index + len <= total) memcpy((uint8_t*)request->_tempObject + index, data, len);
}

static void handle_identify(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    const char* body = (const char*)request->_tempObject;
    long n = body_int(body, "\"n\"", -1), ttl = body_int(body, "\"ttl_s\"", 30);
    if (n < 0 || n > 99 || ttl < 1 || ttl > 600) {
        request->send(400, CT_JSON, "{\"error\":\"JSON n 0..99, ttl_s 1..600\"}");
        return;
    }
    if (display_overlay_active()) {
        request->send(409, CT_JSON, "{\"error\":\"pairing in progress\"}");
        return;
    }
    s_ident_ttl_s = (uint16_t)ttl;
    s_ident_n = (int)n;
    request->send(200, CT_JSON, "{\"ok\":true}");
}

void screen_api_loop() {
    int n = s_ident_n;
    if (n < 0) return;
    s_ident_n = -1;
    if (display_overlay_active()) return;               // a pairing screen went up meanwhile
    if (n == 0) {
        display_temp_end();
        return;
    }
    char name[16];
    peers_self_name(name, sizeof(name));
    display_show_identify((unsigned)n, name);
    display_temp_hold((uint32_t)s_ident_ttl_s * 1000UL);
}

void screen_api_register_routes(AsyncWebServer& server) {
    s_boot_id = (uint16_t)(esp_random() & 0xFFFF);
    server.on("/api/screen/raw", HTTP_GET, handle_raw);
    server.on("/api/screen.bmp", HTTP_GET, handle_bmp);
    server.on("/api/screen/state", HTTP_GET, handle_state);
    server.on("/api/screen/identify", HTTP_POST, handle_identify, nullptr, collect_small_body);
}
