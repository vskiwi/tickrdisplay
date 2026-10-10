#include "fmt_float.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Exact decimal expansion of a float
// ---------------------------------------------------------------------------
// |v| = m * 2^e with a 24-bit integer m. For e >= 0 the value is the integer
// m << e; for e < 0 it is m * 5^s / 10^s (s = -e), i.e. the integer m * 5^s
// with the decimal point s places from the right. Either integer is computed
// in base-1e9 limbs (<= 112 decimal digits for the smallest denormal), so the
// digit string is exact and rounding decisions below never guess.

#define FD_DIGITS 120
#define FD_LIMBS  14

struct FloatDigits {
    char digits[FD_DIGITS];  // most significant first, no leading zeros ("0" for zero)
    int  len;
    int  point;              // digits before the decimal point (may be <= 0 or > len)
};

static void limbs_mul(uint32_t* l, int* n, uint32_t f) {
    uint64_t carry = 0;
    for (int i = 0; i < *n; i++) {
        uint64_t t = (uint64_t)l[i] * f + carry;
        l[i] = (uint32_t)(t % 1000000000u);
        carry = t / 1000000000u;
    }
    if (carry) l[(*n)++] = (uint32_t)carry;
}

static void float_digits(float v, FloatDigits* d) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    uint32_t m = bits & 0x7FFFFFu;
    int exp = (int)((bits >> 23) & 0xFFu);
    int e;
    if (exp == 0) e = -149;                 // denormal (or zero)
    else { m |= 0x800000u; e = exp - 150; }
    if (m == 0) { d->digits[0] = '0'; d->len = 1; d->point = 1; return; }

    uint32_t l[FD_LIMBS] = { m };
    int n = 1;
    if (e >= 0) for (int i = 0; i < e; i++) limbs_mul(l, &n, 2);
    else        for (int i = 0; i < -e; i++) limbs_mul(l, &n, 5);

    // limbs -> digits: top limb unpadded, the rest padded to 9
    char* p = d->digits;
    for (int i = n - 1; i >= 0; i--) {
        char buf[9];
        uint32_t x = l[i];
        for (int k = 8; k >= 0; k--) { buf[k] = (char)('0' + x % 10); x /= 10; }
        int start = 0;
        if (i == n - 1) while (start < 8 && buf[start] == '0') start++;
        for (int k = start; k < 9; k++) *p++ = buf[k];
    }
    d->len = (int)(p - d->digits);
    d->point = d->len - (e < 0 ? -e : 0);
}

static char digit_at(const FloatDigits* d, int i) {
    return (i >= 0 && i < d->len) ? d->digits[i] : '0';
}

// True when digits after index `i` (exclusive) are all zero.
static bool rest_is_zero(const FloatDigits* d, int i) {
    for (int k = (i + 1 > 0 ? i + 1 : 0); k < d->len; k++) if (d->digits[k] != '0') return false;
    return true;
}

// Round-half-even decision for keeping digits [.., keep_last] of d.
static bool round_up(const FloatDigits* d, int keep_last) {
    char r = digit_at(d, keep_last + 1);
    if (r > '5') return true;
    if (r < '5') return false;
    if (!rest_is_zero(d, keep_last + 1)) return true;
    return ((digit_at(d, keep_last) - '0') & 1) != 0;   // exact tie: to even
}

// Increments the decimal string s[0..len) in place; returns true on carry out.
static bool inc_digits(char* s, int len) {
    for (int i = len - 1; i >= 0; i--) {
        if (s[i] == '9') s[i] = '0';
        else { s[i]++; return false; }
    }
    return true;
}

static bool finish(const char* buf, size_t len, char* out, size_t n) {
    if (!out || n == 0) return false;
    if (len >= n) { out[0] = '\0'; return false; }
    memcpy(out, buf, len + 1);
    return true;
}

static bool special(float v, bool neg, char* out, size_t n) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    if ((bits & 0x7F800000u) != 0x7F800000u) return false;
    const char* s = (bits & 0x7FFFFFu) ? "nan" : (neg ? "-inf" : "inf");
    finish(s, strlen(s), out, n);
    return true;
}

// ---------------------------------------------------------------------------
// "%.<frac>f"
// ---------------------------------------------------------------------------
bool fmt_float_fixed(float v, uint8_t frac, char* out, size_t n) {
    if (!out || n == 0) return false;
    if (frac > 9) frac = 9;
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    bool neg = (bits >> 31) != 0;
    if (special(v, neg, out, n)) return out[0] != '\0';

    FloatDigits d;
    float_digits(v, &d);

    // integer digits + frac fraction digits as one string, then round on the next digit
    char s[FD_DIGITS];
    int il = d.point > 0 ? d.point : 0;        // integer digits taken from d
    int len = 0;
    if (il == 0) s[len++] = '0';
    for (int i = 0; i < il; i++) s[len++] = digit_at(&d, i);
    for (int i = 0; i < frac; i++) s[len++] = digit_at(&d, d.point + i);
    bool carry = round_up(&d, d.point + frac - 1) && inc_digits(s, len);

    char buf[FD_DIGITS + 4];
    size_t o = 0;
    if (neg) buf[o++] = '-';                   // like printf: "-0.00" for -0.0 and tiny negatives
    if (carry) buf[o++] = '1';
    int ilen = len - frac;
    memcpy(buf + o, s, (size_t)ilen); o += (size_t)ilen;
    if (frac) { buf[o++] = '.'; memcpy(buf + o, s + ilen, frac); o += frac; }
    buf[o] = '\0';
    return finish(buf, o, out, n);
}

// ---------------------------------------------------------------------------
// "%g" (precision 6)
// ---------------------------------------------------------------------------
#define G_PREC 6

bool fmt_float_g(float v, char* out, size_t n) {
    if (!out || n == 0) return false;
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    bool neg = (bits >> 31) != 0;
    if (special(v, neg, out, n)) return out[0] != '\0';
    if ((bits & 0x7FFFFFFFu) == 0) return finish(neg ? "-0" : "0", neg ? 2 : 1, out, n);

    FloatDigits d;
    float_digits(v, &d);
    int x = d.point - 1;                       // decimal exponent of the leading digit

    char s[G_PREC + 1];
    for (int i = 0; i < G_PREC; i++) s[i] = digit_at(&d, i);
    if (round_up(&d, G_PREC - 1) && inc_digits(s, G_PREC)) { s[0] = '1'; x++; }
    int sig = G_PREC;
    while (sig > 1 && s[sig - 1] == '0') sig--;   // %g drops trailing zeros

    char buf[24];
    size_t o = 0;
    if (neg) buf[o++] = '-';
    if (x < -4 || x >= G_PREC) {
        buf[o++] = s[0];
        if (sig > 1) { buf[o++] = '.'; memcpy(buf + o, s + 1, (size_t)(sig - 1)); o += (size_t)(sig - 1); }
        buf[o++] = 'e';
        buf[o++] = x < 0 ? '-' : '+';
        int ax = x < 0 ? -x : x;
        if (ax >= 100) buf[o++] = (char)('0' + ax / 100);
        buf[o++] = (char)('0' + (ax / 10) % 10);
        buf[o++] = (char)('0' + ax % 10);
    } else if (x < 0) {
        buf[o++] = '0'; buf[o++] = '.';
        for (int i = 0; i < -x - 1; i++) buf[o++] = '0';
        memcpy(buf + o, s, (size_t)sig); o += (size_t)sig;
    } else {
        int ip = x + 1;                        // digits before the point
        for (int i = 0; i < ip; i++) buf[o++] = i < sig ? s[i] : '0';
        if (sig > ip) { buf[o++] = '.'; memcpy(buf + o, s + ip, (size_t)(sig - ip)); o += (size_t)(sig - ip); }
    }
    buf[o] = '\0';
    return finish(buf, o, out, n);
}
