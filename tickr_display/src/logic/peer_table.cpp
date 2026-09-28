#include "peer_table.h"
#include <stdio.h>
#include <stdlib.h>

void peer_table_init(PeerTable* t) {
    memset(t, 0, sizeof(*t));
}

Peer* peer_table_find(PeerTable* t, const uint8_t mac[6]) {
    for (size_t i = 0; i < t->count; i++) {
        if (memcmp(t->peers[i].mac, mac, 6) == 0) return &t->peers[i];
    }
    return nullptr;
}

Peer* peer_table_find_by_id(PeerTable* t, const char* id) {
    // "tickr-XXXXXX" -> the last three MAC bytes; no formatting inside the
    // (possibly critical) section, plain byte compares only.
    if (!id || strncmp(id, "tickr-", 6) != 0) return nullptr;
    uint8_t tail[6];
    char twelve[13];
    snprintf(twelve, sizeof(twelve), "000000%s", id + 6);
    if (strlen(id + 6) != 6 || !peer_mac_from_str(twelve, tail)) return nullptr;
    for (size_t i = 0; i < t->count; i++) {
        if (memcmp(t->peers[i].mac + 3, tail + 3, 3) == 0) return &t->peers[i];
    }
    return nullptr;
}

// Index of the oldest entry matching `want_flags`/`mask`, or -1.
static int oldest_index(const PeerTable* t, uint8_t mask, uint8_t want) {
    int best = -1;
    for (size_t i = 0; i < t->count; i++) {
        if ((t->peers[i].flags & mask) != want) continue;
        if (best < 0 || t->peers[i].last_seen_s < t->peers[best].last_seen_s) best = (int)i;
    }
    return best;
}

Peer* peer_table_upsert(PeerTable* t, const Peer& in) {
    Peer* p = peer_table_find(t, in.mac);
    if (!p) {
        if (t->count >= TICKR_MAX_PEERS) {
            int victim = oldest_index(t, PEER_F_MEMBER, 0);                    // oldest non-member
            if (victim < 0 && (in.flags & PEER_F_MEMBER)) {
                victim = oldest_index(t, PEER_F_MEMBER | PEER_F_STALE, PEER_F_MEMBER | PEER_F_STALE);
            }
            if (victim < 0) return nullptr;
            p = &t->peers[victim];
        } else {
            p = &t->peers[t->count++];
        }
        *p = in;
        p->flags &= (uint8_t)~PEER_F_STALE;
        t->dirty = true;
        return p;
    }
    // Existing entry: keep the MANUAL flag, refresh everything else. Only
    // the persisted view (name, ip, flags, version, sleep plan) marks dirty -
    // last_seen/rssi alone would rewrite the file on every beacon.
    uint8_t keep = p->flags & PEER_F_MANUAL;
    uint8_t new_flags = (uint8_t)((in.flags & ~(PEER_F_STALE | PEER_F_MANUAL)) | keep);
    bool changed = p->ip != in.ip || p->version != in.version || p->flags != new_flags ||
                   p->epoch != in.epoch || p->pos != in.pos ||
                   (p->next_wake_s == 0) != (in.next_wake_s == 0) ||
                   strncmp(p->name, in.name, PEER_NAME_LEN) != 0;
    p->ip = in.ip;
    p->version = in.version;
    p->flags = new_flags;
    p->epoch = in.epoch;
    p->pos = in.pos;
    p->rssi = in.rssi;
    p->last_seen_s = in.last_seen_s;
    p->next_wake_s = in.next_wake_s;
    memcpy(p->name, in.name, PEER_NAME_LEN);
    p->name[PEER_NAME_LEN - 1] = '\0';
    if (changed) t->dirty = true;
    return p;
}

bool peer_table_remove(PeerTable* t, size_t idx) {
    if (idx >= t->count) return false;
    t->count--;
    if (idx != t->count) t->peers[idx] = t->peers[t->count];
    memset(&t->peers[t->count], 0, sizeof(Peer));
    t->dirty = true;
    return true;
}

size_t peer_table_mark_stale(PeerTable* t, uint32_t now_s, uint32_t ttl_s) {
    size_t changed = 0;
    for (size_t i = 0; i < t->count; i++) {
        Peer& p = t->peers[i];
        if (p.flags & PEER_F_STALE) continue;
        uint32_t limit = ttl_s;
        if (p.next_wake_s > p.last_seen_s) limit += p.next_wake_s - p.last_seen_s;   // sleeper: wait for its plan
        if (now_s - p.last_seen_s > limit) {
            p.flags |= PEER_F_STALE;
            changed++;
        }
    }
    return changed;
}

size_t peer_table_fresh_count(const PeerTable* t, bool members_only) {
    size_t n = 0;
    uint8_t need = members_only ? PEER_F_MEMBER : 0;
    for (size_t i = 0; i < t->count; i++) {
        uint8_t f = t->peers[i].flags;
        if (!(f & PEER_F_STALE) && (f & need) == need) n++;
    }
    return n;
}

uint16_t peer_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t peer_table_save_blob(const PeerTable* t, uint8_t* buf, size_t cap) {
    size_t body = (size_t)t->count * sizeof(Peer);
    if (cap < PEERS_BLOB_HEADER + body) return 0;
    memcpy(buf, PEERS_BLOB_MAGIC, 4);
    buf[4] = PEERS_BLOB_VERSION;
    buf[5] = t->count;
    memcpy(buf + PEERS_BLOB_HEADER, t->peers, body);
    uint16_t crc = peer_crc16(buf + PEERS_BLOB_HEADER, body);
    buf[6] = (uint8_t)(crc & 0xFF);
    buf[7] = (uint8_t)(crc >> 8);
    return PEERS_BLOB_HEADER + body;
}

bool peer_table_load_blob(PeerTable* t, const uint8_t* buf, size_t len) {
    peer_table_init(t);
    if (!buf || len < PEERS_BLOB_HEADER) return false;
    if (memcmp(buf, PEERS_BLOB_MAGIC, 4) != 0 || buf[4] != PEERS_BLOB_VERSION) return false;
    size_t count = buf[5];
    if (count > TICKR_MAX_PEERS) return false;
    size_t body = count * sizeof(Peer);
    if (len != PEERS_BLOB_HEADER + body) return false;
    uint16_t crc = (uint16_t)(buf[6] | (buf[7] << 8));
    if (peer_crc16(buf + PEERS_BLOB_HEADER, body) != crc) return false;
    memcpy(t->peers, buf + PEERS_BLOB_HEADER, body);
    t->count = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        Peer& p = t->peers[i];
        p.flags |= PEER_F_STALE;
        p.last_seen_s = 0;
        p.next_wake_s = 0;
        p.name[PEER_NAME_LEN - 1] = '\0';
    }
    return true;
}

void peer_id_from_mac(const uint8_t mac[6], char* out) {
    snprintf(out, PEER_ID_LEN, "tickr-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool peer_mac_from_str(const char* s, uint8_t mac[6]) {
    if (!s) return false;
    int n = 0;
    while (*s && n < 6) {
        if (*s == ':' || *s == '-') { s++; continue; }
        int hi = hexval(s[0]);
        int lo = s[0] ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0) return false;
        mac[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n == 6 && *s == '\0';
}

uint16_t peer_pack_version(const char* s) {
    if (!s) return 0;
    if (*s == 'v' || *s == 'V') s++;
    char* end;
    unsigned long maj = strtoul(s, &end, 10);
    if (end == s || *end != '.') return 0;
    s = end + 1;
    unsigned long min = strtoul(s, &end, 10);
    if (end == s || *end != '.') return 0;
    s = end + 1;
    unsigned long pat = strtoul(s, &end, 10);
    if (end == s) return 0;
    if (maj > 15) maj = 15;
    if (min > 63) min = 63;
    if (pat > 63) pat = 63;
    return (uint16_t)((maj << 12) | (min << 6) | pat);
}

void peer_unpack_version(uint16_t v, char* out, size_t out_len) {
    snprintf(out, out_len, "%u.%u.%u", (unsigned)(v >> 12), (unsigned)((v >> 6) & 0x3F), (unsigned)(v & 0x3F));
}

void peer_sanitize_name(const char* src, char* dst) {
    size_t i = 0;
    if (src) {
        for (; i < PEER_NAME_LEN - 1 && src[i]; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[i] = (c < 0x20 || c > 0x7E || c == '"' || c == '\\') ? '_' : (char)c;
        }
    }
    dst[i] = '\0';
}
