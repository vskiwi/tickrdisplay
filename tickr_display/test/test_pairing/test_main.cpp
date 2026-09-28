// Host tests for the pairing primitives (src/logic/pairing.cpp) and the
// beacon signature (src/logic/beacon.cpp). HMAC-SHA-256 comes from the
// reference implementation in sha256_ref.h (the firmware uses mbedTLS).
//
// Fixed protocol vectors were produced with an independent implementation
// (node:crypto, a script kept outside the repository) from
// the RFC 7748 §6.1 key pairs: sk_i = Alice, sk_d = Bob, K = the RFC's shared
// secret, n_d = 00..0f, code 482913. The browser implementation
// (www/src/_crypto.js) is checked against the same numbers in node.
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "sha256_ref.h"
#include "logic/pairing.h"
#include "logic/beacon.h"

void setUp() {}
void tearDown() {}

static void unhex(const char* h, uint8_t* out, size_t n) {
    TEST_ASSERT_TRUE_MESSAGE(pairing_unhex(h, out, n), h);
}
static void assert_hex(const char* expected, const uint8_t* got, size_t n) {
    char h[2 * 128 + 1];
    pairing_hex(got, n, h);
    TEST_ASSERT_EQUAL_STRING(expected, h);
}

static const char PK_I[] = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
static const char PK_D[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
static const char K_HEX[] = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
static const char N_D[] = "000102030405060708090a0b0c0d0e0f";

static void test_reference_sha256_and_hmac() {
    uint8_t out[32];
    sharef::sha256((const uint8_t*)"abc", 3, out);
    assert_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", out, 32);
    // RFC 4231 test case 2
    sharef::hmac_sha256((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, out);
    assert_hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", out, 32);
    // RFC 4231 test case 6: key longer than a block
    uint8_t key[131];
    memset(key, 0xaa, sizeof(key));
    const char m6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    sharef::hmac_sha256(key, sizeof(key), (const uint8_t*)m6, sizeof(m6) - 1, out);
    assert_hex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", out, 32);
    // A message across two blocks
    uint8_t big[200];
    for (int i = 0; i < 200; i++) big[i] = (uint8_t)i;
    sharef::sha256(big, 200, out);
    assert_hex("1901da1c9f699b48f6b2636e65cbf73abf99d0441ef67f5c540a42f7051dec6f", out, 32);   // python hashlib
}

static void test_hkdf_rfc5869_case1() {
    uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    assert_hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk, 32);
    TEST_ASSERT_TRUE(hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm)));
    assert_hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", okm, 42);
    TEST_ASSERT_FALSE(hkdf_expand(prk, info, sizeof(info), okm, 256 * 32));
}

static void test_derive_fixed_vectors() {
    uint8_t K[32], n[16], pk_i[32], pk_d[32];
    unhex(K_HEX, K, 32); unhex(N_D, n, 16); unhex(PK_I, pk_i, 32); unhex(PK_D, pk_d, 32);
    PairKeys k;
    pairing_derive(K, n, pk_i, pk_d, &k);
    assert_hex("66466507113c5afc2c0040db5238b8872fa93edf539544da5297919628d5f7af", k.kc, 32);
    assert_hex("ee415b772d8710a73bd085ec18cbe59461214226a8bcf55fbb3992b7bc6ea6c4", k.ke, 32);
    TEST_ASSERT_EQUAL_STRING("MXAA", k.chk);

    uint8_t c[32];
    pairing_confirm_i(k.kc, "482913", c);
    assert_hex("1857ad8d30a7a861bd435455216832a68e6181182f10585bbd9da419a070d899", c, 32);
    pairing_confirm_d(k.kc, "482913", c);
    assert_hex("5432473f5d7512f9119f3a34089dac429c50367eb45116ebfa80a86fddbf692a", c, 32);
    uint8_t old[32];
    memset(old, 0x42, 32);
    pairing_confirm_rekey(k.kc, old, c);
    assert_hex("2c9688ea0793bb393aa9a5136d9d200557fb41466e60786c93caca7b72fd768f", c, 32);
    memset(old, 0x33, 32);
    pairing_confirm_rekey_d(k.kc, old, c);
    assert_hex("d012b38770d37b7c42afd4440256b229e3508c4908d9dcfe8029f27b3f07cd8b", c, 32);

    uint8_t keg[32];
    pairing_group_box_key(K, n, pk_i, pk_d, keg);
    assert_hex("e2bc43c0362f4405c928bd748eaeba9a13e9df0520e107c6ea72110f68d68748", keg, 32);

    // Swapping pk_i/pk_d changes the keys (they are bound to the roles).
    PairKeys k2;
    pairing_derive(K, n, pk_d, pk_i, &k2);
    TEST_ASSERT_FALSE(pairing_ct_equal(k.kc, k2.kc, 32));
}

static void test_box_fixed_vector_and_tamper() {
    uint8_t ke[32];
    unhex("ee415b772d8710a73bd085ec18cbe59461214226a8bcf55fbb3992b7bc6ea6c4", ke, 32);
    GroupCreds c;
    memset(&c, 0, sizeof(c));
    memset(c.id, 0x11, 8);
    memset(c.secret, 0x22, 32);
    c.epoch = 3;
    strcpy(c.name, "Shelf");
    uint8_t packed[GROUP_CREDS_LEN], box[GROUP_BOX_LEN];
    pairing_creds_pack(c, packed);
    TEST_ASSERT_EQUAL_UINT(74, GROUP_CREDS_LEN);
    pairing_box_seal(ke, packed, sizeof(packed), box);
    assert_hex("17ef233e6d39e64e85a6566214dd32efcba87131b0cacdd51183947484e80610f936f4de45927bf30e54d57158e0ebad"
               "72cfcb9496cfabfef5b88d5d14358730da2060cb8c6a0101466eff11e035397c063186977a0fa3e107c3", box, sizeof(box));

    uint8_t plain[GROUP_CREDS_LEN];
    TEST_ASSERT_TRUE(pairing_box_open(ke, box, sizeof(box), plain));
    TEST_ASSERT_EQUAL_MEMORY(packed, plain, sizeof(packed));
    GroupCreds back;
    TEST_ASSERT_TRUE(pairing_creds_unpack(plain, &back));
    TEST_ASSERT_EQUAL_MEMORY(c.id, back.id, 8);
    TEST_ASSERT_EQUAL_MEMORY(c.secret, back.secret, 32);
    TEST_ASSERT_EQUAL_UINT16(3, back.epoch);
    TEST_ASSERT_EQUAL_STRING("Shelf", back.name);

    // Any flipped bit in the ciphertext or tag is rejected.
    for (size_t i = 0; i < sizeof(box); i += 7) {
        box[i] ^= 0x01;
        TEST_ASSERT_FALSE(pairing_box_open(ke, box, sizeof(box), plain));
        box[i] ^= 0x01;
    }
    // Wrong key, wrong length, too short.
    ke[0] ^= 1;
    TEST_ASSERT_FALSE(pairing_box_open(ke, box, sizeof(box), plain));
    ke[0] ^= 1;
    TEST_ASSERT_FALSE(pairing_box_open(ke, box, sizeof(box) - 1, plain));
    TEST_ASSERT_FALSE(pairing_box_open(ke, box, PAIR_TAG_LEN, plain));
    TEST_ASSERT_TRUE(pairing_box_open(ke, box, sizeof(box), plain));
}

static void test_creds_unpack_validation() {
    uint8_t p[GROUP_CREDS_LEN];
    GroupCreds c, out;
    memset(&c, 0, sizeof(c));
    memset(c.secret, 1, 32);
    strcpy(c.name, "ok name 123");
    pairing_creds_pack(c, p);
    TEST_ASSERT_TRUE(pairing_creds_unpack(p, &out));
    TEST_ASSERT_EQUAL_STRING("ok name 123", out.name);
    p[42] = '"';                                     // quote in the name
    TEST_ASSERT_FALSE(pairing_creds_unpack(p, &out));
    p[42] = 0x01;                                    // control character
    TEST_ASSERT_FALSE(pairing_creds_unpack(p, &out));
    p[42] = 'o';
    p[73] = 'x';                                     // byte after the NUL padding must be zero
    TEST_ASSERT_FALSE(pairing_creds_unpack(p, &out));
    p[73] = 0;
    memset(p + 8, 0, 32);                            // all-zero secret
    TEST_ASSERT_FALSE(pairing_creds_unpack(p, &out));
    // 31-character name without terminator inside the field is fine.
    memset(c.name, 'a', 31); c.name[31] = '\0';
    pairing_creds_pack(c, p);
    TEST_ASSERT_TRUE(pairing_creds_unpack(p, &out));
    TEST_ASSERT_EQUAL_UINT(31, strlen(out.name));
}

static uint32_t rng_seq_i = 0;
static uint32_t rng_vals[4];
static uint32_t rng_stub() { return rng_vals[rng_seq_i++ % 4]; }

static void test_code_check_word_helpers() {
    char code[7];
    rng_seq_i = 0;
    rng_vals[0] = 4294967295u;    // >= 4 294 000 000: rejected (would bias)
    rng_vals[1] = 4294000000u;    // exactly the limit: rejected
    rng_vals[2] = 4293999999u;    // accepted: % 1e6 = 999999
    rng_vals[3] = 7;
    pairing_code_from_rng(rng_stub, code);
    TEST_ASSERT_EQUAL_STRING("999999", code);
    TEST_ASSERT_EQUAL_UINT(3, rng_seq_i);
    pairing_code_from_rng(rng_stub, code);
    TEST_ASSERT_EQUAL_STRING("000007", code);

    char chk[5];
    uint8_t b0[2] = {0x00, 0xFF};
    pairing_chk_from_bytes(b0, chk);
    TEST_ASSERT_EQUAL_STRING("AAXX", chk);
    uint8_t b1[2] = {0x9f, 0x00};
    pairing_chk_from_bytes(b1, chk);
    TEST_ASSERT_EQUAL_STRING("MXAA", chk);
    TEST_ASSERT_EQUAL_UINT(16, strlen(PAIR_CHK_ALPHABET));
    for (const char* a = PAIR_CHK_ALPHABET; *a; a++) {
        TEST_ASSERT_NULL(strchr("01258BILOSUVZ", *a));   // no confusable symbols
    }

    uint8_t x[4] = {1, 2, 3, 4}, y[4] = {1, 2, 3, 5};
    TEST_ASSERT_TRUE(pairing_ct_equal(x, x, 4));
    TEST_ASSERT_FALSE(pairing_ct_equal(x, y, 4));
    char h[9];
    pairing_hex(x, 4, h);
    TEST_ASSERT_EQUAL_STRING("01020304", h);
    uint8_t z[4];
    TEST_ASSERT_TRUE(pairing_unhex("0102030A", z, 4));
    TEST_ASSERT_EQUAL_UINT8(10, z[3]);
    TEST_ASSERT_FALSE(pairing_unhex("010203", z, 4));
    TEST_ASSERT_FALSE(pairing_unhex("0102030g", z, 4));
    TEST_ASSERT_FALSE(pairing_unhex(nullptr, z, 4));
}

static void test_rate_limits_and_pairable() {
    TEST_ASSERT_EQUAL_UINT32(0, pairing_cooldown_s(0));
    TEST_ASSERT_EQUAL_UINT32(60, pairing_cooldown_s(1));
    TEST_ASSERT_EQUAL_UINT32(120, pairing_cooldown_s(2));
    TEST_ASSERT_EQUAL_UINT32(240, pairing_cooldown_s(3));
    TEST_ASSERT_EQUAL_UINT32(1920, pairing_cooldown_s(6));
    TEST_ASSERT_EQUAL_UINT32(1920, pairing_cooldown_s(7));     // capped at 32 min
    TEST_ASSERT_EQUAL_UINT32(1920, pairing_cooldown_s(255));
    TEST_ASSERT_EQUAL(90, PAIR_WINDOW_S);
    TEST_ASSERT_EQUAL(3, PAIR_MAX_ATTEMPTS);

    // pairable = USB power, nothing else.
    TEST_ASSERT_EQUAL(200, pairing_start_check(true, false, false, true));
    TEST_ASSERT_EQUAL(403, pairing_start_check(false, false, false, true));
    TEST_ASSERT_EQUAL(403, pairing_start_check(false, true, true, false));   // battery wins over everything
    TEST_ASSERT_EQUAL(429, pairing_start_check(true, true, true, true));     // cooldown before "session open"
    TEST_ASSERT_EQUAL(409, pairing_start_check(true, true, false, true));
    TEST_ASSERT_EQUAL(503, pairing_start_check(true, false, false, false));
}

static void test_beacon_signature() {
    BeaconSelf s;
    memset(&s, 0, sizeof(s));
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xD4, 0xE5, 0xF6};
    memcpy(s.mac, mac, 6);
    strcpy(s.name, "Kitchen left");
    strcpy(s.ip, "192.168.1.41");
    strcpy(s.version, "0.3.1-4-gabcdef");
    s.usb = true;
    s.rssi = -70;
    strcpy(s.group_id, "1111111111111111");
    s.epoch = 3;
    uint8_t secret[32];
    memset(secret, 0x22, 32);

    char msg[BEACON_SIGN_MAX];
    size_t ml = beacon_sign_message(s, 42, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_STRING("tickr-beacon-v1\n020000d4e5f6\nKitchen left\n192.168.1.41\nusb\n0\n42\n1111111111111111\n3", msg);
    char tag[BEACON_HEX16_LEN];
    beacon_tag(secret, msg, ml, tag);
    TEST_ASSERT_EQUAL_STRING("07a48352cc1d9b42", tag);     // HMAC-SHA-256 reference (python hmac)

    char wire[256];
    size_t n = beacon_build(s, 42, nullptr, tag, wire, sizeof(wire));
    TEST_ASSERT_TRUE(n > 0);
    BeaconIn b;
    TEST_ASSERT_TRUE(beacon_parse(wire, n, &b));
    TEST_ASSERT_EQUAL_STRING(tag, b.tag);
    // The receiver rebuilds the identical message from the parsed fields.
    char msg2[BEACON_SIGN_MAX];
    beacon_sign_message_in(b, msg2, sizeof(msg2));
    TEST_ASSERT_EQUAL_STRING(msg, msg2);
    TEST_ASSERT_TRUE(beacon_verify(b, secret));

    // Wrong secret, tampered field, upper-case tag, missing tag/group.
    uint8_t other[32];
    memset(other, 0x23, 32);
    TEST_ASSERT_FALSE(beacon_verify(b, other));
    BeaconIn t = b; t.seq++;
    TEST_ASSERT_FALSE(beacon_verify(t, secret));
    t = b; t.name[0] = 'k';
    TEST_ASSERT_FALSE(beacon_verify(t, secret));
    t = b; t.epoch = 4;
    TEST_ASSERT_FALSE(beacon_verify(t, secret));
    t = b; t.usb = false;
    TEST_ASSERT_FALSE(beacon_verify(t, secret));
    t = b; for (char* p = t.tag; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    TEST_ASSERT_TRUE(beacon_verify(t, secret));            // hex case does not matter
    t = b; t.tag[0] = '\0';
    TEST_ASSERT_FALSE(beacon_verify(t, secret));
    t = b; t.group_id[0] = '\0';
    TEST_ASSERT_FALSE(beacon_verify(t, secret));
    t = b; t.kind = BEACON_PROBE;
    TEST_ASSERT_FALSE(beacon_verify(t, secret));

    // Unsigned beacon (no group) never verifies, whatever the secret.
    BeaconSelf plain = s;
    plain.group_id[0] = '\0';
    plain.epoch = 0;
    n = beacon_build(plain, 43, nullptr, nullptr, wire, sizeof(wire));
    TEST_ASSERT_TRUE(beacon_parse(wire, n, &b));
    TEST_ASSERT_FALSE(beacon_verify(b, secret));

    // Worst-case datagram (max name/ip/version, tag, nonce) still fits.
    strcpy(s.name, "123456789012345");
    strcpy(s.ip, "192.168.100.200");
    strcpy(s.version, "12.34.56-99-gab");
    s.usb = false; s.sleep_s = 86400; s.epoch = 65535;
    n = beacon_build(s, 4294967295UL, "0123456789abcdef", tag, wire, sizeof(wire));
    TEST_ASSERT_TRUE(n > 0 && n <= BEACON_MAX_LEN);
}

int main(int, char**) {
    g_hmac_sha256 = sharef::hmac_sha256;
    UNITY_BEGIN();
    RUN_TEST(test_reference_sha256_and_hmac);
    RUN_TEST(test_hkdf_rfc5869_case1);
    RUN_TEST(test_derive_fixed_vectors);
    RUN_TEST(test_box_fixed_vector_and_tamper);
    RUN_TEST(test_creds_unpack_validation);
    RUN_TEST(test_code_check_word_helpers);
    RUN_TEST(test_rate_limits_and_pairable);
    RUN_TEST(test_beacon_signature);
    return UNITY_END();
}
