#include "pairing.h"
#include <string.h>

HmacSha256Fn g_hmac_sha256 = nullptr;

static const char L_CONFIRM[] = "tickr-pair-v1 confirm";
static const char L_BOX[]     = "tickr-pair-v1 box";
static const char L_CHECK[]   = "tickr-pair-v1 check";
static const char L_GROUP[]   = "tickr-group-v1 box";
static const char L_KS[]      = "box-ks";
static const char L_TAG[]     = "box-tag";
static const char L_REKEY[]   = "rekey";

static void hmac(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
    if (g_hmac_sha256) g_hmac_sha256(key, key_len, msg, msg_len, out);
    else memset(out, 0, 32);
}

void hkdf_extract(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len, uint8_t prk[32]) {
    hmac(salt, salt_len, ikm, ikm_len, prk);
}

bool hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t info_len, uint8_t* okm, size_t okm_len) {
    if (okm_len > 255 * 32 || info_len > 96) return false;
    uint8_t t[32 + 96 + 1];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t done = 0;
    while (done < okm_len) {
        memcpy(t + t_len, info, info_len);
        t[t_len + info_len] = counter++;
        hmac(prk, 32, t, t_len + info_len + 1, t);
        t_len = 32;
        size_t n = okm_len - done < 32 ? okm_len - done : 32;
        memcpy(okm + done, t, n);
        done += n;
    }
    return true;
}

// label || pk_i || pk_d as HKDF info
static void expand_pk(const uint8_t prk[32], const char* label, const uint8_t a[32], const uint8_t b[32],
                      uint8_t* out, size_t out_len) {
    uint8_t info[32 + 64];
    size_t l = strlen(label);
    memcpy(info, label, l);
    memcpy(info + l, a, 32);
    memcpy(info + l + 32, b, 32);
    hkdf_expand(prk, info, l + 64, out, out_len);
}

void pairing_derive(const uint8_t K[32], const uint8_t n[PAIR_NONCE_LEN],
                    const uint8_t pk_i[32], const uint8_t pk_d[32], PairKeys* out) {
    uint8_t prk[32], chk[2];
    hkdf_extract(n, PAIR_NONCE_LEN, K, 32, prk);
    expand_pk(prk, L_CONFIRM, pk_i, pk_d, out->kc, 32);
    expand_pk(prk, L_BOX, pk_i, pk_d, out->ke, 32);
    hkdf_expand(prk, (const uint8_t*)L_CHECK, sizeof(L_CHECK) - 1, chk, 2);
    pairing_chk_from_bytes(chk, out->chk);
}

void pairing_group_box_key(const uint8_t K[32], const uint8_t n[PAIR_NONCE_LEN],
                           const uint8_t pk_i[32], const uint8_t pk_a[32], uint8_t ke[32]) {
    uint8_t prk[32];
    hkdf_extract(n, PAIR_NONCE_LEN, K, 32, prk);
    expand_pk(prk, L_GROUP, pk_i, pk_a, ke, 32);
}

static void confirm(const uint8_t kc[32], char prefix, const char* code, uint8_t out[32]) {
    uint8_t m[1 + PAIR_CODE_DIGITS];
    m[0] = (uint8_t)prefix;
    memcpy(m + 1, code, PAIR_CODE_DIGITS);
    hmac(kc, 32, m, sizeof(m), out);
}

void pairing_confirm_i(const uint8_t kc[32], const char* code, uint8_t out[32]) { confirm(kc, 'i', code, out); }
void pairing_confirm_d(const uint8_t kc[32], const char* code, uint8_t out[32]) { confirm(kc, 'd', code, out); }

static void confirm_secret(const uint8_t kc[32], const char* label, const uint8_t secret[GROUP_SECRET_LEN], uint8_t out[32]) {
    uint8_t m[8 + GROUP_SECRET_LEN];
    size_t l = strlen(label);
    memcpy(m, label, l);
    memcpy(m + l, secret, GROUP_SECRET_LEN);
    hmac(kc, 32, m, l + GROUP_SECRET_LEN, out);
}

void pairing_confirm_rekey(const uint8_t kc[32], const uint8_t old_secret[GROUP_SECRET_LEN], uint8_t out[32]) {
    confirm_secret(kc, L_REKEY, old_secret, out);
}

void pairing_confirm_rekey_d(const uint8_t kc[32], const uint8_t new_secret[GROUP_SECRET_LEN], uint8_t out[32]) {
    confirm_secret(kc, "d-rekey", new_secret, out);
}

// tag = HMAC(ke, "box-tag" || ct)[0..16]
static void box_tag(const uint8_t ke[32], const uint8_t* ct, size_t ct_len, uint8_t tag[PAIR_TAG_LEN]) {
    uint8_t m[sizeof(L_TAG) - 1 + GROUP_CREDS_LEN + 32];
    if (ct_len > sizeof(m) - (sizeof(L_TAG) - 1)) { memset(tag, 0, PAIR_TAG_LEN); return; }
    memcpy(m, L_TAG, sizeof(L_TAG) - 1);
    memcpy(m + sizeof(L_TAG) - 1, ct, ct_len);
    uint8_t full[32];
    hmac(ke, 32, m, sizeof(L_TAG) - 1 + ct_len, full);
    memcpy(tag, full, PAIR_TAG_LEN);
}

static void box_xor(const uint8_t ke[32], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t ks[GROUP_CREDS_LEN + 32];
    if (len > sizeof(ks)) len = sizeof(ks);
    hkdf_expand(ke, (const uint8_t*)L_KS, sizeof(L_KS) - 1, ks, len);
    for (size_t i = 0; i < len; i++) out[i] = in[i] ^ ks[i];
}

void pairing_box_seal(const uint8_t ke[32], const uint8_t* msg, size_t msg_len, uint8_t* out) {
    box_xor(ke, msg, msg_len, out);
    box_tag(ke, out, msg_len, out + msg_len);
}

bool pairing_box_open(const uint8_t ke[32], const uint8_t* box, size_t box_len, uint8_t* out) {
    if (box_len <= PAIR_TAG_LEN || box_len > GROUP_CREDS_LEN + 32 + PAIR_TAG_LEN) return false;
    size_t ct_len = box_len - PAIR_TAG_LEN;
    uint8_t tag[PAIR_TAG_LEN];
    box_tag(ke, box, ct_len, tag);
    if (!pairing_ct_equal(tag, box + ct_len, PAIR_TAG_LEN)) return false;
    box_xor(ke, box, ct_len, out);
    return true;
}

void pairing_creds_pack(const GroupCreds& c, uint8_t out[GROUP_CREDS_LEN]) {
    memset(out, 0, GROUP_CREDS_LEN);
    memcpy(out, c.id, GROUP_ID_LEN);
    memcpy(out + GROUP_ID_LEN, c.secret, GROUP_SECRET_LEN);
    out[GROUP_ID_LEN + GROUP_SECRET_LEN] = (uint8_t)(c.epoch >> 8);
    out[GROUP_ID_LEN + GROUP_SECRET_LEN + 1] = (uint8_t)c.epoch;
    strncpy((char*)out + GROUP_ID_LEN + GROUP_SECRET_LEN + 2, c.name, GROUP_NAME_MAX);
}

bool pairing_creds_unpack(const uint8_t in[GROUP_CREDS_LEN], GroupCreds* out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->id, in, GROUP_ID_LEN);
    memcpy(out->secret, in + GROUP_ID_LEN, GROUP_SECRET_LEN);
    out->epoch = (uint16_t)((in[GROUP_ID_LEN + GROUP_SECRET_LEN] << 8) | in[GROUP_ID_LEN + GROUP_SECRET_LEN + 1]);
    const uint8_t* name = in + GROUP_ID_LEN + GROUP_SECRET_LEN + 2;
    size_t i = 0;
    for (; i < GROUP_NAME_MAX && name[i]; i++) {
        uint8_t c = name[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
        out->name[i] = (char)c;
    }
    for (; i < GROUP_NAME_MAX + 1; i++) {                 // the padding must be NUL
        if (name[i] != 0) return false;
    }
    uint8_t acc = 0;
    for (size_t k = 0; k < GROUP_SECRET_LEN; k++) acc |= out->secret[k];
    return acc != 0;
}

bool pairing_ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void pairing_code_from_rng(uint32_t (*rng)(), char out[PAIR_CODE_DIGITS + 1]) {
    // Rejection sampling: accept only below the largest multiple of 10^6.
    const uint32_t limit = 4294000000u;
    uint32_t r;
    do { r = rng(); } while (r >= limit);
    r %= 1000000u;
    for (int i = PAIR_CODE_DIGITS - 1; i >= 0; i--) {
        out[i] = (char)('0' + r % 10);
        r /= 10;
    }
    out[PAIR_CODE_DIGITS] = '\0';
}

void pairing_chk_from_bytes(const uint8_t b[2], char out[PAIR_CHK_CHARS + 1]) {
    static const char A[] = PAIR_CHK_ALPHABET;
    out[0] = A[b[0] >> 4];
    out[1] = A[b[0] & 15];
    out[2] = A[b[1] >> 4];
    out[3] = A[b[1] & 15];
    out[4] = '\0';
}

uint32_t pairing_cooldown_s(uint8_t exhausted_sessions) {
    if (exhausted_sessions == 0) return 0;
    uint32_t m = exhausted_sessions > 6 ? 32 : (1u << (exhausted_sessions - 1));   // 1,2,4,8,16,32
    return m * 60;
}

int pairing_start_check(bool usb_power, bool session_open, bool in_cooldown, bool heap_ok) {
    if (!usb_power) return 403;
    if (in_cooldown) return 429;
    if (session_open) return 409;
    if (!heap_ok) return 503;
    return 200;
}

void pairing_hex(const uint8_t* in, size_t len, char* out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 15];
    }
    out[2 * len] = '\0';
}

static int hv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pairing_unhex(const char* hex, uint8_t* out, size_t out_len) {
    if (!hex || strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        int a = hv(hex[2 * i]), b = hv(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}
