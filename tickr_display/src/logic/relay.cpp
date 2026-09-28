#include "relay.h"
#include "pairing.h"
#include "peer_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Nonce store
// ---------------------------------------------------------------------------
void relay_nonce_init(RelayNonceStore* s) {
    memset(s, 0, sizeof(*s));
}

void relay_nonce_issue(RelayNonceStore* s, uint32_t now_s, uint32_t (*rng)(), char out[RELAY_NONCE_LEN]) {
    // Prefer a free slot, else the round-robin victim (the oldest in steady state).
    int i = -1;
    for (int k = 0; k < RELAY_NONCE_SLOTS; k++) {
        if (s->slots[k].nonce[0] == '\0') { i = k; break; }
    }
    if (i < 0) { i = s->next; s->next = (uint8_t)((s->next + 1) % RELAY_NONCE_SLOTS); }
    snprintf(s->slots[i].nonce, RELAY_NONCE_LEN, "%08lx%08lx", (unsigned long)rng(), (unsigned long)rng());
    s->slots[i].issued_s = now_s;
    memcpy(out, s->slots[i].nonce, RELAY_NONCE_LEN);
}

bool relay_nonce_consume(RelayNonceStore* s, const char* nonce, uint32_t now_s) {
    if (!nonce || strlen(nonce) != RELAY_NONCE_LEN - 1) return false;
    for (int k = 0; k < RELAY_NONCE_SLOTS; k++) {
        RelayNonce& n = s->slots[k];
        if (n.nonce[0] && strcmp(n.nonce, nonce) == 0) {
            bool fresh = now_s - n.issued_s <= RELAY_NONCE_TTL_S;
            n.nonce[0] = '\0';                               // single use
            return fresh;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Signature
// ---------------------------------------------------------------------------
size_t relay_sign_message(const char* method, const char* path, const char* nonce,
                          const uint8_t body_sha256[32], char* out, size_t cap) {
    char h[65];
    pairing_hex(body_sha256, 32, h);
    int n = snprintf(out, cap, "tickr-relay-v1\n%s\n%s\n%s\n%s", method, path, nonce, h);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

void relay_sign(const uint8_t secret[32], const char* msg, size_t msg_len, char out[RELAY_SIG_LEN]) {
    uint8_t mac[32];
    if (g_hmac_sha256) g_hmac_sha256(secret, GROUP_SECRET_LEN, (const uint8_t*)msg, msg_len, mac);
    else memset(mac, 0, sizeof(mac));
    pairing_hex(mac, 32, out);
}

size_t relay_header_build(const char* group_id, uint16_t epoch, const char* nonce, const char* sig_hex,
                          char* out, size_t cap) {
    int n = snprintf(out, cap, "%.16s:%u:%.16s:%.64s", group_id, (unsigned)epoch, nonce, sig_hex);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

size_t relay_header_make(const char* group_id, uint16_t epoch, const uint8_t secret[32], const char* nonce,
                         const char* method, const char* path, const uint8_t body_sha256[32], char* out, size_t cap) {
    char msg[RELAY_SIGN_MAX], sig[RELAY_SIG_LEN];
    size_t ml = relay_sign_message(method, path, nonce, body_sha256, msg, sizeof(msg));
    if (!ml) return 0;
    relay_sign(secret, msg, ml, sig);
    return relay_header_build(group_id, epoch, nonce, sig, out, cap);
}

// Copies a field of exactly `want` hex characters ending at ':' or NUL.
static const char* take_hex(const char* p, size_t want, char* dst) {
    size_t n = 0;
    while (p[n] && p[n] != ':') n++;
    if (n != want) return nullptr;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return nullptr;
        if (dst) dst[i] = c;
    }
    if (dst) dst[n] = '\0';
    return p + n;
}

bool relay_header_parse(const char* value, RelayHeader* out) {
    memset(out, 0, sizeof(*out));
    if (!value || strlen(value) >= RELAY_HDR_MAX) return false;
    const char* p = take_hex(value, 16, out->group_id);
    if (!p || *p != ':') return false;
    p++;
    // epoch: 1..5 decimal digits, <= 65535
    size_t n = 0;
    unsigned long e = 0;
    while (p[n] >= '0' && p[n] <= '9' && n < 6) { e = e * 10 + (unsigned long)(p[n] - '0'); n++; }
    if (n == 0 || n > 5 || p[n] != ':' || e > 0xFFFF) return false;
    out->epoch = (uint16_t)e;
    p += n + 1;
    p = take_hex(p, 16, out->nonce);
    if (!p || *p != ':') return false;
    p++;
    char sig[RELAY_SIG_LEN];
    p = take_hex(p, 64, sig);
    if (!p || *p != '\0') return false;
    return pairing_unhex(sig, out->mac, 32);
}

bool relay_verify(const RelayHeader& h, const char* group_id, uint16_t epoch, const uint8_t secret[32],
                  const char* method, const char* path, const uint8_t body_sha256[32]) {
    if (!group_id || !group_id[0] || h.epoch != epoch || strcasecmp(h.group_id, group_id) != 0) return false;
    char msg[RELAY_SIGN_MAX];
    size_t ml = relay_sign_message(method, path, h.nonce, body_sha256, msg, sizeof(msg));
    if (!ml) return false;
    uint8_t want[32];
    if (g_hmac_sha256) g_hmac_sha256(secret, GROUP_SECRET_LEN, (const uint8_t*)msg, ml, want);
    else return false;
    return pairing_ct_equal(h.mac, want, 32);
}

// ---------------------------------------------------------------------------
// Routes and files
// ---------------------------------------------------------------------------
// "tickr-XXXXXX" (upper-case hex) at p, followed by `after` (may be "") and then NUL.
static bool take_id(const char* p, const char* after, char id[RELAY_ID_LEN]) {
    if (strncmp(p, "tickr-", 6) != 0) return false;
    for (int i = 6; i < 12; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    if (strcmp(p + 12, after) != 0) return false;
    memcpy(id, p, 12);
    id[12] = '\0';
    return true;
}

RelayRoute relay_route(const char* path, char id[RELAY_ID_LEN]) {
    id[0] = '\0';
    if (!path) return RELAY_ROUTE_NONE;
    if (strcmp(path, "/api/relay") == 0) return RELAY_ROUTE_LIST;
    if (strncmp(path, "/api/relay/", 11) == 0) {
        if (take_id(path + 11, "", id)) return RELAY_ROUTE_PAYLOAD;
        if (take_id(path + 11, "/ota", id)) return RELAY_ROUTE_OTA;
        return RELAY_ROUTE_NONE;
    }
    if (strncmp(path, "/api/peers/", 11) == 0 && take_id(path + 11, "/screen", id)) return RELAY_ROUTE_SCREEN;
    return RELAY_ROUTE_NONE;
}

static const char* const ROUTE_FMT[] = { nullptr, "/api/relay", "/api/relay/%s", "/api/relay/%s/ota", "/api/peers/%s/screen" };
static const char* const FILE_FMT[]  = { nullptr, nullptr, "/relay/%s.json", "/relay/%s.ota.json", "/peers/%s.raw" };

size_t relay_route_path(RelayRoute r, const char* id, char* out, size_t cap) {
    if (r == RELAY_ROUTE_NONE || r > RELAY_ROUTE_SCREEN) { if (cap) out[0] = '\0'; return 0; }
    int n = snprintf(out, cap, ROUTE_FMT[r], id);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

bool relay_file_name(RelayRoute r, const char* id, char* out, size_t cap) {
    if (r < RELAY_ROUTE_PAYLOAD || r > RELAY_ROUTE_SCREEN) { if (cap) out[0] = '\0'; return false; }
    int n = snprintf(out, cap, FILE_FMT[r], id);
    return n > 0 && (size_t)n < cap;
}

RelayRoute relay_file_route(const char* name, char id[RELAY_ID_LEN]) {
    id[0] = '\0';
    if (!name) return RELAY_ROUTE_NONE;
    const char* slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    if (take_id(name, ".ota.json", id)) return RELAY_ROUTE_OTA;
    if (take_id(name, ".json", id)) return RELAY_ROUTE_PAYLOAD;
    if (take_id(name, ".raw", id)) return RELAY_ROUTE_SCREEN;
    return RELAY_ROUTE_NONE;
}

bool relay_url_ok(const char* url) {
    if (!url) return false;
    size_t n = strlen(url);
    if (n < 8 || n >= RELAY_URL_MAX) return false;
    if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) return false;
    for (const char* p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

bool relay_url_from_json(const char* body, char* out, size_t cap) {
    if (cap) out[0] = '\0';
    const char* k = body ? strstr(body, "\"url\"") : nullptr;
    const char* q = k ? strchr(k + 5, '"') : nullptr;
    const char* e = q ? strchr(q + 1, '"') : nullptr;
    if (!q || !e) return false;
    size_t n = (size_t)(e - q - 1);
    if (n + 1 > cap) return false;
    memcpy(out, q + 1, n);
    out[n] = '\0';
    return relay_url_ok(out);
}

// ---------------------------------------------------------------------------
// The sleeper's decisions
// ---------------------------------------------------------------------------
RelaySource relay_pick_source(bool has_pull_url, bool has_group) {
    if (has_pull_url) return RELAY_SRC_PULL_URL;
    return has_group ? RELAY_SRC_RELAY : RELAY_SRC_NONE;
}

bool relay_report_frame(uint32_t interval_min) {
    return interval_min >= RELAY_REPORT_MIN_INTERVAL_MIN;
}

bool relay_reply_usable(const BeaconIn& b, bool verified, const uint8_t self_mac[6]) {
    if (b.kind != BEACON_ANNOUNCE || !verified || !b.usb) return false;
    if (strlen(b.relay_nonce) != RELAY_NONCE_LEN - 1) return false;
    if (memcmp(b.mac, self_mac, 6) == 0) return false;        // never relay through ourselves
    return b.ip != 0;
}

void relay_flow_init(RelayFlow* f, bool has_hint, bool report) {
    memset(f, 0, sizeof(*f));
    f->has_hint = has_hint;
    f->report = report;
    f->step = has_hint ? RELAY_STEP_PROBE_HINT : RELAY_STEP_PROBE_BCAST;
}

void relay_flow_next(RelayFlow* f, bool ok, bool has_payload, bool has_ota) {
    switch (f->step) {
        case RELAY_STEP_PROBE_HINT:
            f->probes++;
            f->step = ok ? RELAY_STEP_FETCH : RELAY_STEP_PROBE_BCAST;
            break;
        case RELAY_STEP_PROBE_BCAST:
            f->probes++;
            f->step = ok ? RELAY_STEP_FETCH : RELAY_STEP_NO_RELAY;
            break;
        case RELAY_STEP_FETCH:
            if (!ok) { f->step = RELAY_STEP_FAILED; break; }
            f->got_payload = has_payload;
            f->got_ota = has_ota;
            f->step = has_payload ? RELAY_STEP_APPLY : has_ota ? RELAY_STEP_OTA : RELAY_STEP_DONE;
            break;
        case RELAY_STEP_APPLY:
            // A rejected payload is not a relay failure; the OTA job still runs.
            if (!ok) f->got_payload = false;
            f->step = f->got_ota ? RELAY_STEP_OTA : (f->got_payload && f->report) ? RELAY_STEP_UPLOAD : RELAY_STEP_DONE;
            break;
        case RELAY_STEP_OTA:
            // ok = the image was flashed: the caller restarts, nothing follows.
            f->step = ok ? RELAY_STEP_DONE : (f->got_payload && f->report) ? RELAY_STEP_UPLOAD : RELAY_STEP_DONE;
            break;
        case RELAY_STEP_UPLOAD:
            f->step = RELAY_STEP_DONE;                        // best effort either way
            break;
        default:
            break;                                            // terminal states stay
    }
}
