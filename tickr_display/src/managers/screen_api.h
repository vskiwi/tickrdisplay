#pragma once
//
// Read-only screen API (docs/API.md "Screen"):
//   GET /api/screen/raw     the shadow framebuffer, 4 736 B, 1 bpp (see hal_display.h),
//                           headers X-Screen-Width/Height/Format, ETag "<boot>-<render_seq>",
//                           If-None-Match -> 304; 503 when the largest free heap block < 32 KB
//   GET /api/screen.bmp     the same frame as a 1-bit BMP (5 182 B) for <img> / HA picture cards
//   GET /api/screen/state   {"title","value","state","stale_s","render_seq","rendered_at_s",
//                            "refreshes_full","refreshes_partial","refreshes_skipped","width",
//                            "height","power_switch","power_grace_s","source","symbol",
//                            "status":{"wifi","bars","usb","power","batt_pct","ip"}}
//                           source = the configured pull target ticker|url|none,
//                           symbol = the ticker's symbol ("" otherwise)
//                           state = boot|setup|waiting|content|pairing|recovery|ota|identify
//                           (logic/ui_strings.h), stale_s = age of the shown content or null,
//                           power = usb|battery|unknown; power_switch =
//                           off|idle|grace|blocked|restart, power_grace_s = seconds left
//   POST /api/screen/identify {"n":k,"ttl_s":30}  show the number k (1..99) as large as
//                           possible for ttl_s seconds, n = 0 restores the content (panel
//                           "Identify" mode); drawn by the main task via screen_api_loop()
// The GETs are public (the pairing code is still withheld with 503),
// identify follows the normal auth rule (open without a token, token when set).
// Bodies are streamed straight from the static buffer - no heap copy.
//
#include <ESPAsyncWebServer.h>

void screen_api_register_routes(AsyncWebServer& server);
// Main-task work: draws a pending identify frame (never on the async_tcp task).
void screen_api_loop();
// Runtime mode switch (logic/device_state.h PowerSwitch): main.cpp stores
// the machine's verdict once a second; GET /api/screen/state reports it.
void screen_api_set_power_switch(uint8_t sw, uint32_t grace_s);
// Content source: "ticker" | "url" | "none" and the ticker symbol, stored
// by main.cpp after init and after POST /config; the strings must stay valid.
void screen_api_set_source(const char* source, const char* symbol);
