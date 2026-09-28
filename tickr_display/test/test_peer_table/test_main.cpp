// Host tests for the peer table, the /peers.bin blob and the beacon wire format
// (src/logic/peer_table.cpp, src/logic/beacon.cpp).
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "logic/peer_table.h"
#include "logic/beacon.h"

void setUp() {}
void tearDown() {}

static PeerTable T;

static Peer mk(uint8_t last, uint32_t seen, uint8_t flags = PEER_F_USB) {
    Peer p;
    memset(&p, 0, sizeof(p));
    p.mac[0] = 0x08; p.mac[5] = last;
    p.ip = 0x0A00000A + ((uint32_t)last << 24);
    p.last_seen_s = seen;
    p.flags = flags;
    snprintf(p.name, sizeof(p.name), "peer-%u", last);
    return p;
}

static void test_struct_layout() {
    TEST_ASSERT_EQUAL_UINT(40, sizeof(Peer));
    TEST_ASSERT_EQUAL_UINT(8, PEERS_BLOB_HEADER);
}

static void test_upsert_and_update() {
    peer_table_init(&T);
    Peer* p = peer_table_upsert(&T, mk(1, 10));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(1, T.count);
    TEST_ASSERT_TRUE(T.dirty);
    T.dirty = false;

    // Same peer again: only last_seen/rssi changed -> not dirty
    Peer u = mk(1, 20);
    u.rssi = -50;
    p = peer_table_upsert(&T, u);
    TEST_ASSERT_EQUAL_UINT8(1, T.count);
    TEST_ASSERT_EQUAL_UINT32(20, p->last_seen_s);
    TEST_ASSERT_EQUAL_INT8(-50, p->rssi);
    TEST_ASSERT_FALSE(T.dirty);

    // Name change -> dirty
    strcpy(u.name, "renamed");
    peer_table_upsert(&T, u);
    TEST_ASSERT_TRUE(T.dirty);
    TEST_ASSERT_EQUAL_STRING("renamed", T.peers[0].name);
}

static void test_eviction_oldest_non_member() {
    peer_table_init(&T);
    for (uint8_t i = 0; i < TICKR_MAX_PEERS; i++) {
        TEST_ASSERT_NOT_NULL(peer_table_upsert(&T, mk(i, 100 + i)));
    }
    TEST_ASSERT_EQUAL_UINT8(TICKR_MAX_PEERS, T.count);
    // Table full: the newcomer evicts the oldest (mac 0, seen 100)
    Peer* n = peer_table_upsert(&T, mk(200, 500));
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_UINT8(TICKR_MAX_PEERS, T.count);
    uint8_t mac0[6] = {0x08, 0, 0, 0, 0, 0};
    TEST_ASSERT_NULL(peer_table_find(&T, mac0));
    uint8_t mac200[6] = {0x08, 0, 0, 0, 0, 200};
    TEST_ASSERT_NOT_NULL(peer_table_find(&T, mac200));
}

static void test_members_never_evicted_for_non_members() {
    peer_table_init(&T);
    for (uint8_t i = 0; i < TICKR_MAX_PEERS; i++) {
        peer_table_upsert(&T, mk(i, 100 + i, PEER_F_USB | PEER_F_MEMBER));
    }
    TEST_ASSERT_NULL(peer_table_upsert(&T, mk(200, 500)));            // non-member refused
    // A member may replace a STALE member
    T.peers[3].flags |= PEER_F_STALE;
    Peer* m = peer_table_upsert(&T, mk(201, 600, PEER_F_USB | PEER_F_MEMBER));
    TEST_ASSERT_NOT_NULL(m);
    uint8_t mac3[6] = {0x08, 0, 0, 0, 0, 3};
    TEST_ASSERT_NULL(peer_table_find(&T, mac3));
}

static void test_remove_and_find_by_id() {
    peer_table_init(&T);
    peer_table_upsert(&T, mk(0xA1, 1));
    peer_table_upsert(&T, mk(0xA2, 2));
    peer_table_upsert(&T, mk(0xA3, 3));
    Peer* p = peer_table_find_by_id(&T, "tickr-0000A2");
    TEST_ASSERT_NOT_NULL(p);
    size_t idx = (size_t)(p - T.peers);
    TEST_ASSERT_TRUE(peer_table_remove(&T, idx));
    TEST_ASSERT_EQUAL_UINT8(2, T.count);
    TEST_ASSERT_NULL(peer_table_find_by_id(&T, "tickr-0000A2"));
    TEST_ASSERT_NOT_NULL(peer_table_find_by_id(&T, "tickr-0000A3"));
    TEST_ASSERT_FALSE(peer_table_remove(&T, 5));
}

static void test_stale_marking_respects_sleep_plan() {
    peer_table_init(&T);
    peer_table_upsert(&T, mk(1, 100));                     // USB
    Peer s = mk(2, 100, 0);                                 // sleeper
    s.next_wake_s = 100 + 3600;
    peer_table_upsert(&T, s);
    TEST_ASSERT_EQUAL_UINT(2, peer_table_fresh_count(&T));
    // TTL = 4 T = 180 s at T = 45 s (peer_manager STALE_PERIODS); 3.6 T = 162 s
    // (three jittered intervals, one beacon lost) must NOT mark a USB peer stale.
    TEST_ASSERT_EQUAL_UINT(0, peer_table_mark_stale(&T, 100 + 162, 180));   // one lost beacon: still fresh
    TEST_ASSERT_EQUAL_UINT(1, peer_table_mark_stale(&T, 100 + 181, 180));   // USB one goes stale
    TEST_ASSERT_EQUAL_UINT(1, peer_table_fresh_count(&T));
    TEST_ASSERT_EQUAL_UINT(0, peer_table_mark_stale(&T, 100 + 3600, 180));  // sleeper still within plan
    TEST_ASSERT_EQUAL_UINT(0, peer_table_mark_stale(&T, 100 + 3600 + 180, 180));
    TEST_ASSERT_EQUAL_UINT(1, peer_table_mark_stale(&T, 100 + 3600 + 181, 180));
    TEST_ASSERT_EQUAL_UINT(0, peer_table_fresh_count(&T));
}

static void test_blob_roundtrip_and_corruption() {
    peer_table_init(&T);
    peer_table_upsert(&T, mk(1, 100));
    peer_table_upsert(&T, mk(2, 200));
    uint8_t buf[PEERS_BLOB_MAX];
    size_t n = peer_table_save_blob(&T, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(8 + 2 * 40, n);
    TEST_ASSERT_EQUAL_MEMORY("TKPR", buf, 4);

    PeerTable L;
    TEST_ASSERT_TRUE(peer_table_load_blob(&L, buf, n));
    TEST_ASSERT_EQUAL_UINT8(2, L.count);
    TEST_ASSERT_TRUE(L.peers[0].flags & PEER_F_STALE);
    TEST_ASSERT_EQUAL_UINT32(0, L.peers[0].last_seen_s);
    TEST_ASSERT_EQUAL_STRING("peer-2", L.peers[1].name);
    TEST_ASSERT_EQUAL_UINT32(T.peers[1].ip, L.peers[1].ip);

    buf[20] ^= 0x55;                                          // flip a byte -> CRC fails
    TEST_ASSERT_FALSE(peer_table_load_blob(&L, buf, n));
    TEST_ASSERT_EQUAL_UINT8(0, L.count);
    buf[20] ^= 0x55;
    TEST_ASSERT_FALSE(peer_table_load_blob(&L, buf, n - 1));  // truncated
    buf[0] = 'X';
    TEST_ASSERT_FALSE(peer_table_load_blob(&L, buf, n));      // bad magic
    TEST_ASSERT_FALSE(peer_table_load_blob(&L, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT(0, peer_table_save_blob(&T, buf, 10));   // too small
}

static void test_helpers() {
    uint8_t mac[6] = {0x02, 0x00, 0x00, 0xD4, 0xE5, 0xF6};
    char id[PEER_ID_LEN];
    peer_id_from_mac(mac, id);
    TEST_ASSERT_EQUAL_STRING("tickr-D4E5F6", id);

    uint8_t m2[6];
    TEST_ASSERT_TRUE(peer_mac_from_str("020000d4e5f6", m2));
    TEST_ASSERT_EQUAL_MEMORY(mac, m2, 6);
    TEST_ASSERT_TRUE(peer_mac_from_str("02:00:00:D4:E5:F6", m2));
    TEST_ASSERT_EQUAL_MEMORY(mac, m2, 6);
    TEST_ASSERT_FALSE(peer_mac_from_str("020000d4e5", m2));
    TEST_ASSERT_FALSE(peer_mac_from_str("020000d4e5f6ff", m2));
    TEST_ASSERT_FALSE(peer_mac_from_str("zz:00:00:d4:e5:f6", m2));
    TEST_ASSERT_FALSE(peer_mac_from_str("", m2));

    TEST_ASSERT_EQUAL_HEX16((1 << 12) | (2 << 6) | 3, peer_pack_version("v1.2.3"));
    TEST_ASSERT_EQUAL_HEX16((0 << 12) | (3 << 6) | 1, peer_pack_version("0.3.1-4-gabc-dirty"));
    TEST_ASSERT_EQUAL_HEX16(0, peer_pack_version("0.0.0-ge32df60"));
    TEST_ASSERT_EQUAL_HEX16(0, peer_pack_version("garbage"));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, peer_pack_version("99.99.99"));   // clamped
    char v[12];
    peer_unpack_version(peer_pack_version("v1.2.3"), v, sizeof(v));
    TEST_ASSERT_EQUAL_STRING("1.2.3", v);

    char name[PEER_NAME_LEN];
    peer_sanitize_name("Kitchen \"left\"\\ \x01 shelf very long name", name);
    TEST_ASSERT_EQUAL_STRING("Kitchen _left__", name);
    TEST_ASSERT_EQUAL_UINT(15, strlen(name));
    peer_sanitize_name(nullptr, name);
    TEST_ASSERT_EQUAL_STRING("", name);
}

// ---------------------------------------------------------------------------
// Beacon wire format
// ---------------------------------------------------------------------------
static BeaconSelf self_fixture() {
    BeaconSelf s;
    memset(&s, 0, sizeof(s));
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xD4, 0xE5, 0xF6};
    memcpy(s.mac, mac, 6);
    strcpy(s.name, "Kitchen left");
    strcpy(s.ip, "192.168.1.41");
    strcpy(s.version, "0.3.1-4-gabcdef");
    s.usb = true;
    s.rssi = -70;
    return s;
}

static void test_beacon_build_parse_roundtrip() {
    BeaconSelf s = self_fixture();
    char buf[256];
    size_t n = beacon_build(s, 42, nullptr, nullptr, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE_MESSAGE(n <= 200, buf);
    TEST_ASSERT_EQUAL_UINT(n, strlen(buf));

    BeaconIn b;
    TEST_ASSERT_TRUE(beacon_parse(buf, n, &b));
    TEST_ASSERT_EQUAL(BEACON_ANNOUNCE, b.kind);
    TEST_ASSERT_EQUAL_MEMORY(s.mac, b.mac, 6);
    TEST_ASSERT_EQUAL_STRING("Kitchen left", b.name);
    TEST_ASSERT_TRUE(b.usb);
    TEST_ASSERT_EQUAL_UINT32(42, b.seq);
    TEST_ASSERT_EQUAL_INT8(-70, b.rssi);
    TEST_ASSERT_EQUAL_HEX16(peer_pack_version("0.3.1"), b.version);
    char ip[16];
    beacon_ip_format(b.ip, ip, sizeof(ip));
    TEST_ASSERT_EQUAL_STRING("192.168.1.41", ip);
    TEST_ASSERT_EQUAL_STRING("", b.group_id);
    TEST_ASSERT_EQUAL_STRING("", b.tag);
    TEST_ASSERT_EQUAL_STRING("", b.nonce);

    Peer p;
    beacon_to_peer(b, 1000, &p);
    TEST_ASSERT_EQUAL_UINT32(1000, p.last_seen_s);
    TEST_ASSERT_EQUAL_UINT32(0, p.next_wake_s);
    TEST_ASSERT_TRUE(p.flags & PEER_F_USB);
    TEST_ASSERT_FALSE(p.flags & PEER_F_MEMBER);
}

static void test_beacon_battery_and_reserved_fields_fit() {
    BeaconSelf s = self_fixture();
    strcpy(s.name, "123456789012345");          // 15 chars, the maximum
    strcpy(s.ip, "192.168.100.200");
    strcpy(s.version, "12.34.56-99-gab");       // 15 chars
    s.usb = false;
    s.sleep_s = 86400;
    s.rssi = -100;
    strcpy(s.group_id, "0123456789abcdef");     // group field, worst case
    s.epoch = 65535;
    char buf[256];
    size_t n = beacon_build(s, 4294967295UL, "0123456789abcdef", "fedcba9876543210", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    // Worst case incl. the 16-hex tag and a nonce: still below BEACON_MAX_LEN.
    TEST_ASSERT_TRUE_MESSAGE(n <= BEACON_MAX_LEN, buf);
    BeaconIn b;
    TEST_ASSERT_TRUE(beacon_parse(buf, n, &b));
    TEST_ASSERT_FALSE(b.usb);
    TEST_ASSERT_EQUAL_UINT32(86400, b.sleep_s);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", b.group_id);
    TEST_ASSERT_EQUAL_STRING("fedcba9876543210", b.tag);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", b.nonce);
    TEST_ASSERT_EQUAL_HEX16(65535, b.epoch);
    Peer p;
    beacon_to_peer(b, 500, &p);
    TEST_ASSERT_EQUAL_UINT32(500 + 86400, p.next_wake_s);
    TEST_ASSERT_EQUAL_UINT8(0xFF, p.epoch);
    TEST_ASSERT_FALSE(p.flags & PEER_F_USB);
    // Too small an output buffer is refused, not truncated
    TEST_ASSERT_EQUAL_UINT(0, beacon_build(s, 1, nullptr, nullptr, buf, 64));
}

static void test_probe_roundtrip() {
    uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    char buf[128];
    size_t n = beacon_build_probe(mac, "abc123", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    BeaconIn b;
    TEST_ASSERT_TRUE(beacon_parse(buf, n, &b));
    TEST_ASSERT_EQUAL(BEACON_PROBE, b.kind);
    TEST_ASSERT_EQUAL_MEMORY(mac, b.mac, 6);
    TEST_ASSERT_EQUAL_STRING("abc123", b.nonce);
}

static void test_malformed_datagrams_rejected() {
    BeaconIn b;
    TEST_ASSERT_FALSE(beacon_parse(nullptr, 0, &b));
    TEST_ASSERT_FALSE(beacon_parse("", 0, &b));
    TEST_ASSERT_FALSE(beacon_parse("garbage", 7, &b));
    TEST_ASSERT_FALSE(beacon_parse("{\"t\":\"tickr\",\"mac\":", 19, &b));           // truncated JSON
    TEST_ASSERT_FALSE(beacon_parse("{\"t\":\"other\",\"mac\":\"010203040506\"}", 35, &b));   // wrong magic
    TEST_ASSERT_FALSE(beacon_parse("{\"t\":\"tickr\"}", 13, &b));                    // no mac
    TEST_ASSERT_FALSE(beacon_parse("{\"t\":\"tickr\",\"mac\":\"nope\"}", 27, &b));    // bad mac
    TEST_ASSERT_FALSE(beacon_parse("[1,2,3]", 7, &b));                              // not an object
    TEST_ASSERT_FALSE(beacon_parse("{\"t\":5,\"mac\":\"010203040506\"}", 29, &b));   // t not a string
    TEST_ASSERT_EQUAL(BEACON_INVALID, b.kind);

    // Too long: exactly BEACON_MAX_LEN+1 bytes of otherwise valid JSON
    char big[BEACON_MAX_LEN + 2];
    memset(big, ' ', sizeof(big));
    const char head[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"";
    memcpy(big, head, sizeof(head) - 1);
    big[BEACON_MAX_LEN - 1] = '"';
    big[BEACON_MAX_LEN] = '}';
    big[BEACON_MAX_LEN + 1] = '\0';
    TEST_ASSERT_FALSE(beacon_parse(big, BEACON_MAX_LEN + 1, &b));
    // ... but the same shape at BEACON_MAX_LEN parses and the name is truncated safely
    big[BEACON_MAX_LEN - 2] = '"';
    big[BEACON_MAX_LEN - 1] = '}';
    TEST_ASSERT_TRUE(beacon_parse(big, BEACON_MAX_LEN, &b));
    TEST_ASSERT_TRUE(strlen(b.name) <= PEER_NAME_LEN - 1);

    // Hostile fields are neutralised: quotes in the name, non-hex group id, huge numbers
    const char hostile[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"a\\\"b\\\\c\",\"ip\":\"999.1.1.1\","
                           "\"g\":\"not-hex\",\"tag\":\"zz\",\"rs\":-999,\"ep\":99999999,\"nc\":\"x\\\"y\"}";
    TEST_ASSERT_TRUE(beacon_parse(hostile, sizeof(hostile) - 1, &b));
    TEST_ASSERT_EQUAL_STRING("a_b_c", b.name);
    TEST_ASSERT_EQUAL_UINT32(0, b.ip);
    TEST_ASSERT_EQUAL_STRING("", b.group_id);
    TEST_ASSERT_EQUAL_STRING("", b.tag);
    TEST_ASSERT_EQUAL_INT8(-127, b.rssi);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, b.epoch);
    TEST_ASSERT_EQUAL_STRING("x_y", b.nonce);
}

// Parses a copy of exactly `len` bytes on the heap without a terminator, so
// the sanitizer (native build: -fsanitize=address) flags any read past the
// datagram.
static bool parse_exact(const char* s, size_t len, BeaconIn* b) {
    char* buf = (char*)malloc(len ? len : 1);
    memcpy(buf, s, len);
    bool ok = beacon_parse(buf, len, b);
    free(buf);
    return ok;
}

static void test_parser_hostile_input() {
    BeaconIn b;
    BeaconSelf s = self_fixture();
    char full[256];
    size_t n = beacon_build(s, 7, "nonce1", nullptr, full, sizeof(full));
    TEST_ASSERT_TRUE(n > 0);

    // Every proper prefix is rejected, the whole datagram is accepted.
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse_exact(full, i, &b), "prefix parsed");
    }
    TEST_ASSERT_TRUE(parse_exact(full, n, &b));

    // Single-byte mutations at every position must never crash; whatever
    // parses must still carry a valid MAC (peer_mac_from_str accepted it).
    static const char muts[] = "\"{}[],:\\ \x01" "\xff" "0-";
    for (size_t i = 0; i < n; i++) {
        for (size_t m = 0; m < sizeof(muts) - 1; m++) {
            char mutated[256];
            memcpy(mutated, full, n);
            mutated[i] = muts[m];
            if (parse_exact(mutated, n, &b)) {
                TEST_ASSERT_TRUE(b.kind == BEACON_ANNOUNCE || b.kind == BEACON_PROBE);
            }
        }
    }

    // Trailing garbage after the object is rejected, trailing whitespace is fine.
    char trail[300];
    memcpy(trail, full, n);
    memcpy(trail + n, "x", 1);
    TEST_ASSERT_FALSE(parse_exact(trail, n + 1, &b));
    memcpy(trail + n, " \n", 2);
    TEST_ASSERT_TRUE(parse_exact(trail, n + 2, &b));
    memcpy(trail + n, "{\"t\":\"tickr\"}", 13);
    TEST_ASSERT_FALSE(parse_exact(trail, n + 13, &b));

    // Leading whitespace, unknown keys with nested values (skipped), literals.
    const char nested[] = " \t{\"x\":{\"a\":[1,2,{\"b\":null,\"c\":[]}],\"d\":\"q\\\"\"},\"t\":\"tickr\","
                          "\"y\":true,\"z\":-1.5e3,\"mac\":\"010203040506\",\"seq\":9}";
    TEST_ASSERT_TRUE(parse_exact(nested, sizeof(nested) - 1, &b));
    TEST_ASSERT_EQUAL_UINT32(9, b.seq);
    TEST_ASSERT_EQUAL_UINT8(6, b.mac[5]);
    // ... but not deeper than the cap (5 levels here).
    const char deep[] = "{\"x\":[[[[[1]]]]],\"t\":\"tickr\",\"mac\":\"010203040506\"}";
    TEST_ASSERT_FALSE(parse_exact(deep, sizeof(deep) - 1, &b));
    const char deep4[] = "{\"x\":[[[[1]]]],\"t\":\"tickr\",\"mac\":\"010203040506\"}";
    TEST_ASSERT_TRUE(parse_exact(deep4, sizeof(deep4) - 1, &b));

    // Duplicate keys: the first occurrence wins (a signature covers the first).
    const char dup[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"seq\":1,\"n\":\"first\",\"seq\":2,\"n\":\"second\","
                       "\"mac\":\"ffffffffffff\",\"t\":\"tickr?\"}";
    TEST_ASSERT_TRUE(parse_exact(dup, sizeof(dup) - 1, &b));
    TEST_ASSERT_EQUAL(BEACON_ANNOUNCE, b.kind);
    TEST_ASSERT_EQUAL_UINT32(1, b.seq);
    TEST_ASSERT_EQUAL_STRING("first", b.name);
    TEST_ASSERT_EQUAL_UINT8(1, b.mac[0]);

    // Over-long strings: the name is truncated to 15, a 17-char hex id is
    // dropped (not truncated into a different valid id), a long "pw" is not
    // mistaken for "bat", a long "t" is not "tickr".
    char big[256];
    int bl = snprintf(big, sizeof(big), "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"%0140d\",\"g\":\"0123456789abcdef0\","
                      "\"tag\":\"0123456789abcdeF\",\"pw\":\"battery\"}", 0);
    TEST_ASSERT_TRUE(parse_exact(big, (size_t)bl, &b));
    TEST_ASSERT_EQUAL_UINT(15, strlen(b.name));
    TEST_ASSERT_EQUAL_STRING("", b.group_id);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdeF", b.tag);
    TEST_ASSERT_TRUE(b.usb);
    const char longt[] = "{\"t\":\"tickr-extra\",\"mac\":\"010203040506\"}";
    TEST_ASSERT_FALSE(parse_exact(longt, sizeof(longt) - 1, &b));

    // Escapes: \t etc. become '_', \/ is kept; control characters, \u and
    // unknown escapes are fatal (a beacon never needs them).
    const char esc[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"a\\nb\\/c\\td\"}";
    TEST_ASSERT_TRUE(parse_exact(esc, sizeof(esc) - 1, &b));
    TEST_ASSERT_EQUAL_STRING("a_b/c_d", b.name);
    const char ctrl[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"a\x01b\"}";
    TEST_ASSERT_FALSE(parse_exact(ctrl, sizeof(ctrl) - 1, &b));
    const char badesc[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"a\\qb\"}";
    TEST_ASSERT_FALSE(parse_exact(badesc, sizeof(badesc) - 1, &b));
    const char uesc[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"a\\u00e9\"}";
    TEST_ASSERT_FALSE(parse_exact(uesc, sizeof(uesc) - 1, &b));
    const char cutesc[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":\"a\\";
    TEST_ASSERT_FALSE(parse_exact(cutesc, sizeof(cutesc) - 1, &b));

    // Wrong types: a non-string value for a string key counts as absent.
    const char types[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"n\":5,\"ip\":[1],\"sl\":\"9\",\"seq\":{},\"rs\":true}";
    TEST_ASSERT_TRUE(parse_exact(types, sizeof(types) - 1, &b));
    TEST_ASSERT_EQUAL_STRING("", b.name);
    TEST_ASSERT_EQUAL_UINT32(0, b.ip);
    TEST_ASSERT_EQUAL_UINT32(0, b.sleep_s);
    TEST_ASSERT_EQUAL_UINT32(0, b.seq);
    TEST_ASSERT_EQUAL_INT8(0, b.rssi);
    // Numbers: negatives clamp, huge values saturate, fractions keep the integer part.
    const char nums[] = "{\"t\":\"tickr\",\"mac\":\"010203040506\",\"sl\":-4,\"seq\":99999999999999999999,\"ep\":70000,\"rs\":-12.7}";
    TEST_ASSERT_TRUE(parse_exact(nums, sizeof(nums) - 1, &b));
    TEST_ASSERT_EQUAL_UINT32(0, b.sleep_s);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, b.seq);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, b.epoch);
    TEST_ASSERT_EQUAL_INT8(-12, b.rssi);

    // Structural noise.
#define LIT(s) s, sizeof(s) - 1
    TEST_ASSERT_FALSE(parse_exact(LIT("{}"), &b));
    TEST_ASSERT_FALSE(parse_exact(LIT("{"), &b));
    TEST_ASSERT_FALSE(parse_exact(LIT("{\"t\":\"tickr\",}"), &b));                       // trailing comma
    TEST_ASSERT_FALSE(parse_exact(LIT("{\"t\" \"tickr\"}"), &b));                        // missing colon
    TEST_ASSERT_FALSE(parse_exact(LIT("{t:\"tickr\"}"), &b));                            // unquoted key
    TEST_ASSERT_FALSE(parse_exact(LIT("{\"t\":\"tickr\",\"mac\":\"010203040506\""), &b)); // no closing brace
    TEST_ASSERT_FALSE(parse_exact(LIT("{\"t\":\"tickr\",\"mac\":\"010203040506\"}}"), &b));
    TEST_ASSERT_FALSE(parse_exact(LIT("{\"t\":\"tickr\",\"mac\":\"010203040506\",\"x\":}"), &b));
    TEST_ASSERT_FALSE(parse_exact(LIT("\xff\xfe{\"t\":\"tickr\"}"), &b));
#undef LIT
    // A probe with a hostile nonce: quotes are neutralised before the echo.
    const char probe[] = "{\"t\":\"tickr?\",\"mac\":\"010203040506\",\"nc\":\"ab\\\"c\\\\d e\"}";
    TEST_ASSERT_TRUE(parse_exact(probe, sizeof(probe) - 1, &b));
    TEST_ASSERT_EQUAL(BEACON_PROBE, b.kind);
    TEST_ASSERT_EQUAL_STRING("ab_c_d_e", b.nonce);
    // Mismatched brackets inside a skipped value are tolerated (bounded, string-aware);
    // an empty value is not.
    const char mism[] = "{\"x\":[1,{\"a\":\"]}\"}],\"t\":\"tickr\",\"mac\":\"010203040506\"}";
    TEST_ASSERT_TRUE(parse_exact(mism, sizeof(mism) - 1, &b));
    const char emptyv[] = "{\"x\":,\"t\":\"tickr\",\"mac\":\"010203040506\"}";
    TEST_ASSERT_FALSE(parse_exact(emptyv, sizeof(emptyv) - 1, &b));
}

static void test_ip_helpers() {
    uint32_t ip;
    TEST_ASSERT_TRUE(beacon_ip_parse("192.168.1.40", &ip));
    char s[16];
    beacon_ip_format(ip, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("192.168.1.40", s);
    TEST_ASSERT_FALSE(beacon_ip_parse("192.168.1", &ip));
    TEST_ASSERT_FALSE(beacon_ip_parse("192.168.1.256", &ip));
    TEST_ASSERT_FALSE(beacon_ip_parse("192.168.1.40.1", &ip));
    TEST_ASSERT_FALSE(beacon_ip_parse("", &ip));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_struct_layout);
    RUN_TEST(test_upsert_and_update);
    RUN_TEST(test_eviction_oldest_non_member);
    RUN_TEST(test_members_never_evicted_for_non_members);
    RUN_TEST(test_remove_and_find_by_id);
    RUN_TEST(test_stale_marking_respects_sleep_plan);
    RUN_TEST(test_blob_roundtrip_and_corruption);
    RUN_TEST(test_helpers);
    RUN_TEST(test_beacon_build_parse_roundtrip);
    RUN_TEST(test_beacon_battery_and_reserved_fields_fit);
    RUN_TEST(test_probe_roundtrip);
    RUN_TEST(test_malformed_datagrams_rejected);
    RUN_TEST(test_parser_hostile_input);
    RUN_TEST(test_ip_helpers);
    return UNITY_END();
}
