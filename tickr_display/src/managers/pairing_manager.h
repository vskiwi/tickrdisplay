#pragma once
//
// Pairing and group trust (docs/MULTI_DEVICE.md "Groups and pairing") - the device side of
// protocol v1: X25519 (mbedTLS) -> HKDF -> 6-digit code + check word on the
// e-ink -> HMAC confirmation -> group credentials in config.json.
//
// Routes (all except GET /api/pair/status follow the group rule: 403 without
// a configured api_token, 401 without a valid one):
//   POST   /api/pair/start   pk=<b64 32 B> [mode=code|rekey]      -> 202 {sid,state:"starting"}
//                            the X25519 and the screen run on the main task; poll status
//   GET    /api/pair/status  (open) {state,sid,pk_d,n_d,chk,expires_s,attempts_left,retry_after_s,mode}
//   POST   /api/pair/confirm sid= ci=<b64 32 B> [box=<b64 90 B>] [name=]  -> 200 {c_d,group}
//   POST   /api/pair/cancel  | DELETE /api/pair                     -> 200
//   GET    /api/group        -> {id,name,epoch,members} | 404
//   GET    /api/group/secret?pk=<b64>  -> 202 (computing) ... 200 {pk_a,n_a,box}
//   POST   /api/group/leave  -> credentials wiped, one unsigned beacon ("bye")
//   POST   /api/group/rekey  -> new secret, epoch+1 on this device (the panel distributes it)
//
// Request bodies are form-encoded (application/x-www-form-urlencoded) so no
// JSON parser is needed on the device; responses are JSON, binary as base64.
// All crypto (ECDH, HKDF, HMAC) and all drawing happen on the main task
// (pairing_loop); handlers only validate, record and answer.
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config_manager.h"

// Installs the HMAC primitive, publishes the stored credentials to the
// beacon sender. `cfg` must outlive the program (ConnectivityManager's copy).
void pairing_begin(AppConfig* cfg);
void pairing_register_routes(AsyncWebServer& server);
// Main-task work: key agreement, screens, timeouts, cooldown, deferred beacon.
void pairing_loop();

bool pairing_has_group();
// Wipes the credentials (config.json) and announces it with one unsigned
// beacon - what POST /api/group/leave does; also used by recovery mode.
// False when there is no group or the write failed.
bool pairing_leave_group();
// {"id":"..","name":"..","epoch":N} or null - for /api/identity.
void pairing_group_json(char* out, size_t out_len);
