// Unity tests for src/logic/fmt_float.cpp - the integer-only replacement for
// snprintf("%g") / snprintf("%.<n>f") (the firmware links the ROM nano printf,
// docs/DEVELOPMENT.md "Build flags"). The host's printf is the reference:
// every output must be byte-identical.
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "logic/fmt_float.h"

void setUp(void) {}
void tearDown(void) {}

// The C standard removes trailing zeros from a %g mantissa. Apple's libc keeps
// one after a half-to-even rounding in exponent style ("-2.47860e+06" for
// -2478605); normalise the reference so the test also passes there.
static void strip_g_zeros(char* s) {
    char* e = strchr(s, 'e');
    if (!e || !strchr(s, '.')) return;
    char* p = e;
    while (p > s && p[-1] == '0') p--;
    if (p > s && p[-1] == '.') p--;
    memmove(p, e, strlen(e) + 1);
}

static void check_g(float v) {
    char ref[64], got[64];
    snprintf(ref, sizeof(ref), "%g", (double)v);
    strip_g_zeros(ref);
    TEST_ASSERT_TRUE_MESSAGE(fmt_float_g(v, got, sizeof(got)), ref);
    TEST_ASSERT_EQUAL_STRING(ref, got);
}

static void check_fixed(float v, unsigned frac) {
    char fmt[8], ref[96], got[96];
    snprintf(fmt, sizeof(fmt), "%%.%uf", frac);
    snprintf(ref, sizeof(ref), fmt, (double)v);
    TEST_ASSERT_TRUE_MESSAGE(fmt_float_fixed(v, (uint8_t)frac, got, sizeof(got)), ref);
    TEST_ASSERT_EQUAL_STRING(ref, got);
}

static const float kValues[] = {
    0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, 0.25f, 0.125f,
    123.456f, -123.456f, 84000.06f, 84000.0f, 0.00001234f, 1e-7f, 1e-4f, 1e-5f, 0.0001f, 0.00001f,
    1e6f, 999999.0f, 999999.5f, 9999995.0f, 1e12f, 123456789.0f, 1e15f, 1e18f, 1e19f, 3.4e38f,
    0.1f, 0.2f, 0.3f, 1.0f / 3.0f, 2.0f / 3.0f, 0.001953125f, 0.0000000149f,
    65504.0f, 65535.0f, 65536.0f, 1e-45f, FLT_MIN, FLT_MAX, FLT_EPSILON, -FLT_MIN,
    12345.678f, 0.07f, 1.005f, 2.675f, 100.0f, 1000000.0f, 100000.0f, 0.000099999f,
};

void test_g_matches_printf_on_known_values(void) {
    for (size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); i++) check_g(kValues[i]);
}

void test_fixed_8_matches_printf_on_known_values(void) {
    for (size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); i++) check_fixed(kValues[i], 8);
}

void test_fixed_other_precisions(void) {
    for (size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); i++) {
        if (fabsf(kValues[i]) >= 1e18f) continue;     // ref buffer: the ROM formatter is not used for these anyway
        check_fixed(kValues[i], 0);
        check_fixed(kValues[i], 2);
        check_fixed(kValues[i], 4);
        check_fixed(kValues[i], 9);
    }
}

void test_half_even_ties(void) {
    // exact binary ties: 2^-9 = 0.001953125 -> %.8f rounds the last 5 to even
    check_fixed(0.001953125f, 8);
    check_fixed(0.5f, 0); check_fixed(1.5f, 0); check_fixed(2.5f, 0);
    check_fixed(0.125f, 2); check_fixed(0.375f, 2);
    check_g(2.5f); check_g(1234565.0f); check_g(1234575.0f);
}

void test_non_finite(void) {
    char got[16];
    TEST_ASSERT_TRUE(fmt_float_g(INFINITY, got, sizeof(got)));  TEST_ASSERT_EQUAL_STRING("inf", got);
    TEST_ASSERT_TRUE(fmt_float_g(-INFINITY, got, sizeof(got))); TEST_ASSERT_EQUAL_STRING("-inf", got);
    TEST_ASSERT_TRUE(fmt_float_g(NAN, got, sizeof(got)));       TEST_ASSERT_EQUAL_STRING("nan", got);
    TEST_ASSERT_TRUE(fmt_float_fixed(INFINITY, 8, got, sizeof(got)));  TEST_ASSERT_EQUAL_STRING("inf", got);
    TEST_ASSERT_TRUE(fmt_float_fixed(-INFINITY, 8, got, sizeof(got))); TEST_ASSERT_EQUAL_STRING("-inf", got);
    TEST_ASSERT_TRUE(fmt_float_fixed(NAN, 8, got, sizeof(got)));       TEST_ASSERT_EQUAL_STRING("nan", got);
}

void test_buffer_too_small(void) {
    char got[8];
    TEST_ASSERT_FALSE(fmt_float_fixed(84000.06f, 8, got, sizeof(got)));   // "84000.06000000" needs 15
    TEST_ASSERT_EQUAL_STRING("", got);
    TEST_ASSERT_FALSE(fmt_float_g(1.23457e+38f, got, 4));
    TEST_ASSERT_FALSE(fmt_float_g(1.0f, NULL, 4));
    TEST_ASSERT_FALSE(fmt_float_g(1.0f, got, 0));
    TEST_ASSERT_TRUE(fmt_float_g(1.0f, got, 2));
    TEST_ASSERT_EQUAL_STRING("1", got);
}

// Every finite bit pattern a 32-bit LCG produces: exponent span of the whole
// float range, both signs; a few thousand per run keep the test fast.
void test_random_bit_patterns_match_printf(void) {
    uint32_t x = 0x9E3779B9u;
    int checked = 0;
    for (int i = 0; i < 20000; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t bits = x;
        if (((bits >> 23) & 0xFFu) == 0xFFu) continue;        // skip inf/nan
        float v;
        memcpy(&v, &bits, sizeof(v));
        check_g(v);
        if (fabsf(v) < 1e18f) check_fixed(v, 8);
        checked++;
    }
    TEST_ASSERT_TRUE(checked > 15000);
}

// Values with the magnitude of ticker prices and percentages (the real use).
void test_price_like_values_match_printf(void) {
    uint32_t x = 12345u;
    for (int i = 0; i < 20000; i++) {
        x = x * 1664525u + 1013904223u;
        float mant = (float)(x >> 8) / 16777216.0f;             // [0, 1)
        int e = (int)((x & 0xFF) % 16) - 8;                     // 1e-8 .. 1e7
        float v = mant * powf(10.0f, (float)e);
        if (x & 1) v = -v;
        check_g(v);
        check_fixed(v, 8);
        check_fixed(v, 2);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_g_matches_printf_on_known_values);
    RUN_TEST(test_fixed_8_matches_printf_on_known_values);
    RUN_TEST(test_fixed_other_precisions);
    RUN_TEST(test_half_even_ties);
    RUN_TEST(test_non_finite);
    RUN_TEST(test_buffer_too_small);
    RUN_TEST(test_random_bit_patterns_match_printf);
    RUN_TEST(test_price_like_values_match_printf);
    return UNITY_END();
}
