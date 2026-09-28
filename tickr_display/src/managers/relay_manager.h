#pragma once
//
// Relay for sleepers (docs/MULTI_DEVICE.md "Relay for sleeping members").
//
// Relay side (any USB group member): the panel parks a payload and/or an OTA
// job per peer id, a waking battery member fetches them with a request signed
// by the group secret (logic/relay.h) and uploads its rendered frame.
//   GET    /api/relay                 api_token  {"payload":[ids],"ota":[ids]} - what is pending
//   PUT    /api/relay/<id>            api_token  body = payload (<= 4 KB, validated) -> /relay/<id>.json
//   PUT    /api/relay/<id>/ota        api_token  {"url":"http(s)://.../firmware.bin"} -> /relay/<id>.ota.json
//   DELETE /api/relay/<id>[/ota]      api_token  forget
//   GET    /api/relay/<id>[?consume=1] signature  200 + payload | 204; header X-Tickr-OTA-URL when an OTA
//                                                 job waits; consume=1 deletes both after delivery
//   PUT    /api/peers/<id>/screen     signature  4 736 B frame -> /peers/<id>.raw
//   GET    /api/peers/<id>/screen     public     the sleeper's last frame (+ X-Screen-Age-S when known)
// Every verified signed request answers with X-Tickr-Nonce: the nonce for the
// sleeper's next request (the first one comes with the probe reply, "rn").
// The api_token routes follow the group rule (403 without a configured token,
// 401 without a valid one) and need this device to be in a group; <id> must
// be a known member of it.
//
// Battery side: relay_client_run() - probe (cached relay first), signed fetch,
// render, optional OTA, frame upload - runs blocking on the main task right
// after the wake-up beacon (main.cpp), only when no Pull URL is configured.
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Register BEFORE peers_register_routes(): "/api/peers" would otherwise
// swallow /api/peers/<id>/screen (prefix matching of the URI matcher).
void relay_register_routes(AsyncWebServer& server);
// Creates /relay and /peers on LittleFS. Call after LittleFS is mounted.
void relay_begin();

// True when this device offers the relay role: USB power and a group.
bool relay_is_relay();
// Nonce for a probe reply ("rn"). False (out = "") when not a relay.
bool relay_issue_nonce(char out[17]);

enum RelayClientResult : uint8_t {
    RELAY_RC_NO_RELAY,   // nobody answered the probe(s)
    RELAY_RC_NOTHING,    // a relay answered, nothing pending
    RELAY_RC_SHOWN,      // a payload was rendered (frame uploaded when the interval allows)
    RELAY_RC_FAILED,     // the relay answered but the fetch/apply failed
};
// `relay_ip_cache` (RTC memory) is read as the hint and updated; a successful
// OTA job restarts the device and never returns. `interval_min` decides the
// frame upload (>= 10 min).
RelayClientResult relay_client_run(uint32_t* relay_ip_cache, uint32_t interval_min);
