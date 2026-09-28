#pragma once
//
// Multi-device discovery (docs/MULTI_DEVICE.md "How it works"):
//   * UDP beacons on port 47000 (src/logic/beacon.h): broadcast to the local
//     subnet with an adaptive period T = clamp(45 s * ceil(N/8), 45, 180) s
//     (N = fresh peers) and +-20 % jitter; first beacon 0-2 s after start,
//     preceded by a probe so that running peers answer within ~200 ms.
//     A received probe is answered with a unicast beacon after a random
//     0-200 ms delay. Battery devices send exactly one beacon per wake-up
//     (peers_send_beacon_now) and never listen.
//   * Peer table (src/logic/peer_table.h, TICKR_MAX_PEERS entries, 40 B each),
//     persisted in /peers.bin (LittleFS) - written when dirty, at most every
//     10 min, and by peers_flush() before a planned restart. Loaded on boot
//     with STALE set until a fresh beacon arrives.
//   * GET    /api/peers          streamed JSON array (public - beacon data only)
//     DELETE /api/peers/<id>     forget a peer            (group rule: 403 without api_token)
//     POST   /api/peers {"ip"}   probe a peer by unicast  (group rule)
//
// Transport: WiFiUDP polled from the main task (peers_loop). Chosen over
// AsyncUDP because it is already linked (DNSServer), needs no lwIP-task
// callback/queue, and keeps every table mutation on the main task; the
// HTTP handlers only take a short critical section to copy one entry.
// Nothing is sent or received while the setup portal (AP mode) is up:
// peers_begin() is only called once the STA link exists.
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

void peers_register_routes(AsyncWebServer& server);

// Call once the STA connection is up. `name` is the human device name
// (empty -> default "Tickr-XXXX"), `version` the firmware version.
// battery=true: no listener, no periodic beacons (use peers_send_beacon_now).
// sleep_s: planned sleep after this wake-up (battery), 0 on USB.
void peers_begin(const char* name, const char* version, bool battery, uint32_t sleep_s);

// Sends one broadcast beacon immediately (battery wake-up).
void peers_send_beacon_now();

// Main-task housekeeping: receive, beacon, probe replies, staleness, persistence.
void peers_loop();

// Writes /peers.bin now if the table changed since the last save.
void peers_flush();

// Human name shown in beacons (also called when POST /api/identity renames the device).
void peers_set_name(const char* name);

// Group credentials for beacon tags (docs/MULTI_DEVICE.md "What the group secret signs"). secret = NULL
// clears them (beacons go out unsigned, every peer loses MEMBER until its next
// tagged beacon). Safe from any task.
void peers_set_group(const char* id_hex, uint16_t epoch, const uint8_t* secret);

// Fresh peers whose last beacon verified as a member of our group.
size_t peers_member_count();
// Copies the group credentials (id as 16 hex, epoch, 32-byte secret); false without a group. Any task.
bool peers_group_snapshot(char id[17], uint16_t* epoch, uint8_t secret[32]);
// True when `id` ("tickr-XXXXXX") is in the table with MEMBER set (stale entries count).
bool peers_is_member_id(const char* id);
// True in USB mode once peers_begin() ran (the listener / relay role exists).
bool peers_listening();
// Battery side of the relay lookup (relay_manager.cpp): sends one
// probe (unicast to hint_ip, or broadcast when 0) and waits up to timeout_ms
// for a reply from a verified USB member carrying a relay nonce ("rn").
// On success fills the relay's address and the nonce. Blocking, main task.
bool peers_find_relay(uint32_t hint_ip, uint32_t* relay_ip, char nonce[17], uint32_t timeout_ms);

// Own identity helpers (valid after peers_begin, else derived from the MAC on demand).
void peers_self_mac(uint8_t out[6]);
void peers_self_id(char* out, size_t out_len);          // "tickr-XXXXXX"
void peers_default_name(char* out, size_t out_len);     // "Tickr-XXXX"
void peers_self_name(char* out, size_t out_len);        // beacon name (<= 15 chars; the configured name or the default)

// Diagnostics for /api/status: {"count","fresh","members","sent","send_failed","received","verified","dropped","period_s","broadcast","running"}
void peers_stats_json(char* out, size_t out_len);
