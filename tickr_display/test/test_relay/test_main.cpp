// Host tests for the relay logic (src/logic/relay.cpp, docs/MULTI_DEVICE.md
// "Relay for sleeping members"): request signature and its header, the
// single-use nonce store, route <-> file mapping, the sleeper's decisions and
// the wake-up flow. HMAC-SHA-256 from the reference implementation of the
// pairing tests. Also the beacon's "rn" field and the layout "groups" key.
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "../test_pairing/sha256_ref.h"
#include "logic/relay.h"
#include "logic/pairing.h"
#include "logic/beacon.h"
#include "logic/layout.h"

void setUp() {}
void tearDown() {}

static const char GID[] = "1111111111111111";
static uint8_t SECRET[32];
static uint8_t EMPTY_SHA[32];            // SHA-256("")
static uint32_t s_rng_state = 1;
static uint32_t rng() { s_rng_state = s_rng_state * 1103515245u + 12345u; return s_rng_state; }

// ---------------------------------------------------------------------------
// Nonce store
// ---------------------------------------------------------------------------
static void test_nonce_issue_and_single_use() {
    RelayNonceStore st;
    relay_nonce_init(&st);
    char n[RELAY_NONCE_LEN];
    relay_nonce_issue(&st, 100, rng, n);
    TEST_ASSERT_EQUAL_UINT(16, strlen(n));
    for (const char* p = n; *p; p++) TEST_ASSERT_TRUE((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
    TEST_ASSERT_TRUE(relay_nonce_consume(&st, n, 110));
    TEST_ASSERT_FALSE_MESSAGE(relay_nonce_consume(&st, n, 111), "a nonce is single use");
}

static void test_nonce_ttl_and_unknown() {
    RelayNonceStore st;
    relay_nonce_init(&st);
    char n[RELAY_NONCE_LEN];
    relay_nonce_issue(&st, 100, rng, n);
    TEST_ASSERT_FALSE_MESSAGE(relay_nonce_consume(&st, n, 100 + RELAY_NONCE_TTL_S + 1), "expired");
    relay_nonce_issue(&st, 200, rng, n);
    TEST_ASSERT_TRUE_MESSAGE(relay_nonce_consume(&st, n, 200 + RELAY_NONCE_TTL_S), "exactly TTL is still fresh");
    TEST_ASSERT_FALSE(relay_nonce_consume(&st, "0123456789abcdef", 200));   // never issued
    TEST_ASSERT_FALSE(relay_nonce_consume(&st, "", 200));
    TEST_ASSERT_FALSE(relay_nonce_consume(&st, nullptr, 200));
    TEST_ASSERT_FALSE(relay_nonce_consume(&st, "0123456789abcde", 200));    // 15 chars
}

static void test_nonce_store_overwrites_oldest() {
    RelayNonceStore st;
    relay_nonce_init(&st);
    char n[RELAY_NONCE_SLOTS + 1][RELAY_NONCE_LEN];
    for (int i = 0; i <= RELAY_NONCE_SLOTS; i++) relay_nonce_issue(&st, 100 + i, rng, n[i]);
    TEST_ASSERT_FALSE_MESSAGE(relay_nonce_consume(&st, n[0], 110), "the first one was evicted");
    for (int i = 1; i <= RELAY_NONCE_SLOTS; i++) TEST_ASSERT_TRUE(relay_nonce_consume(&st, n[i], 110));
    // all consumed: the store is empty again
    for (int i = 1; i <= RELAY_NONCE_SLOTS; i++) TEST_ASSERT_FALSE(relay_nonce_consume(&st, n[i], 110));
}

// ---------------------------------------------------------------------------
// Signature
// ---------------------------------------------------------------------------
static void test_sign_message_canonical_form() {
    char msg[RELAY_SIGN_MAX];
    size_t n = relay_sign_message("GET", "/api/relay/tickr-A1B2C3", "0123456789abcdef", EMPTY_SHA, msg, sizeof(msg));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("tickr-relay-v1\nGET\n/api/relay/tickr-A1B2C3\n0123456789abcdef\n"
                             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", msg);
    char small[40];
    TEST_ASSERT_EQUAL_UINT(0, relay_sign_message("GET", "/api/relay/tickr-A1B2C3", "0123456789abcdef", EMPTY_SHA, small, sizeof(small)));
}

static void test_header_roundtrip_and_verify() {
    char hdr[RELAY_HDR_MAX];
    size_t n = relay_header_make(GID, 3, SECRET, "0123456789abcdef", "GET", "/api/relay/tickr-A1B2C3", EMPTY_SHA, hdr, sizeof(hdr));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT(16 + 1 + 1 + 1 + 16 + 1 + 64, n);
    RelayHeader h;
    TEST_ASSERT_TRUE(relay_header_parse(hdr, &h));
    TEST_ASSERT_EQUAL_STRING(GID, h.group_id);
    TEST_ASSERT_EQUAL_UINT16(3, h.epoch);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", h.nonce);
    TEST_ASSERT_TRUE(relay_verify(h, GID, 3, SECRET, "GET", "/api/relay/tickr-A1B2C3", EMPTY_SHA));
    // Fixed vector (independent: python hmac over the canonical message with a 32 x 0x22 key)
    uint8_t k22[32];
    memset(k22, 0x22, sizeof(k22));
    char msg[RELAY_SIGN_MAX], sig[RELAY_SIG_LEN];
    size_t ml = relay_sign_message("GET", "/api/relay/tickr-A1B2C3", "0123456789abcdef", EMPTY_SHA, msg, sizeof(msg));
    relay_sign(k22, msg, ml, sig);
    TEST_ASSERT_EQUAL_UINT(64, strlen(sig));
    TEST_ASSERT_EQUAL_STRING("9c720e365347b200660ec3dab5c9a1d258ddfeffdbbb209eb5b1138aa1519ba6", sig);
}

static void test_verify_rejects_every_mismatch() {
    char hdr[RELAY_HDR_MAX];
    relay_header_make(GID, 3, SECRET, "0123456789abcdef", "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA, hdr, sizeof(hdr));
    RelayHeader h;
    TEST_ASSERT_TRUE(relay_header_parse(hdr, &h));
    TEST_ASSERT_TRUE(relay_verify(h, GID, 3, SECRET, "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA));
    uint8_t other[32];
    memcpy(other, SECRET, 32);
    other[5] ^= 1;
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, GID, 3, other, "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA), "wrong secret");
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, GID, 4, SECRET, "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA), "other epoch");
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, "2222222222222222", 3, SECRET, "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA), "other group");
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, "", 3, SECRET, "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA), "no group");
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, GID, 3, SECRET, "GET", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA), "other method");
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, GID, 3, SECRET, "PUT", "/api/peers/tickr-A1B2C4/screen", EMPTY_SHA), "other path");
    uint8_t body[32];
    memcpy(body, EMPTY_SHA, 32);
    body[0] ^= 0x80;
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, GID, 3, SECRET, "PUT", "/api/peers/tickr-A1B2C3/screen", body), "other body");
    h.mac[31] ^= 1;
    TEST_ASSERT_FALSE_MESSAGE(relay_verify(h, GID, 3, SECRET, "PUT", "/api/peers/tickr-A1B2C3/screen", EMPTY_SHA), "flipped mac bit");
    // upper-case group id in the header is accepted (hex is case-insensitive)
    char up[RELAY_HDR_MAX];
    relay_header_make("abcdefabcdefabcd", 1, SECRET, "0123456789abcdef", "GET", "/api/relay/tickr-A1B2C3", EMPTY_SHA, up, sizeof(up));
    RelayHeader hu;
    TEST_ASSERT_TRUE(relay_header_parse(up, &hu));
    TEST_ASSERT_TRUE(relay_verify(hu, "ABCDEFABCDEFABCD", 1, SECRET, "GET", "/api/relay/tickr-A1B2C3", EMPTY_SHA));
}

static void test_header_parse_rejects_malformed() {
    RelayHeader h;
    const char SIG[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char ok[RELAY_HDR_MAX];
    snprintf(ok, sizeof(ok), "%s:65535:fedcba9876543210:%s", GID, SIG);
    TEST_ASSERT_TRUE(relay_header_parse(ok, &h));
    TEST_ASSERT_EQUAL_UINT16(65535, h.epoch);
    // "%s" stands for a well-formed 64-hex signature
    const char* bad[] = {
        "", ":", "1111111111111111", "1111111111111111:3", "1111111111111111:3:fedcba9876543210",
        "1111111111111111:3:fedcba9876543210:",                      // empty signature
        "111111111111111:3:fedcba9876543210:%s" ,                    // 15-char group id
        "11111111111111111:3:fedcba9876543210:%s",                   // 17-char group id
        "111111111111111g:3:fedcba9876543210:%s",                    // non-hex id
        "1111111111111111::fedcba9876543210:%s",                     // empty epoch
        "1111111111111111:65536:fedcba9876543210:%s",                // epoch > 16 bit
        "1111111111111111:-1:fedcba9876543210:%s",
        "1111111111111111:3x:fedcba9876543210:%s",
        "1111111111111111:3:fedcba987654321:%s",                     // 15-char nonce
        "1111111111111111:3:fedcba9876543210z:%s",                   // non-hex nonce
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char buf[RELAY_HDR_MAX + 80];
        snprintf(buf, sizeof(buf), bad[i], SIG);
        TEST_ASSERT_FALSE_MESSAGE(relay_header_parse(buf, &h), buf);
    }
    char sig63[RELAY_HDR_MAX], sig65[RELAY_HDR_MAX + 4], tail[RELAY_HDR_MAX + 4], gsig[RELAY_HDR_MAX];
    snprintf(sig63, sizeof(sig63), "%s:3:fedcba9876543210:%.63s", GID, SIG);
    snprintf(sig65, sizeof(sig65), "%s:3:fedcba9876543210:%s0", GID, SIG);
    snprintf(tail, sizeof(tail), "%s:3:fedcba9876543210:%s:x", GID, SIG);
    snprintf(gsig, sizeof(gsig), "%s:3:fedcba9876543210:%.63sG", GID, SIG);
    TEST_ASSERT_FALSE_MESSAGE(relay_header_parse(sig63, &h), "63-char signature");
    TEST_ASSERT_FALSE_MESSAGE(relay_header_parse(sig65, &h), "65-char signature");
    TEST_ASSERT_FALSE_MESSAGE(relay_header_parse(tail, &h), "trailing field");
    TEST_ASSERT_FALSE_MESSAGE(relay_header_parse(gsig, &h), "non-hex signature");
    TEST_ASSERT_FALSE(relay_header_parse(nullptr, &h));
    char huge[RELAY_HDR_MAX + 40];
    memset(huge, '1', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    TEST_ASSERT_FALSE_MESSAGE(relay_header_parse(huge, &h), "over-long header");
}

// ---------------------------------------------------------------------------
// Routes and files
// ---------------------------------------------------------------------------
static void test_route_classification() {
    char id[RELAY_ID_LEN];
    TEST_ASSERT_EQUAL(RELAY_ROUTE_LIST, relay_route("/api/relay", id));
    TEST_ASSERT_EQUAL_STRING("", id);
    TEST_ASSERT_EQUAL(RELAY_ROUTE_PAYLOAD, relay_route("/api/relay/tickr-A1B2C3", id));
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", id);
    TEST_ASSERT_EQUAL(RELAY_ROUTE_OTA, relay_route("/api/relay/tickr-A1B2C3/ota", id));
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", id);
    TEST_ASSERT_EQUAL(RELAY_ROUTE_SCREEN, relay_route("/api/peers/tickr-0F0F0F/screen", id));
    TEST_ASSERT_EQUAL_STRING("tickr-0F0F0F", id);
    const char* none[] = {
        "/api/relay/", "/api/relay/tickr-a1b2c3", "/api/relay/tickr-A1B2C", "/api/relay/tickr-A1B2C3/", "/api/relay/tickr-A1B2C3/x",
        "/api/relay/tickr-A1B2C3/ota/", "/api/relay/kitchen", "/api/relay/tickr-A1B2C3G", "/api/peers", "/api/peers/tickr-A1B2C3",
        "/api/peers/tickr-A1B2C3/screen/", "/api/peers/tickr-A1B2C3/raw", "/api/relays", "/api/relay/../tickr-A1B2C3", nullptr,
    };
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(RELAY_ROUTE_NONE, relay_route(none[i], id), none[i] ? none[i] : "null");
        TEST_ASSERT_EQUAL_STRING("", id);
    }
}

static void test_route_paths_and_files() {
    char p[RELAY_PATH_MAX], f[48], id[RELAY_ID_LEN];
    TEST_ASSERT_TRUE(relay_route_path(RELAY_ROUTE_PAYLOAD, "tickr-A1B2C3", p, sizeof(p)) > 0);
    TEST_ASSERT_EQUAL_STRING("/api/relay/tickr-A1B2C3", p);
    TEST_ASSERT_TRUE(relay_route_path(RELAY_ROUTE_SCREEN, "tickr-A1B2C3", p, sizeof(p)) > 0);
    TEST_ASSERT_EQUAL_STRING("/api/peers/tickr-A1B2C3/screen", p);
    TEST_ASSERT_TRUE(relay_route_path(RELAY_ROUTE_LIST, "", p, sizeof(p)) > 0);
    TEST_ASSERT_EQUAL_STRING("/api/relay", p);
    TEST_ASSERT_EQUAL_UINT(0, relay_route_path(RELAY_ROUTE_NONE, "tickr-A1B2C3", p, sizeof(p)));
    TEST_ASSERT_TRUE(relay_file_name(RELAY_ROUTE_PAYLOAD, "tickr-A1B2C3", f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("/relay/tickr-A1B2C3.json", f);
    TEST_ASSERT_TRUE(relay_file_name(RELAY_ROUTE_OTA, "tickr-A1B2C3", f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("/relay/tickr-A1B2C3.ota.json", f);
    TEST_ASSERT_TRUE(relay_file_name(RELAY_ROUTE_SCREEN, "tickr-A1B2C3", f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("/peers/tickr-A1B2C3.raw", f);
    TEST_ASSERT_FALSE(relay_file_name(RELAY_ROUTE_LIST, "", f, sizeof(f)));
    TEST_ASSERT_FALSE(relay_file_name(RELAY_ROUTE_NONE, "", f, sizeof(f)));
    // every route's path classifies back to itself
    for (int r = RELAY_ROUTE_PAYLOAD; r <= RELAY_ROUTE_SCREEN; r++) {
        relay_route_path((RelayRoute)r, "tickr-ABCDEF", p, sizeof(p));
        TEST_ASSERT_EQUAL(r, relay_route(p, id));
        TEST_ASSERT_EQUAL_STRING("tickr-ABCDEF", id);
        relay_file_name((RelayRoute)r, "tickr-ABCDEF", f, sizeof(f));
        TEST_ASSERT_EQUAL(r, relay_file_route(f, id));
        TEST_ASSERT_EQUAL_STRING("tickr-ABCDEF", id);
    }
    TEST_ASSERT_EQUAL(RELAY_ROUTE_PAYLOAD, relay_file_route("tickr-ABCDEF.json", id));     // bare name (LittleFS dir listing)
    TEST_ASSERT_EQUAL(RELAY_ROUTE_NONE, relay_file_route("tickr-ABCDEF.json.tmp", id));
    TEST_ASSERT_EQUAL(RELAY_ROUTE_NONE, relay_file_route("layout.json", id));
    TEST_ASSERT_EQUAL(RELAY_ROUTE_NONE, relay_file_route(nullptr, id));
}

static void test_url_checks() {
    TEST_ASSERT_TRUE(relay_url_ok("http://10.0.0.5/fw/tickrdisplay-0.4.0.bin"));
    TEST_ASSERT_TRUE(relay_url_ok("https://example.org/a.bin"));
    TEST_ASSERT_TRUE(relay_url_ok("HTTP://x.y/z"));
    TEST_ASSERT_FALSE(relay_url_ok("ftp://x/y.bin"));
    TEST_ASSERT_FALSE(relay_url_ok("http://"));
    TEST_ASSERT_FALSE(relay_url_ok("http://a b/c"));
    TEST_ASSERT_FALSE(relay_url_ok("http://a/\"c"));
    TEST_ASSERT_FALSE(relay_url_ok(nullptr));
    char lng[RELAY_URL_MAX + 8];
    memset(lng, 'a', sizeof(lng) - 1);
    memcpy(lng, "http://", 7);
    lng[sizeof(lng) - 1] = '\0';
    TEST_ASSERT_FALSE(relay_url_ok(lng));
    lng[RELAY_URL_MAX - 1] = '\0';
    TEST_ASSERT_TRUE(relay_url_ok(lng));                       // 127 chars: the maximum
    char out[RELAY_URL_MAX];
    TEST_ASSERT_TRUE(relay_url_from_json("{\"url\":\"http://10.0.0.5/fw.bin\",\"x\":1}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.5/fw.bin", out);
    TEST_ASSERT_TRUE(relay_url_from_json("{ \"url\" : \"http://h/f\" }", out, sizeof(out)));
    TEST_ASSERT_FALSE(relay_url_from_json("{\"url\":\"http://h/f", out, sizeof(out)));          // unterminated
    TEST_ASSERT_FALSE(relay_url_from_json("{\"uri\":\"http://h/f\"}", out, sizeof(out)));
    TEST_ASSERT_FALSE(relay_url_from_json("{\"url\":\"gopher://h/f\"}", out, sizeof(out)));
    TEST_ASSERT_FALSE(relay_url_from_json("", out, sizeof(out)));
    TEST_ASSERT_FALSE(relay_url_from_json(nullptr, out, sizeof(out)));
    char tiny[8];
    TEST_ASSERT_FALSE(relay_url_from_json("{\"url\":\"http://h/f\"}", tiny, sizeof(tiny)));
}

// ---------------------------------------------------------------------------
// The sleeper's decisions
// ---------------------------------------------------------------------------
static void test_pick_source_precedence() {
    TEST_ASSERT_EQUAL(RELAY_SRC_PULL_URL, relay_pick_source(true, true));
    TEST_ASSERT_EQUAL(RELAY_SRC_PULL_URL, relay_pick_source(true, false));
    TEST_ASSERT_EQUAL(RELAY_SRC_RELAY, relay_pick_source(false, true));
    TEST_ASSERT_EQUAL(RELAY_SRC_NONE, relay_pick_source(false, false));
    TEST_ASSERT_FALSE(relay_report_frame(1));
    TEST_ASSERT_FALSE(relay_report_frame(9));
    TEST_ASSERT_TRUE(relay_report_frame(10));
    TEST_ASSERT_TRUE(relay_report_frame(1440));
}

static BeaconIn reply_fixture() {
    BeaconIn b;
    memset(&b, 0, sizeof(b));
    b.kind = BEACON_ANNOUNCE;
    const uint8_t mac[6] = {0x02, 0, 0, 0xA1, 0xB2, 0xC3};
    memcpy(b.mac, mac, 6);
    b.usb = true;
    b.ip = 0x0A00000B;
    strcpy(b.relay_nonce, "0123456789abcdef");
    return b;
}

static void test_reply_usable_rules() {
    const uint8_t self[6] = {0x02, 0, 0, 0xD4, 0xE5, 0xF6};
    BeaconIn b = reply_fixture();
    TEST_ASSERT_TRUE(relay_reply_usable(b, true, self));
    TEST_ASSERT_FALSE_MESSAGE(relay_reply_usable(b, false, self), "tag did not verify");
    b.usb = false;
    TEST_ASSERT_FALSE_MESSAGE(relay_reply_usable(b, true, self), "a battery device is no relay");
    b = reply_fixture();
    b.relay_nonce[0] = '\0';
    TEST_ASSERT_FALSE_MESSAGE(relay_reply_usable(b, true, self), "no relay nonce = no relay role");
    b = reply_fixture();
    b.kind = BEACON_PROBE;
    TEST_ASSERT_FALSE(relay_reply_usable(b, true, self));
    b = reply_fixture();
    memcpy(b.mac, self, 6);
    TEST_ASSERT_FALSE_MESSAGE(relay_reply_usable(b, true, self), "never through ourselves (loop)");
    b = reply_fixture();
    b.ip = 0;
    TEST_ASSERT_FALSE(relay_reply_usable(b, true, self));
}

static void test_flow_happy_path_with_hint() {
    RelayFlow f;
    relay_flow_init(&f, true, true);
    TEST_ASSERT_EQUAL(RELAY_STEP_PROBE_HINT, f.step);
    relay_flow_next(&f, true, false, false);                 // hint answered
    TEST_ASSERT_EQUAL(RELAY_STEP_FETCH, f.step);
    relay_flow_next(&f, true, true, false);                  // payload, no OTA
    TEST_ASSERT_EQUAL(RELAY_STEP_APPLY, f.step);
    relay_flow_next(&f, true, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_UPLOAD, f.step);
    relay_flow_next(&f, false, false, false);                // upload failed: best effort
    TEST_ASSERT_EQUAL(RELAY_STEP_DONE, f.step);
    TEST_ASSERT_EQUAL_UINT8(1, f.probes);
}

static void test_flow_hint_miss_then_broadcast_then_nothing() {
    RelayFlow f;
    relay_flow_init(&f, true, false);
    relay_flow_next(&f, false, false, false);                // cached relay gone
    TEST_ASSERT_EQUAL(RELAY_STEP_PROBE_BCAST, f.step);
    relay_flow_next(&f, true, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_FETCH, f.step);
    relay_flow_next(&f, true, false, false);                 // 204: nothing pending
    TEST_ASSERT_EQUAL(RELAY_STEP_DONE, f.step);
    TEST_ASSERT_EQUAL_UINT8(2, f.probes);
    // no relay at all: two probes at most, then a terminal state that stays
    relay_flow_init(&f, true, true);
    relay_flow_next(&f, false, false, false);
    relay_flow_next(&f, false, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_NO_RELAY, f.step);
    relay_flow_next(&f, true, true, true);
    TEST_ASSERT_EQUAL(RELAY_STEP_NO_RELAY, f.step);
    TEST_ASSERT_EQUAL_UINT8(2, f.probes);
    relay_flow_init(&f, false, true);
    TEST_ASSERT_EQUAL(RELAY_STEP_PROBE_BCAST, f.step);
    relay_flow_next(&f, false, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_NO_RELAY, f.step);
    TEST_ASSERT_EQUAL_UINT8(1, f.probes);
}

static void test_flow_ota_ordering_and_failures() {
    RelayFlow f;
    // payload + OTA: apply first (the panel sees the content even if the flash fails), OTA next,
    // upload only when the OTA did not succeed (a success restarts the device).
    relay_flow_init(&f, false, true);
    relay_flow_next(&f, true, false, false);
    relay_flow_next(&f, true, true, true);
    TEST_ASSERT_EQUAL(RELAY_STEP_APPLY, f.step);
    relay_flow_next(&f, true, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_OTA, f.step);
    relay_flow_next(&f, false, false, false);                // flash failed
    TEST_ASSERT_EQUAL(RELAY_STEP_UPLOAD, f.step);
    relay_flow_next(&f, true, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_DONE, f.step);
    // OTA only, success -> DONE (the caller restarts)
    relay_flow_init(&f, false, true);
    relay_flow_next(&f, true, false, false);
    relay_flow_next(&f, true, false, true);
    TEST_ASSERT_EQUAL(RELAY_STEP_OTA, f.step);
    relay_flow_next(&f, true, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_DONE, f.step);
    // a rejected payload is not uploaded, and the interval rule skips the upload
    relay_flow_init(&f, false, true);
    relay_flow_next(&f, true, false, false);
    relay_flow_next(&f, true, true, false);
    relay_flow_next(&f, false, false, false);                // payload rejected
    TEST_ASSERT_EQUAL(RELAY_STEP_DONE, f.step);
    relay_flow_init(&f, false, false);                       // short interval: no report
    relay_flow_next(&f, true, false, false);
    relay_flow_next(&f, true, true, false);
    relay_flow_next(&f, true, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_DONE, f.step);
    // the relay answered but the fetch failed: terminal FAILED
    relay_flow_init(&f, false, true);
    relay_flow_next(&f, true, false, false);
    relay_flow_next(&f, false, false, false);
    TEST_ASSERT_EQUAL(RELAY_STEP_FAILED, f.step);
}

// ---------------------------------------------------------------------------
// Beacon "rn" field and layout "groups"
// ---------------------------------------------------------------------------
static BeaconSelf self_fixture() {
    BeaconSelf s;
    memset(&s, 0, sizeof(s));
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xD4, 0xE5, 0xF6};
    memcpy(s.mac, mac, 6);
    strcpy(s.name, "Kitchen left");
    strcpy(s.ip, "192.168.1.41");
    strcpy(s.version, "0.3.1");
    s.usb = true;
    strcpy(s.group_id, GID);
    s.epoch = 3;
    return s;
}

static void test_beacon_relay_nonce_roundtrip_and_size() {
    BeaconSelf s = self_fixture();
    strcpy(s.name, "123456789012345");
    strcpy(s.ip, "192.168.100.200");
    strcpy(s.version, "12.34.56-99-gab");
    s.epoch = 65535;
    char buf[256], msg1[BEACON_SIGN_MAX], msg2[BEACON_SIGN_MAX];
    size_t n = beacon_build(s, 4294967295UL, "0123456789abcdef", "fedcba9876543210", buf, sizeof(buf), "89abcdef01234567");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE_MESSAGE(n <= BEACON_MAX_LEN, buf);      // worst case incl. nc + rn still fits the datagram cap
    BeaconIn b;
    TEST_ASSERT_TRUE(beacon_parse(buf, n, &b));
    TEST_ASSERT_EQUAL_STRING("89abcdef01234567", b.relay_nonce);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", b.nonce);
    // rn is not part of the signed message: the same tag verifies with and without it
    size_t m1 = beacon_sign_message(s, 4294967295UL, msg1, sizeof(msg1));
    size_t m2 = beacon_sign_message_in(b, msg2, sizeof(msg2));
    TEST_ASSERT_EQUAL_UINT(m1, m2);
    TEST_ASSERT_EQUAL_STRING(msg1, msg2);
    char tag[BEACON_HEX16_LEN];
    beacon_tag(SECRET, msg1, m1, tag);
    strcpy(b.tag, tag);
    TEST_ASSERT_TRUE(beacon_verify(b, SECRET));
    // without rn the field is empty
    n = beacon_build(s, 1, nullptr, nullptr, buf, sizeof(buf));
    TEST_ASSERT_TRUE(beacon_parse(buf, n, &b));
    TEST_ASSERT_EQUAL_STRING("", b.relay_nonce);
    n = beacon_build(s, 1, nullptr, nullptr, buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(beacon_parse(buf, n, &b));
    TEST_ASSERT_EQUAL_STRING("", b.relay_nonce);
}

static void test_beacon_relay_nonce_rejects_bad_values() {
    const char* bad[] = { "0123456789abcde", "0123456789abcdef0", "0123456789abcdeg", "" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "{\"t\":\"tickr\",\"mac\":\"020000d4e5f6\",\"rn\":\"%s\"}", bad[i]);
        BeaconIn b;
        TEST_ASSERT_TRUE_MESSAGE(beacon_parse(buf, (size_t)n, &b), buf);   // the datagram is fine, the field is dropped
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", b.relay_nonce, buf);
    }
    // a non-string rn is skipped like any other wrong-typed field
    const char num[] = "{\"t\":\"tickr\",\"mac\":\"020000d4e5f6\",\"rn\":12345}";
    BeaconIn b;
    TEST_ASSERT_TRUE(beacon_parse(num, strlen(num), &b));
    TEST_ASSERT_EQUAL_STRING("", b.relay_nonce);
    // a \u escape is a bad escape for the whole datagram (fails closed, as for every other field)
    const char esc[] = "{\"t\":\"tickr\",\"mac\":\"020000d4e5f6\",\"rn\":\"\\u0041123456789abcde\"}";
    TEST_ASSERT_FALSE(beacon_parse(esc, strlen(esc), &b));
    // duplicate: first wins
    const char dup[] = "{\"t\":\"tickr\",\"mac\":\"020000d4e5f6\",\"rn\":\"0123456789abcdef\",\"rn\":\"fedcba9876543210\"}";
    TEST_ASSERT_TRUE(beacon_parse(dup, strlen(dup), &b));
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", b.relay_nonce);
}

static const char* lwhy(const char* json) {
    static char err[48];
    err[0] = '\0';
    layout_validate(json, strlen(json), nullptr, err, sizeof(err));
    return err;
}

static void test_layout_groups_key() {
    size_t n = 0;
    char err[48] = "";
    const char ok[] = "{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":0,\"y\":0}},"
                      "\"groups\":{\"Kitchen\":[\"tickr-A1B2C3\",\"tickr-D4E5F6\"],\"Empty\":[]}}";
    TEST_ASSERT_TRUE_MESSAGE(layout_validate(ok, strlen(ok), &n, err, sizeof(err)), err);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("groups must be an object", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":[]}"));
    TEST_ASSERT_EQUAL_STRING("group must be an array of ids", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{\"a\":\"tickr-A1B2C3\"}}"));
    TEST_ASSERT_EQUAL_STRING("group member must be tickr-XXXXXX", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{\"a\":[\"kitchen\"]}}"));
    TEST_ASSERT_EQUAL_STRING("group member must be tickr-XXXXXX", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{\"a\":[5]}}"));
    TEST_ASSERT_EQUAL_STRING("group name must be 1..31 chars", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{\"\":[]}}"));
    TEST_ASSERT_EQUAL_STRING("group name must be 1..31 chars", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{\"0123456789012345678901234567890X\":[]}}"));
    TEST_ASSERT_EQUAL_STRING("group name must be printable ASCII", lwhy("{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{\"K\\u00fcche\":[]}}"));
    char many[512] = "{\"v\":1,\"updated_at\":0,\"devices\":{},\"groups\":{";
    for (int i = 0; i <= LAYOUT_MAX_GROUPS; i++) {
        char g[24];
        snprintf(g, sizeof(g), "%s\"g%d\":[]", i ? "," : "", i);
        strcat(many, g);
    }
    strcat(many, "}}");
    TEST_ASSERT_EQUAL_STRING("too many groups", lwhy(many));
}

int main() {
    memset(SECRET, 0x42, sizeof(SECRET));
    sharef::sha256((const uint8_t*)"", 0, EMPTY_SHA);
    g_hmac_sha256 = sharef::hmac_sha256;
    UNITY_BEGIN();
    RUN_TEST(test_nonce_issue_and_single_use);
    RUN_TEST(test_nonce_ttl_and_unknown);
    RUN_TEST(test_nonce_store_overwrites_oldest);
    RUN_TEST(test_sign_message_canonical_form);
    RUN_TEST(test_header_roundtrip_and_verify);
    RUN_TEST(test_verify_rejects_every_mismatch);
    RUN_TEST(test_header_parse_rejects_malformed);
    RUN_TEST(test_route_classification);
    RUN_TEST(test_route_paths_and_files);
    RUN_TEST(test_url_checks);
    RUN_TEST(test_pick_source_precedence);
    RUN_TEST(test_reply_usable_rules);
    RUN_TEST(test_flow_happy_path_with_hint);
    RUN_TEST(test_flow_hint_miss_then_broadcast_then_nothing);
    RUN_TEST(test_flow_ota_ordering_and_failures);
    RUN_TEST(test_beacon_relay_nonce_roundtrip_and_size);
    RUN_TEST(test_beacon_relay_nonce_rejects_bad_values);
    RUN_TEST(test_layout_groups_key);
    return UNITY_END();
}
