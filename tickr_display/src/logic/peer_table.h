#pragma once
// Peer table for multi-device discovery (docs/MULTI_DEVICE.md "The peer table").
//
// Pure logic, no Arduino dependencies: the table, its eviction policy, the
// /peers.bin blob format and the small helpers (id string, version packing)
// are unit-tested on the host. The UDP transport and the HTTP routes live in
// src/managers/peer_manager.cpp.
//
// Group membership (MEMBER flag, epoch) is set by peer_manager when the
// beacon tag verifies under this device's group secret.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef TICKR_MAX_PEERS
#define TICKR_MAX_PEERS 16
#endif

// Peer flags
#define PEER_F_USB      0x01   // sender is USB powered (else battery, see next_wake_s)
#define PEER_F_MEMBER   0x02   // beacon tag verified with the group secret
#define PEER_F_RELAY    0x04   // sender offers the relay role
#define PEER_F_PAIRABLE 0x08   // sender accepts pairing (USB power)
#define PEER_F_MANUAL   0x10   // added by POST /api/peers, not learned from a broadcast
#define PEER_F_STALE    0x20   // no beacon for 4 periods, or loaded from /peers.bin

#define PEER_NAME_LEN   16     // incl. NUL: 15 visible characters
#define PEER_ID_LEN     13     // "tickr-XXXXXX" + NUL

struct Peer {                      // 40 bytes, naturally aligned
    uint8_t  mac[6];               // identity; id "tickr-XXXXXX" derived from mac[3..5]
    uint8_t  flags;                // PEER_F_*
    uint8_t  pos;                  // layout slot cache: x (high nibble) | y (low nibble)
    uint32_t ip;                   // IPv4, network byte order as uint32 (a.b.c.d -> a in the low byte)
    uint32_t last_seen_s;          // receiver uptime seconds when the last beacon arrived
    uint32_t next_wake_s;          // receiver uptime seconds; 0 = USB / unknown
    uint16_t version;              // packed MAJOR(4).MINOR(6).PATCH(6)
    uint8_t  epoch;                // group epoch from the last beacon (low byte); 0 = none
    int8_t   rssi;                 // as reported by the sender
    char     name[PEER_NAME_LEN];  // NUL-terminated, printable ASCII, no quotes/backslashes
};
static_assert(sizeof(Peer) == 40, "Peer must stay 40 bytes (docs/MULTI_DEVICE.md, The peer table)");

// Header of /peers.bin: magic 'TKPR', format version, entry count, CRC-16 of
// the entries. 8 bytes, followed by count * sizeof(Peer).
#define PEERS_BLOB_MAGIC   "TKPR"
#define PEERS_BLOB_VERSION 1
#define PEERS_BLOB_HEADER  8
#define PEERS_BLOB_MAX     (PEERS_BLOB_HEADER + TICKR_MAX_PEERS * (int)sizeof(Peer))

struct PeerTable {
    Peer    peers[TICKR_MAX_PEERS];
    uint8_t count;
    bool    dirty;                  // changed since the last save
};

void  peer_table_init(PeerTable* t);
Peer* peer_table_find(PeerTable* t, const uint8_t mac[6]);
Peer* peer_table_find_by_id(PeerTable* t, const char* id);

// Inserts or updates the entry for `in.mac`. `in.last_seen_s` must already
// be the current uptime. Returns the stored entry, or nullptr when the table
// is full and nothing could be evicted. Eviction order when full: oldest
// non-member first, then oldest STALE member; members are never evicted for
// a non-member. Sets `dirty` whenever the persisted view changed.
Peer* peer_table_upsert(PeerTable* t, const Peer& in);

// Removes the entry at `idx` (order is not preserved). Returns false if out of range.
bool  peer_table_remove(PeerTable* t, size_t idx);

// Sets PEER_F_STALE on every entry whose last beacon is older than
// `ttl_s` (+ its own sleep plan for sleepers). Returns the number of entries
// that changed state.
size_t peer_table_mark_stale(PeerTable* t, uint32_t now_s, uint32_t ttl_s);

// Number of entries without PEER_F_STALE, optionally only those with
// PEER_F_MEMBER (drives the adaptive beacon period: members once a group exists).
size_t peer_table_fresh_count(const PeerTable* t, bool members_only = false);

// Serialises the table into `buf` (capacity `cap` >= PEERS_BLOB_HEADER +
// count*40). Returns the number of bytes written, 0 on error.
size_t peer_table_save_blob(const PeerTable* t, uint8_t* buf, size_t cap);

// Loads a blob written by peer_table_save_blob(). Every loaded entry gets
// PEER_F_STALE and last_seen_s/next_wake_s = 0 (uptime restarted). Returns
// false (table left empty) on a bad magic/version/CRC/length.
bool  peer_table_load_blob(PeerTable* t, const uint8_t* buf, size_t len);

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
uint16_t peer_crc16(const uint8_t* data, size_t len);

// "tickr-XXXXXX" from the last three MAC bytes (upper-case hex). `out` >= PEER_ID_LEN.
void  peer_id_from_mac(const uint8_t mac[6], char* out);

// Parses "aabbccddeeff" / "AA:BB:CC:DD:EE:FF" into mac. Returns false on bad input.
bool  peer_mac_from_str(const char* s, uint8_t mac[6]);

// Packs "v1.2.3", "1.2.3-4-gabc" or "1.2.3" into 4.6.6 bits (clamped). Anything
// unparsable packs to 0.
uint16_t peer_pack_version(const char* s);
// "M.m.p" into `out` (>= 12 bytes).
void  peer_unpack_version(uint16_t v, char* out, size_t out_len);

// Copies `src` into a PEER_NAME_LEN buffer, replacing every character that
// is not printable ASCII or that would need JSON escaping (", \) with '_'.
void  peer_sanitize_name(const char* src, char* dst);
