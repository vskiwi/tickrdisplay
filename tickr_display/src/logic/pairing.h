#pragma once
// Pairing and group-trust primitives (docs/MULTI_DEVICE.md "The pairing protocol", protocol v1).
//
// Pure logic: everything here is deterministic given its inputs and is
// unit-tested on the host against RFC 5869 / RFC 4231 vectors and against
// the browser implementation (www/src/_crypto.js) on fixed vectors. The only
// dependency is one HMAC-SHA-256 primitive, injected through g_hmac_sha256:
// the firmware points it at mbedtls_md_hmac (src/managers/pairing_manager.cpp),
// the host tests at a small reference SHA-256. X25519 itself is not here -
// it is mbedTLS on the device and BigInt arithmetic in the browser; this
// module starts from the 32-byte shared secret K.
//
// Labels below are part of the wire protocol - change them and every
// browser/device pair stops agreeing. They are listed in docs/MULTI_DEVICE.md "The pairing protocol".
//
//   PRK  = HKDF-Extract(salt = n (16 B), ikm = K (32 B))
//   K_c  = HKDF-Expand(PRK, "tickr-pair-v1 confirm" || pk_i || pk_d, 32)
//   K_e  = HKDF-Expand(PRK, "tickr-pair-v1 box"     || pk_i || pk_d, 32)
//   chk  = HKDF-Expand(PRK, "tickr-pair-v1 check", 2) -> 4 letters of PAIR_CHK_ALPHABET
//   c_i  = HMAC(K_c, "i" || code6)      c_d = HMAC(K_c, "d" || code6)
//   c_i  = HMAC(K_c, "rekey" || old_group_secret (32 B))         (mode rekey)
//   c_d  = HMAC(K_c, "d-rekey" || new_group_secret (32 B))       (mode rekey)
//   box  = Enc(K_e, creds): ks = HKDF-Expand(K_e, "box-ks", len), ct = m ^ ks,
//                           tag = HMAC(K_e, "box-tag" || ct)[0..16]; wire = ct || tag
//   GET /api/group/secret: K_e = HKDF-Expand(HKDF-Extract(n_a, K), "tickr-group-v1 box" || pk_i || pk_a, 32)
//   beacon tag = HMAC(group_secret, beacon_sign_message)[0..8] as 16 lower-case hex
#include <stdint.h>
#include <stddef.h>

typedef void (*HmacSha256Fn)(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]);
extern HmacSha256Fn g_hmac_sha256;   // must be set before any function below is used

#define PAIR_KEY_LEN      32
#define PAIR_PK_LEN       32
#define PAIR_NONCE_LEN    16
#define PAIR_CODE_DIGITS  6
#define PAIR_CHK_CHARS    4
#define PAIR_TAG_LEN      16     // box authentication tag (bytes)
#define PAIR_CHK_ALPHABET "ACDEFGHJKMNPRTWX"   // 16 symbols, no 0/O, 1/I/L, 2/Z, 5/S, 8/B, U/V

#define GROUP_ID_LEN      8
#define GROUP_SECRET_LEN  32
#define GROUP_NAME_MAX    31     // characters, NUL-terminated in GroupCreds::name
#define GROUP_CREDS_LEN   (GROUP_ID_LEN + GROUP_SECRET_LEN + 2 + GROUP_NAME_MAX + 1)   // 74 B packed
#define GROUP_BOX_LEN     (GROUP_CREDS_LEN + PAIR_TAG_LEN)                              // 90 B

struct GroupCreds {
    uint8_t  id[GROUP_ID_LEN];
    uint8_t  secret[GROUP_SECRET_LEN];
    uint16_t epoch;
    char     name[GROUP_NAME_MAX + 1];
};

struct PairKeys {
    uint8_t kc[PAIR_KEY_LEN];
    uint8_t ke[PAIR_KEY_LEN];
    char    chk[PAIR_CHK_CHARS + 1];
};

// RFC 5869 over HMAC-SHA-256. hkdf_expand: okm_len <= 255 * 32.
void hkdf_extract(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len, uint8_t prk[32]);
bool hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t info_len, uint8_t* okm, size_t okm_len);

// Session keys and check word from the shared secret K and the device nonce.
void pairing_derive(const uint8_t K[32], const uint8_t n[PAIR_NONCE_LEN],
                    const uint8_t pk_i[32], const uint8_t pk_d[32], PairKeys* out);
// Box key for GET /api/group/secret (label "tickr-group-v1 box").
void pairing_group_box_key(const uint8_t K[32], const uint8_t n[PAIR_NONCE_LEN],
                           const uint8_t pk_i[32], const uint8_t pk_a[32], uint8_t ke[32]);

// Confirmations. `code` is exactly PAIR_CODE_DIGITS ASCII digits.
void pairing_confirm_i(const uint8_t kc[32], const char* code, uint8_t out[32]);
void pairing_confirm_d(const uint8_t kc[32], const char* code, uint8_t out[32]);
void pairing_confirm_rekey(const uint8_t kc[32], const uint8_t old_secret[GROUP_SECRET_LEN], uint8_t out[32]);
// Device confirmation in rekey mode: HMAC(K_c, "d-rekey" || new_group_secret).
void pairing_confirm_rekey_d(const uint8_t kc[32], const uint8_t new_secret[GROUP_SECRET_LEN], uint8_t out[32]);

// Enc()/Dec(): out has room for msg_len + PAIR_TAG_LEN resp. box_len - PAIR_TAG_LEN.
// pairing_box_open verifies the tag in constant time and returns false on mismatch.
void pairing_box_seal(const uint8_t ke[32], const uint8_t* msg, size_t msg_len, uint8_t* out);
bool pairing_box_open(const uint8_t ke[32], const uint8_t* box, size_t box_len, uint8_t* out);

// Credentials <-> 74-byte wire layout (id || secret || epoch BE || name NUL-padded).
void pairing_creds_pack(const GroupCreds& c, uint8_t out[GROUP_CREDS_LEN]);
// Fails on a name that is not printable ASCII without quotes/backslashes, or an all-zero secret.
bool pairing_creds_unpack(const uint8_t in[GROUP_CREDS_LEN], GroupCreds* out);

// Constant-time comparison of n bytes.
bool pairing_ct_equal(const uint8_t* a, const uint8_t* b, size_t n);

// Six decimal digits from a 32-bit RNG with rejection sampling (no modulo bias).
void pairing_code_from_rng(uint32_t (*rng)(), char out[PAIR_CODE_DIGITS + 1]);
// Two bytes -> four letters of PAIR_CHK_ALPHABET (one nibble each).
void pairing_chk_from_bytes(const uint8_t b[2], char out[PAIR_CHK_CHARS + 1]);

// Rate limiting (docs/MULTI_DEVICE.md "Screen, state machine, limits"): cooldown after the n-th exhausted session, 1,2,4,...,32 min.
uint32_t pairing_cooldown_s(uint8_t exhausted_sessions);
#define PAIR_WINDOW_S        90
#define PAIR_MAX_ATTEMPTS    3
#define PAIR_STREAK_RESET_S  3600

// HTTP status for POST /api/pair/start given the device state (docs/MULTI_DEVICE.md "Screen, state machine, limits"):
// 200 ok, 403 not pairable (battery), 409 session open, 429 cooldown, 503 low heap.
int pairing_start_check(bool usb_power, bool session_open, bool in_cooldown, bool heap_ok);

// Hex helpers (lower-case). hex_decode returns false on odd length / non-hex.
void pairing_hex(const uint8_t* in, size_t len, char* out);
bool pairing_unhex(const char* hex, uint8_t* out, size_t out_len);
