#include "beacon.h"
#include "pairing.h"
#include <stdio.h>
#include <stdlib.h>

static void mac_hex(const uint8_t mac[6], char* out) {   // 13 bytes
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

size_t beacon_build(const BeaconSelf& self, uint32_t seq, const char* nonce, const char* tag, char* out, size_t cap,
                    const char* relay_nonce) {
    char mac[13];
    mac_hex(self.mac, mac);
    char name[PEER_NAME_LEN];
    peer_sanitize_name(self.name, name);
    int n = snprintf(out, cap,
                     "{\"t\":\"tickr\",\"mac\":\"%s\",\"n\":\"%s\",\"ip\":\"%s\",\"v\":\"%.15s\","
                     "\"pw\":\"%s\",\"sl\":%lu,\"seq\":%lu,\"rs\":%d,\"g\":\"%.16s\",\"ep\":%u,\"tag\":\"%.16s\"",
                     mac, name, self.ip, self.version, self.usb ? "usb" : "bat",
                     (unsigned long)self.sleep_s, (unsigned long)seq, (int)self.rssi,
                     self.group_id, (unsigned)self.epoch, tag ? tag : "");
    if (n < 0 || (size_t)n >= cap) return 0;
    if (nonce && *nonce) {
        int m = snprintf(out + n, cap - (size_t)n, ",\"nc\":\"%.16s\"", nonce);
        if (m < 0 || (size_t)(n + m) >= cap) return 0;
        n += m;
    }
    if (relay_nonce && *relay_nonce) {
        int m = snprintf(out + n, cap - (size_t)n, ",\"rn\":\"%.16s\"", relay_nonce);
        if (m < 0 || (size_t)(n + m) >= cap) return 0;
        n += m;
    }
    if ((size_t)n + 2 > cap) return 0;
    out[n++] = '}';
    out[n] = '\0';
    return (size_t)n;
}

size_t beacon_build_probe(const uint8_t mac6[6], const char* nonce, char* out, size_t cap) {
    char mac[13];
    mac_hex(mac6, mac);
    int n = snprintf(out, cap, "{\"t\":\"tickr?\",\"mac\":\"%s\",\"nc\":\"%.16s\"}", mac, nonce ? nonce : "");
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

// The canonical string a beacon tag signs (docs/MULTI_DEVICE.md "What the group secret signs", as
// built). Fields the receiver cannot reproduce byte-for-byte are left out
// (v is packed lossily, rs is cosmetic); the name is sanitised on both
// sides and the ip is re-formatted from the parsed address.
static size_t sign_message(const uint8_t mac6[6], const char* name, const char* ip, bool usb, uint32_t sleep_s,
                           uint32_t seq, const char* group_id, uint16_t epoch, char* out, size_t cap) {
    char mac[13];
    mac_hex(mac6, mac);
    int n = snprintf(out, cap, "tickr-beacon-v1\n%s\n%s\n%s\n%s\n%lu\n%lu\n%s\n%u",
                     mac, name, ip, usb ? "usb" : "bat", (unsigned long)sleep_s, (unsigned long)seq,
                     group_id, (unsigned)epoch);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

size_t beacon_sign_message(const BeaconSelf& self, uint32_t seq, char* out, size_t cap) {
    char name[PEER_NAME_LEN];
    peer_sanitize_name(self.name, name);
    return sign_message(self.mac, name, self.ip, self.usb, self.sleep_s, seq, self.group_id, self.epoch, out, cap);
}

size_t beacon_sign_message_in(const BeaconIn& b, char* out, size_t cap) {
    char ip[16] = "";
    if (b.ip) beacon_ip_format(b.ip, ip, sizeof(ip));
    return sign_message(b.mac, b.name, ip, b.usb, b.sleep_s, b.seq, b.group_id, b.epoch, out, cap);
}

void beacon_tag(const uint8_t secret[GROUP_SECRET_LEN], const char* msg, size_t msg_len, char out[BEACON_HEX16_LEN]) {
    uint8_t mac[32];
    if (g_hmac_sha256) g_hmac_sha256(secret, GROUP_SECRET_LEN, (const uint8_t*)msg, msg_len, mac);
    else memset(mac, 0, sizeof(mac));
    pairing_hex(mac, 8, out);
}

bool beacon_verify(const BeaconIn& b, const uint8_t secret[GROUP_SECRET_LEN]) {
    if (b.kind != BEACON_ANNOUNCE || strlen(b.tag) != 16 || b.group_id[0] == '\0') return false;
    char msg[BEACON_SIGN_MAX];
    size_t n = beacon_sign_message_in(b, msg, sizeof(msg));
    if (!n) return false;
    char want[BEACON_HEX16_LEN];
    beacon_tag(secret, msg, n, want);
    // Case-insensitive constant-time compare of the two hex strings.
    uint8_t got[8], exp[8];
    if (!pairing_unhex(b.tag, got, 8) || !pairing_unhex(want, exp, 8)) return false;
    return pairing_ct_equal(got, exp, 8);
}

// ---------------------------------------------------------------------------
// Bounded JSON scanner (replaces ArduinoJson for the datagram).
// Works on [p, end) and never reads a byte at or past `end`; every routine
// fails closed on anything unexpected. Only a flat object with string,
// number, literal and (skipped) nested values is understood - exactly what
// a beacon is - and the first occurrence of a key wins. Written for size:
// no 64-bit arithmetic, one table of keys, one scratch buffer.
// ---------------------------------------------------------------------------
struct Scan {
    const char* p;
    const char* end;
};

static const size_t SCAN_BAD = (size_t)-1;
static const int    SCAN_MAX_DEPTH = 4;

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Skips whitespace; false when the input is exhausted.
static bool skip_ws(Scan& s) {
    while (s.p < s.end && is_ws(*s.p)) s.p++;
    return s.p < s.end;
}

// Parses a JSON string at s.p (which must be '"'). The unescaped characters
// are copied into dst (dst_len incl. NUL; extra characters are dropped, the
// full length is returned so callers can detect truncation). dst may be
// NULL to skip. Escapes: \" \\ \/ give the character, \b \f \n \r \t give
// '_' (never meaningful in a beacon), \u and anything else is a bad escape.
// Returns SCAN_BAD when unterminated, on a control character or a bad escape.
static size_t scan_string(Scan& s, char* dst, size_t dst_len) {
    if (s.p >= s.end || *s.p != '"') return SCAN_BAD;
    s.p++;
    size_t n = 0, total = 0;
    while (s.p < s.end) {
        char c = *s.p++;
        if (c == '"') {
            if (dst && dst_len) dst[n] = '\0';
            return total;
        }
        if ((unsigned char)c < 0x20) return SCAN_BAD;
        if (c == '\\') {
            if (s.p >= s.end) return SCAN_BAD;
            c = *s.p++;
            if (c != '"' && c != '\\' && c != '/') {
                if (!strchr("bfnrt", c)) return SCAN_BAD;
                c = '_';
            }
        }
        if (dst && n + 1 < dst_len) dst[n++] = c;
        total++;
    }
    return SCAN_BAD;                                 // unterminated
}

// Skips one value of any kind. Containers are skipped by string-aware
// bracket counting up to SCAN_MAX_DEPTH levels (the inside is not validated
// further - nothing in a beacon lives there); a scalar runs until the next
// separator. Returns false on depth overflow, an empty value or exhaustion.
static bool scan_skip(Scan& s) {
    int depth = 0;
    const char* start = s.p;
    while (s.p < s.end) {
        char c = *s.p;
        if (c == '"') {
            if (scan_string(s, nullptr, 0) == SCAN_BAD) return false;
            if (depth == 0) return true;
            continue;
        }
        if (c == '{' || c == '[') {
            if (++depth > SCAN_MAX_DEPTH) return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return s.p > start;
            if (--depth == 0) { s.p++; return true; }
        } else if (depth == 0 && (c == ',' || is_ws(c))) {
            return s.p > start;
        }
        s.p++;
    }
    return false;
}

// Non-negative magnitude with saturation (far above any field's range) and
// a sign flag; a fraction/exponent tail is skipped. A non-number value is
// skipped and reported as 0.
static bool take_number(Scan& s, uint32_t* mag, bool* neg) {
    *mag = 0;
    *neg = false;
    char c = *s.p;
    if (c == '-') { *neg = true; s.p++; }
    else if (c < '0' || c > '9') return scan_skip(s);
    if (s.p >= s.end || *s.p < '0' || *s.p > '9') return false;
    uint32_t v = 0;
    while (s.p < s.end && *s.p >= '0' && *s.p <= '9') {
        v = (v >= 400000000u) ? 0xFFFFFFFFu : v * 10 + (uint32_t)(*s.p - '0');
        s.p++;
    }
    while (s.p < s.end && strchr(".eE+-0123456789", *s.p)) s.p++;
    *mag = v;
    return true;
}

// String field into `raw`: a non-string value counts as absent ("" / 0).
static size_t take_string(Scan& s, char* raw, size_t raw_len) {
    raw[0] = '\0';
    if (*s.p != '"') return scan_skip(s) ? 0 : SCAN_BAD;
    return scan_string(s, raw, raw_len);
}

static bool is_hex_str(const char* s) {
    for (; *s; s++) {
        bool hex = (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F');
        if (!hex) return false;
    }
    return true;
}

// Keys in the order of the K_* enum; everything up to K_RN is a string.
static const char* const KEYS[] = { "t", "mac", "n", "ip", "v", "pw", "g", "tag", "nc", "rn", "sl", "seq", "rs", "ep" };
enum { K_T, K_MAC, K_N, K_IP, K_V, K_PW, K_G, K_TAG, K_NC, K_RN, K_SL, K_SEQ, K_RS, K_EP, K_COUNT };

bool beacon_parse(const char* data, size_t len, BeaconIn* out) {
    memset(out, 0, sizeof(*out));
    if (!data || len == 0 || len > BEACON_MAX_LEN) return false;
    Scan s = { data, data + len };
    if (!skip_ws(s) || *s.p != '{') return false;
    s.p++;

    char t[8] = "", mac[18] = "", pw[4] = "usb", raw[24];   // raw: the longest exact field is a 16-hex id
    uint16_t seen = 0;

    if (!skip_ws(s)) return false;
    if (*s.p == '}') {
        s.p++;
    } else {
        while (true) {
            if (!skip_ws(s)) return false;
            size_t kl = scan_string(s, raw, sizeof(raw));
            if (kl == SCAN_BAD) return false;
            if (!skip_ws(s) || *s.p != ':') return false;
            s.p++;
            if (!skip_ws(s)) return false;

            int k = 0;
            while (k < K_COUNT && strcmp(raw, KEYS[k]) != 0) k++;
            if (k == K_COUNT || (seen & (1u << k))) {
                if (!scan_skip(s)) return false;         // unknown or duplicate key: first wins
            } else if (k <= K_RN) {
                seen |= (uint16_t)(1u << k);
                size_t tl = take_string(s, raw, sizeof(raw));
                if (tl == SCAN_BAD) return false;
                switch (k) {
                    case K_T:   if (tl < sizeof(t)) strcpy(t, raw); break;
                    case K_MAC: if (tl < sizeof(mac)) strcpy(mac, raw); break;
                    case K_PW:  if (tl < sizeof(pw)) strcpy(pw, raw); break;
                    case K_N:   peer_sanitize_name(raw, out->name); break;
                    case K_IP:  if (tl >= 16 || !beacon_ip_parse(raw, &out->ip)) out->ip = 0; break;
                    case K_V:   out->version = peer_pack_version(raw); break;
                    case K_NC:  memcpy(out->nonce, raw, BEACON_NONCE_LEN - 1); out->nonce[BEACON_NONCE_LEN - 1] = '\0'; break;
                    case K_RN:  if (tl == 16 && is_hex_str(raw)) strcpy(out->relay_nonce, raw); break;   // exactly 16 hex, else ""
                    default: {                           // K_G, K_TAG: exactly hex, at most 16 chars, else ""
                        char* dst = k == K_G ? out->group_id : out->tag;
                        if (tl <= 16 && is_hex_str(raw)) strcpy(dst, raw);
                        break;
                    }
                }
            } else {
                seen |= (uint16_t)(1u << k);
                uint32_t v;
                bool neg;
                if (!take_number(s, &v, &neg)) return false;
                switch (k) {
                    case K_SL:  out->sleep_s = neg ? 0 : v; break;
                    case K_SEQ: out->seq = neg ? 0 : v; break;
                    case K_RS:  out->rssi = (int8_t)(neg ? -(int)(v > 127 ? 127 : v) : 0); break;
                    default:    out->epoch = (uint16_t)(neg ? 0 : (v > 0xFFFF ? 0xFFFF : v)); break;   // K_EP
                }
            }
            if (!skip_ws(s)) return false;
            if (*s.p == ',') { s.p++; continue; }
            if (*s.p == '}') { s.p++; break; }
            return false;
        }
    }
    while (s.p < s.end) {                            // nothing but whitespace after the object
        if (!is_ws(*s.p)) return false;
        s.p++;
    }

    if (strcmp(t, "tickr") == 0) out->kind = BEACON_ANNOUNCE;
    else if (strcmp(t, "tickr?") == 0) out->kind = BEACON_PROBE;
    else return false;
    if (!peer_mac_from_str(mac, out->mac)) { out->kind = BEACON_INVALID; return false; }

    // Keep nonces printable so they can be echoed verbatim in JSON.
    for (char* p = out->nonce; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\') *p = '_';
    }
    out->usb = strcmp(pw, "bat") != 0;
    return true;
}

void beacon_to_peer(const BeaconIn& b, uint32_t now_s, Peer* p) {
    memset(p, 0, sizeof(*p));
    memcpy(p->mac, b.mac, 6);
    memcpy(p->name, b.name, PEER_NAME_LEN);
    p->ip = b.ip;
    p->version = b.version;
    p->last_seen_s = now_s;
    p->next_wake_s = (!b.usb && b.sleep_s > 0) ? now_s + b.sleep_s : 0;
    p->flags = b.usb ? (uint8_t)(PEER_F_USB | PEER_F_PAIRABLE) : 0;
    p->epoch = (uint8_t)(b.epoch & 0xFF);
    p->rssi = b.rssi;
}

bool beacon_ip_parse(const char* s, uint32_t* out) {
    if (!s || !*s) return false;
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        char* end;
        unsigned long v = strtoul(s, &end, 10);
        if (end == s || v > 255) return false;
        if (i < 3 && *end != '.') return false;
        if (i == 3 && *end != '\0') return false;
        ip |= (uint32_t)v << (8 * i);
        s = end + 1;
    }
    *out = ip;
    return true;
}

void beacon_ip_format(uint32_t ip, char* out, size_t out_len) {
    snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}
