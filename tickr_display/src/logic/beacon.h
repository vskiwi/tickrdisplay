#pragma once
// UDP discovery beacon - wire format (docs/MULTI_DEVICE.md "Discovery: UDP beacons", docs/API.md "UDP beacon").
//
// One JSON object per datagram, <= BEACON_MAX_LEN bytes:
//   {"t":"tickr","mac":"020000d4e5f6","n":"Kitchen left","ip":"10.0.0.5",
//    "v":"0.3.1","pw":"usb"|"bat","sl":0,"seq":12,"rs":-62,"g":"","ep":0,"tag":""}
//   "nc":"<nonce>" is added to a unicast reply to a probe; a USB group member
//   also adds "rn":"<16 hex>" there - a relay nonce for one signed request
//   (docs/MULTI_DEVICE.md "Relay for sleeping members" / logic/relay.h). Neither is signed:
//   a forged nonce only fails at the relay.
// A probe is {"t":"tickr?","mac":"...","nc":"<nonce>"}.
//
// "g" (group id, 16 hex), "ep" (group epoch) and "tag" authenticate a group
// member (docs/MULTI_DEVICE.md "What the group secret signs"): tag = first 8 bytes of
// HMAC-SHA-256(group_secret, beacon_sign_message()) as 16 lower-case hex
// digits, where the signed message is
//   "tickr-beacon-v1\n" mac "\n" name "\n" ip "\n" pw "\n" sl "\n" seq "\n" g "\n" ep
// (v and rs are not signed: v is packed lossily on receipt, rs is cosmetic).
// A device without a group sends g="", ep=0, tag="". The identity is the MAC;
// the "tickr-XXXXXX" id is derived from it on both sides (peer_id_from_mac).
//
// Pure logic with its own bounded JSON scanner (no ArduinoJson - flash budget,
// docs/DEVELOPMENT.md "Size gate"), unit-tested on the host against hostile
// input: the parser never reads past the datagram, fails closed on anything
// malformed, takes the first occurrence of a duplicate key, skips unknown
// and nested values (depth <= 4) and rejects trailing bytes after the object.
#include <stdint.h>
#include <stddef.h>
#include "peer_table.h"

#define BEACON_PORT       47000
#define BEACON_MAX_LEN    255     // datagrams above this are dropped unparsed
#define BEACON_HEX16_LEN  17      // 16 hex chars + NUL (group id / tag)
#define BEACON_NONCE_LEN  17
#define BEACON_SIGN_MAX   128     // canonical signed message buffer

enum BeaconKind : uint8_t {
    BEACON_INVALID = 0,
    BEACON_ANNOUNCE,   // "t":"tickr"
    BEACON_PROBE,      // "t":"tickr?"
};

// What we say about ourselves.
struct BeaconSelf {
    uint8_t  mac[6];
    char     name[PEER_NAME_LEN];
    char     ip[16];
    char     version[16];      // truncated for the wire, packed on receipt
    bool     usb;
    uint32_t sleep_s;          // planned sleep after this wake (battery), 0 on USB
    int8_t   rssi;
    char     group_id[BEACON_HEX16_LEN];   // "" until paired
    uint16_t epoch;                        // 0 until paired
};

// Parsed incoming datagram.
struct BeaconIn {
    BeaconKind kind;
    uint8_t  mac[6];
    char     name[PEER_NAME_LEN];   // sanitised (peer_sanitize_name)
    uint32_t ip;                    // 0 if absent/invalid
    uint16_t version;               // packed
    bool     usb;
    uint32_t sleep_s;
    uint32_t seq;
    int8_t   rssi;
    uint16_t epoch;
    char     group_id[BEACON_HEX16_LEN];
    char     tag[BEACON_HEX16_LEN];
    char     nonce[BEACON_NONCE_LEN];   // probe nonce / echoed nonce
    char     relay_nonce[BEACON_HEX16_LEN];   // "rn": exactly 16 hex, else ""
};

// Writes the announce datagram into `out` (capacity `cap`). `seq` is the
// sender's monotonic counter, `nonce` (may be NULL) is echoed as "nc", `tag`
// (may be NULL) is the 16-hex signature from beacon_tag(), `relay_nonce`
// (may be NULL) goes out as "rn". Returns the length, or 0 if it did not fit.
size_t beacon_build(const BeaconSelf& self, uint32_t seq, const char* nonce, const char* tag, char* out, size_t cap,
                    const char* relay_nonce = nullptr);

// Canonical signed message for a beacon we send resp. one we received (the
// two must agree byte for byte for a valid tag). Returns the length or 0.
size_t beacon_sign_message(const BeaconSelf& self, uint32_t seq, char* out, size_t cap);
size_t beacon_sign_message_in(const BeaconIn& b, char* out, size_t cap);

// HMAC tag over a signed message, 16 lower-case hex chars.
void beacon_tag(const uint8_t secret[32], const char* msg, size_t msg_len, char out[BEACON_HEX16_LEN]);

// True when `b` carries a group id and a tag that verifies under `secret`
// (constant-time compare). The caller still checks group id and epoch.
bool beacon_verify(const BeaconIn& b, const uint8_t secret[32]);

// Writes a probe datagram. Returns the length, or 0.
size_t beacon_build_probe(const uint8_t mac[6], const char* nonce, char* out, size_t cap);

// Parses exactly `len` bytes (no NUL terminator needed or read). Returns
// false (out->kind = BEACON_INVALID) on anything that is not a well-formed
// tickr datagram: too long, not a JSON object, trailing garbage, wrong "t",
// missing/invalid "mac". Never throws, never allocates beyond the stack.
bool beacon_parse(const char* data, size_t len, BeaconIn* out);

// Fills a Peer from a parsed announce (last_seen_s = now_s, next_wake_s from
// sleep_s). MEMBER is never set here (the tag is verified first).
void beacon_to_peer(const BeaconIn& b, uint32_t now_s, Peer* out);

// Dotted quad -> uint32 in the same byte order as Peer::ip (a.b.c.d -> a low).
bool beacon_ip_parse(const char* s, uint32_t* out);
void beacon_ip_format(uint32_t ip, char* out, size_t out_len);
