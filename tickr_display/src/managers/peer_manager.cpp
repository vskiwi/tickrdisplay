#include "peer_manager.h"
#include "../log.h"
#include "web_auth.h"
#include "relay_manager.h"
#include "../logic/peer_table.h"
#include "../logic/beacon.h"
#include "../logic/pairing.h"
#include "../logic/relay.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <memory>

// ---------------------------------------------------------------------------
// Constants / state
// ---------------------------------------------------------------------------
static const char PEERS_FILE[]     = "/peers.bin";
static const char PEERS_FILE_TMP[] = "/peers.bin.tmp";
static const char CT_JSON[]        = "application/json";
static const char PEERS_ROUTE_PREFIX[] = "/api/peers/";

static const uint32_t BEACON_BASE_S       = 45;      // docs/MULTI_DEVICE.md "Discovery: UDP beacons": T = clamp(45 * ceil(N/8), 45, 180)
static const uint32_t BEACON_MAX_S        = 180;
static const uint32_t BEACON_BOOT_MAX_MS  = 2000;    // first beacon 0-2 s after start
static const uint32_t PROBE_REPLY_MAX_MS  = 200;     // see docs/MULTI_DEVICE.md "Discovery: UDP beacons"
static const uint32_t STALE_CHECK_MS      = 10000;
// Staleness TTL = STALE_PERIODS × T. 3 T flickered after a single lost beacon
// (three jittered intervals reach 3.6 T); 4 T does not.
static const uint32_t STALE_PERIODS       = 4;
static const uint32_t SAVE_MIN_INTERVAL_MS = 10UL * 60UL * 1000UL;   // at most every 10 min
static const uint32_t SAVE_SETTLE_MS      = 30000;   // ... and only after 30 s without changes
static const uint32_t MANUAL_PROBE_TTL_MS = 15000;
static const int      RX_PER_LOOP         = 8;

static PeerTable s_table;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // table: main task writes, HTTP reads

static WiFiUDP  s_udp;
static bool     s_started = false;
static bool     s_battery = false;
static bool     s_listening = false;   // UDP socket open (USB always; battery only for the relay lookup)
static BeaconSelf s_self;
static uint32_t s_seq = 0;
static uint32_t s_next_beacon_ms = 0;
static uint32_t s_period_s = BEACON_BASE_S;
static uint32_t s_next_stale_ms = 0;
static uint32_t s_last_save_ms = 0;
static uint32_t s_last_change_ms = 0;
static bool     s_saved_once = false;

// Pending unicast reply to a probe (latest probe wins).
static bool      s_reply_pending = false;
static uint32_t  s_reply_at_ms = 0;
static IPAddress s_reply_ip;
static char      s_reply_nonce[BEACON_NONCE_LEN] = "";

// POST /api/peers {"ip"}: the HTTP task only records the request; the main
// task sends the probe and tags the answering peer MANUAL.
static volatile bool s_manual_pending = false;
static uint32_t  s_manual_ip = 0;
static uint32_t  s_manual_until_ms = 0;

// Group credentials for beacon tags (docs/MULTI_DEVICE.md "What the group secret signs"); written by
// the pairing manager (any task) under s_mux, read on the main task.
static bool     s_have_group = false;
static char     s_group_id[BEACON_HEX16_LEN] = "";
static uint16_t s_group_epoch = 0;
static uint8_t  s_group_secret[GROUP_SECRET_LEN];

// Diagnostics
static uint32_t s_sent = 0, s_send_failed = 0, s_received = 0, s_dropped = 0, s_verified = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t ip_to_u32(const IPAddress& ip) {
    return (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) | ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24);
}

static uint32_t uptime_s() {
    return millis() / 1000;
}

// Copies the group credentials under the lock; returns false without a group.
static bool group_snapshot(char id[BEACON_HEX16_LEN], uint16_t* epoch, uint8_t secret[GROUP_SECRET_LEN]) {
    portENTER_CRITICAL(&s_mux);
    bool have = s_have_group;
    if (have) {
        memcpy(id, s_group_id, BEACON_HEX16_LEN);
        *epoch = s_group_epoch;
        memcpy(secret, s_group_secret, GROUP_SECRET_LEN);
    }
    portEXIT_CRITICAL(&s_mux);
    return have;
}

static void refresh_self() {
    WiFi.macAddress(s_self.mac);
    strlcpy(s_self.ip, WiFi.localIP().toString().c_str(), sizeof(s_self.ip));
    s_self.rssi = (int8_t)WiFi.RSSI();
    if (s_self.name[0] == '\0') peers_default_name(s_self.name, sizeof(s_self.name));
    uint8_t secret[GROUP_SECRET_LEN];
    if (!group_snapshot(s_self.group_id, &s_self.epoch, secret)) {
        s_self.group_id[0] = '\0';
        s_self.epoch = 0;
    }
}

static void schedule_next_beacon() {
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = peer_table_fresh_count(&s_table, s_have_group);   // members once a group exists
    portEXIT_CRITICAL(&s_mux);
    uint32_t t = BEACON_BASE_S * (uint32_t)((n + 7) / 8);
    if (t < BEACON_BASE_S) t = BEACON_BASE_S;
    if (t > BEACON_MAX_S) t = BEACON_MAX_S;
    s_period_s = t;
    // +-20 % jitter: T * (0.8 .. 1.2)
    uint32_t base_ms = t * 1000;
    uint32_t jitter = base_ms / 5;
    uint32_t delta = jitter ? esp_random() % (2 * jitter + 1) : 0;
    s_next_beacon_ms = millis() + base_ms - jitter + delta;
}

static void send_datagram(const IPAddress& to, const char* buf, size_t n) {
    if (!s_udp.beginPacket(to, BEACON_PORT)) { s_send_failed++; return; }
    s_udp.write((const uint8_t*)buf, n);
    if (!s_udp.endPacket()) s_send_failed++;
}

static void send_beacon(const IPAddress& to, const char* nonce) {
    refresh_self();
    ++s_seq;
    char tag[BEACON_HEX16_LEN] = "", rn[BEACON_HEX16_LEN] = "";
    char id[BEACON_HEX16_LEN];
    uint16_t epoch;
    uint8_t secret[GROUP_SECRET_LEN];
    if (group_snapshot(id, &epoch, secret)) {
        char msg[BEACON_SIGN_MAX];
        size_t ml = beacon_sign_message(s_self, s_seq, msg, sizeof(msg));
        if (ml) beacon_tag(secret, msg, ml, tag);
        memset(secret, 0, sizeof(secret));
        // A probe reply from a relay carries a nonce for the prober's first signed request.
        if (nonce) relay_issue_nonce(rn);
    }
    char buf[BEACON_MAX_LEN + 1];
    size_t n = beacon_build(s_self, s_seq, nonce, tag, buf, sizeof(buf), rn);
    if (n == 0) return;
    send_datagram(to, buf, n);
    s_sent++;
}

static void send_probe(const IPAddress& to) {
    char nonce[BEACON_NONCE_LEN];
    snprintf(nonce, sizeof(nonce), "%08lx", (unsigned long)esp_random());
    char buf[96];
    size_t n = beacon_build_probe(s_self.mac, nonce, buf, sizeof(buf));
    if (n) send_datagram(to, buf, n);
}

static void handle_datagram(const char* data, size_t len, const IPAddress& from) {
    BeaconIn b;
    if (!beacon_parse(data, len, &b)) { s_dropped++; return; }
    if (memcmp(b.mac, s_self.mac, 6) == 0) return;          // our own broadcast echoed back

    if (b.kind == BEACON_PROBE) {
        s_reply_pending = true;
        s_reply_ip = from;
        s_reply_at_ms = millis() + esp_random() % (PROBE_REPLY_MAX_MS + 1);
        strlcpy(s_reply_nonce, b.nonce, sizeof(s_reply_nonce));
        return;
    }

    s_received++;
    Peer p;
    beacon_to_peer(b, uptime_s(), &p);
    uint32_t src = ip_to_u32(from);
    if (src) p.ip = src;                                     // the address we can actually reach
    // Membership (docs/MULTI_DEVICE.md "What the group secret signs"): same group id and epoch, tag verifies under our secret.
    char id[BEACON_HEX16_LEN];
    uint16_t epoch;
    uint8_t secret[GROUP_SECRET_LEN];
    if (b.tag[0] && group_snapshot(id, &epoch, secret)) {
        if (b.epoch == epoch && strcasecmp(b.group_id, id) == 0 && beacon_verify(b, secret)) {
            p.flags |= PEER_F_MEMBER;
            s_verified++;
        }
        memset(secret, 0, sizeof(secret));
    }
    if (s_manual_pending == false && s_manual_until_ms && p.ip == s_manual_ip &&
        (int32_t)(millis() - s_manual_until_ms) < 0) {
        p.flags |= PEER_F_MANUAL;
        s_manual_until_ms = 0;
    }
    portENTER_CRITICAL(&s_mux);
    Peer* stored = peer_table_upsert(&s_table, p);
    bool dirty = s_table.dirty;
    portEXIT_CRITICAL(&s_mux);
    if (!stored) {
        Serial.printf("[Peers] table full, beacon from %s ignored\n", from.toString().c_str());
        return;
    }
    if (dirty) s_last_change_ms = millis();
}

static void receive_all() {
    for (int i = 0; i < RX_PER_LOOP; i++) {
        int len = s_udp.parsePacket();
        if (len <= 0) return;
        IPAddress from = s_udp.remoteIP();
        if (len <= BEACON_MAX_LEN) {
            char buf[BEACON_MAX_LEN + 1];
            int got = s_udp.read((uint8_t*)buf, len);
            if (got > 0) {
                buf[got] = '\0';
                handle_datagram(buf, (size_t)got, from);
            }
        } else {
            s_dropped++;                                     // oversized: not even parsed
        }
        s_udp.flush();                                       // release the rx buffer
    }
}

static void save_now() {
    uint8_t* buf = (uint8_t*)malloc(PEERS_BLOB_MAX);
    if (!buf) return;
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = peer_table_save_blob(&s_table, buf, PEERS_BLOB_MAX);
    s_table.dirty = false;
    portEXIT_CRITICAL(&s_mux);
    bool ok = false;
    if (n) {
        File f = LittleFS.open(PEERS_FILE_TMP, "w");
        if (f) {
            ok = f.write(buf, n) == n;
            f.close();
            if (ok && !LittleFS.rename(PEERS_FILE_TMP, PEERS_FILE)) {
                LittleFS.remove(PEERS_FILE);
                ok = LittleFS.rename(PEERS_FILE_TMP, PEERS_FILE);
            }
            if (!ok) LittleFS.remove(PEERS_FILE_TMP);
        }
    }
    free(buf);
    s_last_save_ms = millis();
    s_saved_once = true;
    Serial.printf("[Peers] %s %u entries to %s\n", ok ? "saved" : "FAILED to save", (unsigned)(n ? (n - PEERS_BLOB_HEADER) / sizeof(Peer) : 0), PEERS_FILE);
}

static void load_saved() {
    if (!LittleFS.exists(PEERS_FILE)) return;
    File f = LittleFS.open(PEERS_FILE, "r");
    if (!f) return;
    size_t size = f.size();
    if (size < PEERS_BLOB_HEADER || size > (size_t)PEERS_BLOB_MAX) { f.close(); return; }
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) { f.close(); return; }
    size_t got = f.read(buf, size);
    f.close();
    PeerTable t;
    bool ok = got == size && peer_table_load_blob(&t, buf, size);
    free(buf);
    if (!ok) {
        Serial.println("[Peers] /peers.bin invalid, ignored");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_table = t;
    s_table.dirty = false;
    portEXIT_CRITICAL(&s_mux);
    LOGV("[Peers] loaded %u stale entries from %s\n", t.count, PEERS_FILE);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void peers_self_mac(uint8_t out[6]) {
    WiFi.macAddress(out);
}

void peers_self_id(char* out, size_t out_len) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char id[PEER_ID_LEN];
    peer_id_from_mac(mac, id);
    strlcpy(out, id, out_len);
}

void peers_default_name(char* out, size_t out_len) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(out, out_len, "Tickr-%02X%02X", mac[4], mac[5]);
}

void peers_self_name(char* out, size_t out_len) {
    if (s_self.name[0]) strlcpy(out, s_self.name, out_len);
    else peers_default_name(out, out_len);
}

void peers_set_name(const char* name) {
    if (name && *name) peer_sanitize_name(name, s_self.name);
    else peers_default_name(s_self.name, sizeof(s_self.name));
}

void peers_set_group(const char* id_hex, uint16_t epoch, const uint8_t* secret) {
    portENTER_CRITICAL(&s_mux);
    s_have_group = secret != nullptr && id_hex != nullptr && id_hex[0] != '\0';
    if (s_have_group) {
        strlcpy(s_group_id, id_hex, sizeof(s_group_id));
        s_group_epoch = epoch;
        memcpy(s_group_secret, secret, GROUP_SECRET_LEN);
    } else {
        s_group_id[0] = '\0';
        s_group_epoch = 0;
        memset(s_group_secret, 0, sizeof(s_group_secret));
    }
    // Membership is relative to *our* credentials: forget it until the next tagged beacon.
    for (size_t i = 0; i < s_table.count; i++) s_table.peers[i].flags &= (uint8_t)~PEER_F_MEMBER;
    portEXIT_CRITICAL(&s_mux);
}

size_t peers_member_count() {
    portENTER_CRITICAL(&s_mux);
    size_t n = peer_table_fresh_count(&s_table, true);
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool peers_group_snapshot(char id[BEACON_HEX16_LEN], uint16_t* epoch, uint8_t secret[GROUP_SECRET_LEN]) {
    return group_snapshot(id, epoch, secret);
}

bool peers_listening() {
    return s_started && !s_battery;
}

bool peers_is_member_id(const char* id) {
    bool member = false;
    portENTER_CRITICAL(&s_mux);
    Peer* p = peer_table_find_by_id(&s_table, id);
    if (p) member = (p->flags & PEER_F_MEMBER) != 0;      // stale members count: a sleeper's beacon is old by definition
    portEXIT_CRITICAL(&s_mux);
    return member;
}

// Battery side of the relay lookup (docs/MULTI_DEVICE.md "Relay for sleeping members"): one probe -
// unicast to the cached relay or broadcast - then the first reply that is a
// verified member on USB carrying a relay nonce wins. Blocking, main task.
bool peers_find_relay(uint32_t hint_ip, uint32_t* relay_ip, char nonce[BEACON_HEX16_LEN], uint32_t timeout_ms) {
    if (!s_started || !WiFi.isConnected()) return false;
    if (!s_listening) s_listening = s_udp.begin(BEACON_PORT);   // battery mode has no listener until now
    if (!s_listening) return false;
    refresh_self();
    send_probe(hint_ip ? IPAddress(hint_ip) : WiFi.broadcastIP());
    uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(millis() - deadline) < 0) {
        int len = s_udp.parsePacket();
        if (len <= 0) { delay(5); continue; }
        IPAddress from = s_udp.remoteIP();
        char buf[BEACON_MAX_LEN + 1];
        int got = len <= BEACON_MAX_LEN ? s_udp.read((uint8_t*)buf, len) : 0;
        s_udp.flush();
        BeaconIn b;
        if (got <= 0 || !beacon_parse(buf, (size_t)got, &b)) continue;
        uint32_t src = ip_to_u32(from);
        if (src) b.ip = src;
        char id[BEACON_HEX16_LEN];
        uint16_t epoch;
        uint8_t secret[GROUP_SECRET_LEN];
        bool verified = false;
        if (b.tag[0] && group_snapshot(id, &epoch, secret)) {
            verified = b.epoch == epoch && strcasecmp(b.group_id, id) == 0 && beacon_verify(b, secret);
            memset(secret, 0, sizeof(secret));
        }
        if (!relay_reply_usable(b, verified, s_self.mac)) continue;
        *relay_ip = b.ip;
        memcpy(nonce, b.relay_nonce, BEACON_HEX16_LEN);
        LOGV("[Peers] relay %s\n", from.toString().c_str());
        return true;
    }
    return false;
}

void peers_begin(const char* name, const char* version, bool battery, uint32_t sleep_s) {
    if (s_started) return;
    memset(&s_self, 0, sizeof(s_self));
    peers_set_name(name);
    strlcpy(s_self.version, version ? version : "", sizeof(s_self.version));
    s_self.usb = !battery;
    s_self.sleep_s = battery ? sleep_s : 0;
    s_battery = battery;
    refresh_self();
    portENTER_CRITICAL(&s_mux);
    peer_table_init(&s_table);
    portEXIT_CRITICAL(&s_mux);

    if (!battery) {
        load_saved();
        s_listening = s_udp.begin(BEACON_PORT);
        if (!s_listening) {
            Serial.println("[Peers] UDP begin failed");
        }
        s_next_beacon_ms = millis() + esp_random() % (BEACON_BOOT_MAX_MS + 1);
        s_next_stale_ms = millis() + STALE_CHECK_MS;
    }
    s_started = true;
    char id[PEER_ID_LEN];
    peer_id_from_mac(s_self.mac, id);
    LOGV("[Peers] %s '%s' %s, port %u, max %u peers\n", id, s_self.name,
                  battery ? "battery: one beacon" : "listening", BEACON_PORT, (unsigned)TICKR_MAX_PEERS);
}

void peers_send_beacon_now() {
    if (!WiFi.isConnected()) return;
    // The socket may not exist yet (battery mode never calls begin()).
    send_beacon(WiFi.broadcastIP(), nullptr);
}

void peers_loop() {
    if (!s_started || s_battery) return;
    uint32_t now = millis();
    bool up = WiFi.isConnected();

    receive_all();

    if (up && (int32_t)(now - s_next_beacon_ms) >= 0) {
        bool first = s_seq == 0;
        if (first) send_probe(WiFi.broadcastIP());          // running peers answer within ~200 ms
        send_beacon(WiFi.broadcastIP(), nullptr);
        schedule_next_beacon();
    }
    if (up && s_reply_pending && (int32_t)(now - s_reply_at_ms) >= 0) {
        s_reply_pending = false;
        send_beacon(s_reply_ip, s_reply_nonce);
    }
    if (up && s_manual_pending) {
        s_manual_pending = false;
        IPAddress ip(s_manual_ip);
        s_manual_until_ms = now + MANUAL_PROBE_TTL_MS;
        send_probe(ip);
    }
    if ((int32_t)(now - s_next_stale_ms) >= 0) {
        s_next_stale_ms = now + STALE_CHECK_MS;
        portENTER_CRITICAL(&s_mux);
        size_t changed = peer_table_mark_stale(&s_table, uptime_s(), STALE_PERIODS * s_period_s);
        portEXIT_CRITICAL(&s_mux);
        if (changed) s_last_change_ms = now;                 // STALE is part of the persisted view
    }
    // Persistence: dirty, settled for 30 s, and not more often than every 10 min.
    if (s_table.dirty && (int32_t)(now - s_last_change_ms) >= (int32_t)SAVE_SETTLE_MS &&
        (!s_saved_once || (int32_t)(now - s_last_save_ms) >= (int32_t)SAVE_MIN_INTERVAL_MS)) {
        save_now();
    }
}

void peers_flush() {
    if (s_started && !s_battery && s_table.dirty) save_now();
}

void peers_stats_json(char* out, size_t out_len) {
    snprintf(out, out_len,
             "{\"count\":%u,\"fresh\":%u,\"members\":%u,\"sent\":%lu,\"send_failed\":%lu,\"received\":%lu,\"verified\":%lu,\"dropped\":%lu,"
             "\"period_s\":%lu,\"broadcast\":\"%s\",\"running\":%s}",
             (unsigned)s_table.count, (unsigned)peer_table_fresh_count(&s_table), (unsigned)peer_table_fresh_count(&s_table, true),
             (unsigned long)s_sent, (unsigned long)s_send_failed, (unsigned long)s_received, (unsigned long)s_verified,
             (unsigned long)s_dropped, (unsigned long)s_period_s, WiFi.broadcastIP().toString().c_str(),
             (s_started && !s_battery) ? "true" : "false");
}

// ---------------------------------------------------------------------------
// HTTP: GET /api/peers (streamed), DELETE /api/peers/<id>, POST /api/peers
// ---------------------------------------------------------------------------
// Formats one entry. Returns the length or 0 if it does not fit.
static size_t format_peer(const Peer& p, uint32_t now_s, char* out, size_t cap) {
    char id[PEER_ID_LEN], ip[16], ver[12];
    peer_id_from_mac(p.mac, id);
    beacon_ip_format(p.ip, ip, sizeof(ip));
    peer_unpack_version(p.version, ver, sizeof(ver));
    bool stale = p.flags & PEER_F_STALE;
    bool from_file = stale && p.last_seen_s == 0;
    char seen[16];
    if (from_file) strcpy(seen, "null");
    else snprintf(seen, sizeof(seen), "%lu", (unsigned long)(now_s - p.last_seen_s));
    uint32_t next_wake = (p.next_wake_s > now_s) ? p.next_wake_s - now_s : 0;
    int n = snprintf(out, cap,
        "{\"id\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"name\":\"%s\",\"ip\":\"%s\","
        "\"version\":\"%s\",\"power\":\"%s\",\"member\":%s,\"stale\":%s,\"manual\":%s,"
        "\"last_seen_s\":%s,\"next_wake_s\":%lu,\"rssi\":%d,\"epoch\":%u}",
        id, p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5], p.name, ip, ver,
        (p.flags & PEER_F_USB) ? "usb" : "battery",
        (p.flags & PEER_F_MEMBER) ? "true" : "false", stale ? "true" : "false",
        (p.flags & PEER_F_MANUAL) ? "true" : "false",
        seen, (unsigned long)next_wake, (int)p.rssi, (unsigned)p.epoch);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

struct PeersStream {
    size_t   idx = 0;
    uint32_t now_s = 0;
    bool     done = false;
};

// GET /api/peers - public: everything in it is broadcast in the beacons.
static void handle_peers_get(AsyncWebServerRequest* request) {
    auto st = std::make_shared<PeersStream>();
    st->now_s = uptime_s();
    // Chunked: one JSON entry per 40-byte snapshot; the whole array is never in RAM.
    AsyncWebServerResponse* r = request->beginChunkedResponse(CT_JSON,
        [st](uint8_t* buf, size_t maxLen, size_t) -> size_t {
            if (st->done) return 0;
            size_t n = 0;
            if (st->idx == 0) buf[n++] = '[';
            while (true) {
                Peer p;
                bool have = false;
                portENTER_CRITICAL(&s_mux);
                if (st->idx < s_table.count) { p = s_table.peers[st->idx]; have = true; }
                portEXIT_CRITICAL(&s_mux);
                if (!have) break;
                char entry[288];
                size_t len = format_peer(p, st->now_s, entry, sizeof(entry));
                if (len == 0) { st->idx++; continue; }
                size_t need = len + (st->idx > 0 ? 1 : 0);
                if (n + need > maxLen) {
                    if (n == 0) return RESPONSE_TRY_AGAIN;    // wait for a bigger window
                    return n;
                }
                if (st->idx > 0) buf[n++] = ',';
                memcpy(buf + n, entry, len);
                n += len;
                st->idx++;
            }
            if (n + 1 > maxLen) return n ? n : RESPONSE_TRY_AGAIN;
            buf[n++] = ']';
            st->done = true;
            return n;
        });
    r->addHeader("Cache-Control", "no-cache");
    request->send(r);
}

class PeerDeleteHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        return request->method() == HTTP_DELETE && request->url().startsWith(PEERS_ROUTE_PREFIX);
    }
    void handleRequest(AsyncWebServerRequest* request) override {
        REQUIRE_GROUP_AUTH(request);
        String id = request->url().substring(strlen(PEERS_ROUTE_PREFIX));
        while (id.endsWith("/")) id.remove(id.length() - 1);
        // "tickr-XXXXXX" -> last three MAC bytes, parsed outside the critical section.
        uint8_t tail[6];
        String hex = "000000" + id.substring(6);
        bool valid = id.startsWith("tickr-") && id.length() == 12 && peer_mac_from_str(hex.c_str(), tail);
        bool removed = false;
        if (valid) {
            portENTER_CRITICAL(&s_mux);
            for (size_t i = 0; i < s_table.count; i++) {
                if (memcmp(s_table.peers[i].mac + 3, tail + 3, 3) == 0) {
                    removed = peer_table_remove(&s_table, i);
                    break;
                }
            }
            portEXIT_CRITICAL(&s_mux);
        }
        if (!removed) {
            request->send(404, CT_JSON, "{\"error\":\"unknown peer id\"}");
            return;
        }
        s_last_change_ms = millis();
        request->send(200, CT_JSON, "{\"ok\":true}");
    }
};

static void collect_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    const size_t kMax = 256;
    if (total == 0 || total > kMax) return;
    if (index == 0) request->_tempObject = calloc(total + 1, 1);   // freed by ~AsyncWebServerRequest
    if (request->_tempObject && index + len <= total) memcpy((uint8_t*)request->_tempObject + index, data, len);
}

static void handle_peers_post(AsyncWebServerRequest* request) {
    REQUIRE_GROUP_AUTH(request);
    String ip;
    if (request->hasParam("ip", true)) ip = request->getParam("ip", true)->value();
    else if (request->_tempObject) {
        // Minimal extraction of "ip":"..." without a JsonDocument.
        const char* body = (const char*)request->_tempObject;
        const char* k = strstr(body, "\"ip\"");
        const char* q = k ? strchr(k + 4, '"') : nullptr;
        const char* e = q ? strchr(q + 1, '"') : nullptr;
        if (q && e) ip = String(q + 1).substring(0, (unsigned)(e - q - 1));
    }
    uint32_t addr;
    if (!beacon_ip_parse(ip.c_str(), &addr) || addr == 0) {
        request->send(400, CT_JSON, "{\"error\":\"body {\\\"ip\\\":\\\"a.b.c.d\\\"} required\"}");
        return;
    }
    if (!s_started || s_battery) {
        request->send(503, CT_JSON, "{\"error\":\"discovery not running\"}");
        return;
    }
    s_manual_ip = addr;
    s_manual_pending = true;                                 // main task sends the probe
    String body = "{\"status\":\"probing\",\"ip\":\"" + ip + "\"}";
    request->send(202, CT_JSON, body);
}

void peers_register_routes(AsyncWebServer& server) {
    server.on("/api/peers", HTTP_GET, handle_peers_get);
    server.on("/api/peers", HTTP_POST, handle_peers_post, nullptr, collect_body);
    server.addHandler(new PeerDeleteHandler());
}
