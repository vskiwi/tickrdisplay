#pragma once
// Relay for sleepers (docs/MULTI_DEVICE.md "Relay for sleeping members"): the
// pure parts. A USB group member stores a pending payload (and an OTA job)
// per peer id; a battery member fetches it on wake-up and uploads its frame,
// authenticating with the group secret. This module holds the signature
// scheme, the single-use nonce store, the route/file-name mapping and the
// sleeper's decisions - everything host-tested; the HTTP, UDP and file I/O
// live in src/managers/relay_manager.cpp.
//
// Request signature (header X-Tickr-Group, one line):
//   <group_id 16 hex>:<epoch>:<nonce 16 hex>:<64 hex HMAC-SHA-256(group_secret, msg)>
//   msg = "tickr-relay-v1\n" METHOD "\n" path "\n" nonce "\n" SHA-256(body) as 64 hex
// The path is the request path without the query string. The nonce is issued
// by the relay (in its probe reply as "rn", and after every verified request
// in the response header X-Tickr-Nonce), is valid RELAY_NONCE_TTL_S seconds
// and can be used once. Devices have no clock, so there is no timestamp.
#include <stdint.h>
#include <stddef.h>
#include "beacon.h"

#define RELAY_NONCE_LEN     17    // 16 hex + NUL
#define RELAY_NONCE_TTL_S   30
#define RELAY_NONCE_SLOTS   4     // outstanding nonces per relay (probe replies + response chains)
#define RELAY_SIG_LEN       65    // 64 hex + NUL
#define RELAY_HDR_MAX       112   // 16+1+5+1+16+1+64 = 104 + NUL, rounded
#define RELAY_SIGN_MAX      192   // canonical message buffer
#define RELAY_PATH_MAX      48    // "/api/peers/tickr-XXXXXX/screen" = 30
#define RELAY_ID_LEN        13    // "tickr-XXXXXX" + NUL
#define RELAY_URL_MAX       128   // OTA / pull URLs, as AppConfig::pull_url
#define RELAY_REPORT_MIN_INTERVAL_MIN 10   // frame upload only at intervals >= 10 min

// ---- nonce store (relay side) ----------------------------------------------
struct RelayNonce {
    char     nonce[RELAY_NONCE_LEN];   // "" = free slot
    uint32_t issued_s;
};
struct RelayNonceStore {
    RelayNonce slots[RELAY_NONCE_SLOTS];
    uint8_t    next;                   // round-robin victim
};
void relay_nonce_init(RelayNonceStore* s);
// Issues a fresh 16-hex nonce (two rng() words), overwriting the oldest slot.
void relay_nonce_issue(RelayNonceStore* s, uint32_t now_s, uint32_t (*rng)(), char out[RELAY_NONCE_LEN]);
// True when `nonce` was issued no more than RELAY_NONCE_TTL_S ago; the slot is
// cleared either way (single use, and a stale one is garbage anyway).
bool relay_nonce_consume(RelayNonceStore* s, const char* nonce, uint32_t now_s);

// ---- signature --------------------------------------------------------------
struct RelayHeader {
    char     group_id[BEACON_HEX16_LEN];
    uint16_t epoch;
    char     nonce[RELAY_NONCE_LEN];
    uint8_t  mac[32];                  // the HMAC, decoded
};
// Canonical message; returns its length or 0 when it does not fit.
size_t relay_sign_message(const char* method, const char* path, const char* nonce,
                          const uint8_t body_sha256[32], char* out, size_t cap);
// HMAC-SHA-256(secret, msg) as 64 lower-case hex (g_hmac_sha256 from pairing.h).
void relay_sign(const uint8_t secret[32], const char* msg, size_t msg_len, char out[RELAY_SIG_LEN]);
// "<gid>:<epoch>:<nonce>:<sig>"; returns the length or 0.
size_t relay_header_build(const char* group_id, uint16_t epoch, const char* nonce, const char* sig_hex,
                          char* out, size_t cap);
// Convenience for the client: sign and build in one go.
size_t relay_header_make(const char* group_id, uint16_t epoch, const uint8_t secret[32], const char* nonce,
                         const char* method, const char* path, const uint8_t body_sha256[32], char* out, size_t cap);
// Strict parse: exactly four ':'-separated fields, 16 hex / 0..65535 / 16 hex / 64 hex. False otherwise.
bool relay_header_parse(const char* value, RelayHeader* out);
// True when the header names our group and epoch and its HMAC matches
// (constant time). Does not touch the nonce store - consume the nonce first.
bool relay_verify(const RelayHeader& h, const char* group_id, uint16_t epoch, const uint8_t secret[32],
                  const char* method, const char* path, const uint8_t body_sha256[32]);

// ---- routes and files -------------------------------------------------------
enum RelayRoute : uint8_t {
    RELAY_ROUTE_NONE = 0,
    RELAY_ROUTE_LIST,      // /api/relay
    RELAY_ROUTE_PAYLOAD,   // /api/relay/<id>            -> /relay/<id>.json
    RELAY_ROUTE_OTA,       // /api/relay/<id>/ota        -> /relay/<id>.ota.json
    RELAY_ROUTE_SCREEN,    // /api/peers/<id>/screen     -> /peers/<id>.raw
};
// Classifies a request path (no query string). `id` receives the peer id
// ("tickr-" + 6 upper-case hex) for the per-id routes, "" otherwise.
RelayRoute relay_route(const char* path, char id[RELAY_ID_LEN]);
// The path a client signs / requests for a route and id.
size_t relay_route_path(RelayRoute r, const char* id, char* out, size_t cap);
// LittleFS file behind a per-id route; returns false for LIST/NONE.
bool relay_file_name(RelayRoute r, const char* id, char* out, size_t cap);
// Peer id "tickr-XXXXXX" from a file name in /relay or /peers ("tickr-XXXXXX.json",
// "tickr-XXXXXX.ota.json", "tickr-XXXXXX.raw"); returns the route or NONE.
RelayRoute relay_file_route(const char* name, char id[RELAY_ID_LEN]);
// Accepts "http://" / "https://" URLs of printable ASCII, 8..RELAY_URL_MAX-1 chars.
bool relay_url_ok(const char* url);
// Pulls "url":"..." out of a small JSON body without a parser; false when
// absent, unterminated or not relay_url_ok().
bool relay_url_from_json(const char* body, char* out, size_t cap);

// ---- the sleeper's decisions ----------------------------------------------
enum RelaySource : uint8_t {
    RELAY_SRC_NONE = 0,    // nothing configured: stay awake on the WAITING card
    RELAY_SRC_PULL_URL,    // explicit Pull URL wins
    RELAY_SRC_RELAY,       // group member without a URL: ask the relay
};
RelaySource relay_pick_source(bool has_pull_url, bool has_group);
// Frame upload after a relay-driven render only at intervals >= 10 min.
bool relay_report_frame(uint32_t interval_min);
// A probe reply usable as a relay: an announce from a USB device other than
// ourselves whose tag verified (member) and that carries a relay nonce.
bool relay_reply_usable(const BeaconIn& b, bool verified, const uint8_t self_mac[6]);

// Wake-up flow of the sleeper as a state machine, so the ordering and the
// loop-prevention rules are testable without a network:
//   PROBE_HINT (cached relay ip) -> PROBE_BCAST -> FETCH -> APPLY -> OTA -> UPLOAD -> DONE
// Events: RELAY_EV_OK / RELAY_EV_FAIL for the step just tried.
enum RelayStep : uint8_t {
    RELAY_STEP_PROBE_HINT = 0,   // unicast probe to the cached relay
    RELAY_STEP_PROBE_BCAST,      // broadcast probe
    RELAY_STEP_FETCH,            // signed GET /api/relay/<id>?consume=1
    RELAY_STEP_APPLY,            // render the payload (if any)
    RELAY_STEP_OTA,              // flash the URL from X-Tickr-OTA-URL (if any)
    RELAY_STEP_UPLOAD,           // PUT the frame (if a payload was drawn and the interval allows)
    RELAY_STEP_DONE,
    RELAY_STEP_NO_RELAY,         // terminal: nobody answered
    RELAY_STEP_FAILED,           // terminal: the relay answered but the fetch failed
};
struct RelayFlow {
    RelayStep step;
    bool      has_hint;      // a cached relay ip exists
    bool      got_payload;   // FETCH returned a body
    bool      got_ota;       // FETCH carried an OTA url
    bool      report;        // frame upload allowed (interval)
    uint8_t   probes;        // probes sent (loop guard: at most one hint + one broadcast)
};
void relay_flow_init(RelayFlow* f, bool has_hint, bool report);
// Advances the flow after `ok` for the current step. FETCH's outcome is given
// through `has_payload` / `has_ota` (ignored for other steps).
void relay_flow_next(RelayFlow* f, bool ok, bool has_payload, bool has_ota);
