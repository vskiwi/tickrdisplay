// Unity tests for the pure ticker helpers (src/logic/ticker.cpp) -
// docs/TICKERS.md "What the screen shows": age line, sparkline scaling, LED rule,
// direction from the change string.
#include <unity.h>
#include <string.h>
#include <math.h>
#include "logic/ticker.h"

void setUp(void) {}
void tearDown(void) {}

// --- direction from a formatted change -------------------------------------

void test_dir_from_sign_prefix(void) {
    TEST_ASSERT_EQUAL_INT(1,  ticker_dir_from_change("+0.07%"));
    TEST_ASSERT_EQUAL_INT(-1, ticker_dir_from_change("-1.23%"));
    TEST_ASSERT_EQUAL_INT(1,  ticker_dir_from_change("  +12"));
    TEST_ASSERT_EQUAL_INT(-1, ticker_dir_from_change("\xE2\x88\x92" "0.5%"));   // U+2212 minus
}

void test_dir_from_number_and_garbage(void) {
    TEST_ASSERT_EQUAL_INT(1,  ticker_dir_from_change("3.5%"));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change("0.00%"));
    TEST_ASSERT_EQUAL_INT(1,  ticker_dir_from_change("0.05%"));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change("0 %"));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change("x9"));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change("n/a"));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change(""));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change(NULL));
}

// --- age line ---------------------------------------------------------------

void test_age_line_table(void) {
    char b[24];
    // under a minute: "just now" (Q2 - a frame fresh at wake never says "0 s ago")
    ticker_age_line(0, false, b, sizeof(b));        TEST_ASSERT_EQUAL_STRING("just now", b);
    ticker_age_line(12, false, b, sizeof(b));       TEST_ASSERT_EQUAL_STRING("just now", b);
    ticker_age_line(59, false, b, sizeof(b));       TEST_ASSERT_EQUAL_STRING("just now", b);
    ticker_age_line(60, false, b, sizeof(b));       TEST_ASSERT_EQUAL_STRING("1 min ago", b);
    ticker_age_line(5 * 60 + 30, false, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("5 min ago", b);
    ticker_age_line(3599, false, b, sizeof(b));     TEST_ASSERT_EQUAL_STRING("59 min ago", b);
    ticker_age_line(3600, false, b, sizeof(b));     TEST_ASSERT_EQUAL_STRING("1 h ago", b);
    ticker_age_line(2 * 3600 + 59 * 60, false, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("2 h ago", b);
    ticker_age_line(47 * 3600, false, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("47 h ago", b);
    ticker_age_line(48 * 3600, false, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("2 d ago", b);
}

void test_age_line_stale_and_unknown(void) {
    char b[24];
    ticker_age_line(40 * 60, true, b, sizeof(b));   TEST_ASSERT_EQUAL_STRING("stale 40 min", b);
    ticker_age_line(3 * 3600, true, b, sizeof(b));  TEST_ASSERT_EQUAL_STRING("stale 3 h", b);
    ticker_age_line(3 * 86400, true, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("stale 3 d", b);
    ticker_age_line(TICKER_AGE_UNKNOWN, false, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("", b);
    ticker_age_line(TICKER_AGE_UNKNOWN, true, b, sizeof(b));  TEST_ASSERT_EQUAL_STRING("", b);
    // a tiny buffer is truncated, never overrun
    char t[6];
    ticker_age_line(12, false, t, sizeof(t));       TEST_ASSERT_EQUAL_STRING("just ", t);
    ticker_age_line(40 * 60, true, t, sizeof(t));   TEST_ASSERT_EQUAL_STRING("stale", t);
}

// --- stale crossing ------------------------------------------------------------

void test_stale_crossing_fires_once_and_never_repeats(void) {
    bool fired = false;
    const uint32_t T = 600;   // T_stale in seconds (the 10 min minimum)
    // the age line counts up towards T_stale: nothing owed yet
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 0, T));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 599, T));
    TEST_ASSERT_FALSE(fired);
    // the crossing: exactly one refresh
    TEST_ASSERT_TRUE(ticker_stale_crossing(&fired, true, 600, T));
    TEST_ASSERT_TRUE(fired);
    // a dead proxy for hours: the age keeps growing, no second refresh
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 601, T));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 3600, T));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 86400, T));
    TEST_ASSERT_TRUE(fired);
}

void test_stale_crossing_rearms_with_new_content(void) {
    bool fired = false;
    TEST_ASSERT_TRUE(ticker_stale_crossing(&fired, true, 700, 600));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 800, 600));
    // new content arrived (display_show_content clears the flag); its own
    // age line starts young and crosses again later - once
    fired = false;
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 30, 600));
    TEST_ASSERT_TRUE(ticker_stale_crossing(&fired, true, 600, 600));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 660, 600));
    // content that is already stale when it arrives (age_s from the payload) fires at once, once
    fired = false;
    TEST_ASSERT_TRUE(ticker_stale_crossing(&fired, true, 590 + 10, 600));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 610, 600));
}

void test_stale_crossing_needs_an_age_line(void) {
    bool fired = false;
    // a Text frame or a ticker with a pass-through `time`: no marker, no refresh, flag untouched
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, false, 5000, 600));
    TEST_ASSERT_FALSE(fired);
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, TICKER_AGE_UNKNOWN, 600));
    TEST_ASSERT_FALSE(fired);
    TEST_ASSERT_FALSE(ticker_stale_crossing(NULL, true, 5000, 600));
    // a larger T_stale (3 x a 20 min interval) moves the crossing, same rule
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 3599, 3600));
    TEST_ASSERT_TRUE(ticker_stale_crossing(&fired, true, 3600, 3600));
    TEST_ASSERT_FALSE(ticker_stale_crossing(&fired, true, 7200, 3600));
}

// --- sparkline scaling -------------------------------------------------------

void test_spark_minmax_to_height(void) {
    const float in[] = { 10, 20, 30, 40 };
    uint8_t out[TICKER_SPARK_MAX];
    TEST_ASSERT_EQUAL_UINT8(4, ticker_spark_scale(in, 4, out, TICKER_SPARK_H));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);                 // min -> bottom row
    TEST_ASSERT_EQUAL_UINT8(TICKER_SPARK_H - 1, out[3]); // max -> top row
    TEST_ASSERT_TRUE(out[1] > out[0] && out[1] < out[2] && out[2] < out[3]);
    TEST_ASSERT_EQUAL_UINT8(8, out[1]);                 // 1/3 of 23 = 7.67 -> 8
}

void test_spark_flat_single_and_nan(void) {
    uint8_t out[TICKER_SPARK_MAX];
    const float flat[] = { 5, 5, 5 };
    TEST_ASSERT_EQUAL_UINT8(3, ticker_spark_scale(flat, 3, out, TICKER_SPARK_H));
    TEST_ASSERT_EQUAL_UINT8((TICKER_SPARK_H - 1) / 2, out[0]);   // mid line
    TEST_ASSERT_EQUAL_UINT8(out[0], out[2]);
    const float one[] = { 84000.06f };
    TEST_ASSERT_EQUAL_UINT8(1, ticker_spark_scale(one, 1, out, TICKER_SPARK_H));
    TEST_ASSERT_EQUAL_UINT8((TICKER_SPARK_H - 1) / 2, out[0]);
    const float nan_in[] = { 1, NAN, 3, INFINITY, 2 };
    TEST_ASSERT_EQUAL_UINT8(3, ticker_spark_scale(nan_in, 5, out, TICKER_SPARK_H));   // NaN / inf skipped
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);
    TEST_ASSERT_EQUAL_UINT8(TICKER_SPARK_H - 1, out[1]);
    TEST_ASSERT_EQUAL_UINT8(12, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0, ticker_spark_scale(nan_in, 0, out, TICKER_SPARK_H));
    TEST_ASSERT_EQUAL_UINT8(0, ticker_spark_scale(NULL, 3, out, TICKER_SPARK_H));
}

void test_spark_caps_at_48_and_huge_values(void) {
    float in[60];
    for (int i = 0; i < 60; i++) in[i] = 1e30f * (float)(i + 1);
    uint8_t out[TICKER_SPARK_MAX];
    memset(out, 0xAA, sizeof(out));
    TEST_ASSERT_EQUAL_UINT8(TICKER_SPARK_MAX, ticker_spark_scale(in, 60, out, TICKER_SPARK_H));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);
    TEST_ASSERT_EQUAL_UINT8(TICKER_SPARK_H - 1, out[TICKER_SPARK_MAX - 1]);   // the 48th, not the 60th, is the max
    for (int i = 0; i < TICKER_SPARK_MAX; i++) TEST_ASSERT_TRUE(out[i] < TICKER_SPARK_H);
}

// --- price without its fraction ------------------------------------------------

static const char* trim(const char* in, char* out, size_t cap) {
    memset(out, 'X', cap);
    return ticker_price_trim_round(in, out, cap) ? out : NULL;
}

void test_price_trim_rounds_half_up(void) {
    char b[32];
    TEST_ASSERT_EQUAL_STRING("84 015", trim("84 014.90", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("84 014", trim("84 014.49", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("84 015", trim("84 014.50", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("84 014", trim("84 014.4999", b, sizeof(b)));   // only the first fraction digit decides
    TEST_ASSERT_EQUAL_STRING("1 000", trim("1 000.0", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("-1 235", trim("-1 234.50", b, sizeof(b)));
}

void test_price_trim_carry_regroups(void) {
    char b[32];
    TEST_ASSERT_EQUAL_STRING("10 000", trim("9 999.99", b, sizeof(b)));       // a new group, same separator
    TEST_ASSERT_EQUAL_STRING("10,000", trim("9,999.5", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("10000", trim("9999.99", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1 000 000", trim("999 999.50", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("11 000", trim("10 999.5", b, sizeof(b)));
}

void test_price_trim_separators(void) {
    char b[32];
    TEST_ASSERT_EQUAL_STRING("84,015", trim("84,014.90", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("84015", trim("84014.90", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1 234 568", trim("1 234 567.89", b, sizeof(b)));
    // "2 695.42" trims fine - the renderer just never asks: the whole string
    // already fits the largest size, so the cents stay on the panel
    TEST_ASSERT_EQUAL_STRING("2 695", trim("2 695.42", b, sizeof(b)));
    // not a thousands grouping: mixed separators, wrong group sizes
    TEST_ASSERT_NULL(trim("84,014 567.9", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("1 2345.6", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("12 34 567.8", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("1 23.45", b, sizeof(b)));
}

void test_price_trim_refuses_what_it_should(void) {
    char b[32];
    TEST_ASSERT_NULL(trim("999.99", b, sizeof(b)));          // under 1 000: the cents matter
    TEST_ASSERT_NULL(trim("1 234 567", b, sizeof(b)));       // no fraction to drop
    TEST_ASSERT_NULL(trim("84 014.", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("0.08123456", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("0 123.45", b, sizeof(b)));        // leading zero
    TEST_ASSERT_NULL(trim("abc", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("", b, sizeof(b)));
    TEST_ASSERT_NULL(trim(NULL, b, sizeof(b)));
    // a proxy's free text: currency signs, blanks, a percent - drawn as sent
    TEST_ASSERT_NULL(trim("$95,240.50", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("95 240.50 USD", b, sizeof(b)));
    TEST_ASSERT_NULL(trim(" 84 014.90", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("84 014.90%", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("+84 014.90", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("84 014.9a", b, sizeof(b)));
    TEST_ASSERT_NULL(trim("12345678901234567890.5", b, sizeof(b)));   // too many digits for the buffer
    TEST_ASSERT_EQUAL_CHAR('\0', b[0]);                      // a refusal leaves an empty string
}

void test_price_trim_tight_buffer(void) {
    char b[7];
    TEST_ASSERT_EQUAL_STRING("84 015", trim("84 014.90", b, sizeof(b)));   // exactly fits
    char t[6];
    TEST_ASSERT_NULL(trim("84 014.90", t, sizeof(t)));       // one short: refused, never overrun
    TEST_ASSERT_EQUAL_CHAR('\0', t[0]);
    TEST_ASSERT_NULL(trim("-1 234.50", t, sizeof(t)));
    char one[1];
    TEST_ASSERT_NULL(trim("84 014.90", one, sizeof(one)));
    TEST_ASSERT_FALSE(ticker_price_trim_round("84 014.90", b, 0));
    TEST_ASSERT_FALSE(ticker_price_trim_round("84 014.90", NULL, 8));
}

// --- LED rule ------------------------------------------------------------------

void test_led_rule_off_uses_payload_led_only(void) {
    uint8_t r = 7, g = 7, b = 7;
    TEST_ASSERT_FALSE(ticker_led_decide(LED_RULE_OFF, false, 0, 0, 0, true, 1, &r, &g, &b));
    TEST_ASSERT_FALSE(ticker_led_decide(LED_RULE_OFF, false, 0, 0, 0, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(7, r);   // untouched
    TEST_ASSERT_TRUE(ticker_led_decide(LED_RULE_OFF, true, 0x12, 0x34, 0x56, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0x12, r); TEST_ASSERT_EQUAL_UINT8(0x34, g); TEST_ASSERT_EQUAL_UINT8(0x56, b);
}

void test_led_rule_sign_table(void) {
    uint8_t r, g, b;
    TEST_ASSERT_TRUE(ticker_led_decide(LED_RULE_SIGN, false, 0, 0, 0, true, 1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r); TEST_ASSERT_EQUAL_UINT8(255, g); TEST_ASSERT_EQUAL_UINT8(0, b);   // up = green
    TEST_ASSERT_TRUE(ticker_led_decide(LED_RULE_SIGN, false, 0, 0, 0, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, r); TEST_ASSERT_EQUAL_UINT8(0, g); TEST_ASSERT_EQUAL_UINT8(0, b);   // down = red
    r = g = b = 9;
    TEST_ASSERT_FALSE(ticker_led_decide(LED_RULE_SIGN, false, 0, 0, 0, true, 0, &r, &g, &b));      // flat = unchanged
    TEST_ASSERT_FALSE(ticker_led_decide(LED_RULE_SIGN, false, 0, 0, 0, false, 1, &r, &g, &b));     // no direction = unchanged
    TEST_ASSERT_EQUAL_UINT8(9, r);
    // an explicit alert.led wins over the rule for that frame
    TEST_ASSERT_TRUE(ticker_led_decide(LED_RULE_SIGN, true, 0, 0, 255, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r); TEST_ASSERT_EQUAL_UINT8(0, g); TEST_ASSERT_EQUAL_UINT8(255, b);
}

void test_led_rule_strings(void) {
    TEST_ASSERT_EQUAL_STRING("off", led_rule_str(LED_RULE_OFF));
    TEST_ASSERT_EQUAL_STRING("sign", led_rule_str(LED_RULE_SIGN));
    TEST_ASSERT_EQUAL_STRING("off", led_rule_str(200));
    TEST_ASSERT_EQUAL_UINT8(LED_RULE_SIGN, led_rule_parse("sign"));
    TEST_ASSERT_EQUAL_UINT8(LED_RULE_OFF, led_rule_parse("off"));
    TEST_ASSERT_EQUAL_UINT8(LED_RULE_OFF, led_rule_parse("alert"));
    TEST_ASSERT_EQUAL_UINT8(LED_RULE_OFF, led_rule_parse(NULL));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_dir_from_sign_prefix);
    RUN_TEST(test_dir_from_number_and_garbage);
    RUN_TEST(test_age_line_table);
    RUN_TEST(test_age_line_stale_and_unknown);
    RUN_TEST(test_stale_crossing_fires_once_and_never_repeats);
    RUN_TEST(test_stale_crossing_rearms_with_new_content);
    RUN_TEST(test_stale_crossing_needs_an_age_line);
    RUN_TEST(test_spark_minmax_to_height);
    RUN_TEST(test_spark_flat_single_and_nan);
    RUN_TEST(test_spark_caps_at_48_and_huge_values);
    RUN_TEST(test_price_trim_rounds_half_up);
    RUN_TEST(test_price_trim_carry_regroups);
    RUN_TEST(test_price_trim_separators);
    RUN_TEST(test_price_trim_refuses_what_it_should);
    RUN_TEST(test_price_trim_tight_buffer);
    RUN_TEST(test_led_rule_off_uses_payload_led_only);
    RUN_TEST(test_led_rule_sign_table);
    RUN_TEST(test_led_rule_strings);
    return UNITY_END();
}
