// Unity tests for the on-device sparkline history (src/logic/spark_hist.cpp) -
// docs/TICKERS.md "Sparkline history": 48 x uint16 ring scaled to the window's min-max.
#include <unity.h>
#include <string.h>
#include <math.h>
#include "logic/spark_hist.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t K = 0x1234abcd;

void test_struct_is_the_documented_size(void) {
    TEST_ASSERT_EQUAL_UINT(96, sizeof(((SparkHist*)0)->v));       // the 96 B ring
    TEST_ASSERT_TRUE(sizeof(SparkHist) <= 112);                     // + key, lo/hi, n/head
}

void test_push_and_read_back_in_order(void) {
    SparkHist h;
    spark_hist_reset(&h, K);
    float out[SPARK_HIST_N];
    TEST_ASSERT_EQUAL_UINT8(0, spark_hist_values(&h, out, SPARK_HIST_N));
    spark_hist_push(&h, K, 100.0f);
    spark_hist_push(&h, K, 110.0f);
    spark_hist_push(&h, K, 105.0f);
    TEST_ASSERT_EQUAL_UINT8(3, spark_hist_values(&h, out, SPARK_HIST_N));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 110.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 105.0f, out[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 100.0f, h.lo);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 110.0f, h.hi);
}

void test_ring_keeps_the_last_48(void) {
    SparkHist h;
    spark_hist_reset(&h, K);
    for (int i = 0; i < 60; i++) spark_hist_push(&h, K, 1000.0f + (float)i);
    float out[SPARK_HIST_N];
    TEST_ASSERT_EQUAL_UINT8(SPARK_HIST_N, spark_hist_values(&h, out, SPARK_HIST_N));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1012.0f, out[0]);               // oldest kept = the 13th push
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1059.0f, out[SPARK_HIST_N - 1]);
    // the window range still covers the dropped points (lo stays at 1000): quantisation error <= range / 65535
    for (int i = 1; i < SPARK_HIST_N; i++) TEST_ASSERT_TRUE(out[i] > out[i - 1]);
}

void test_range_widens_and_requantises(void) {
    SparkHist h;
    spark_hist_reset(&h, K);
    spark_hist_push(&h, K, 84000.0f);
    spark_hist_push(&h, K, 84010.0f);
    spark_hist_push(&h, K, 83000.0f);     // below lo -> everything re-quantised into [83000, 84010]
    spark_hist_push(&h, K, 85000.0f);     // above hi
    float out[SPARK_HIST_N];
    TEST_ASSERT_EQUAL_UINT8(4, spark_hist_values(&h, out, SPARK_HIST_N));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 84000.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 84010.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 83000.0f, out[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 85000.0f, out[3]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 83000.0f, h.lo);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 85000.0f, h.hi);
}

void test_key_change_clears_the_ring(void) {
    SparkHist h;
    spark_hist_reset(&h, K);
    spark_hist_push(&h, K, 1.0f);
    spark_hist_push(&h, K, 2.0f);
    spark_hist_push(&h, K + 1, 50.0f);    // the symbol changed
    float out[SPARK_HIST_N];
    TEST_ASSERT_EQUAL_UINT8(1, spark_hist_values(&h, out, SPARK_HIST_N));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 50.0f, out[0]);
    TEST_ASSERT_EQUAL_UINT32(K + 1, h.key);
}

void test_random_rtc_memory_is_reset_on_first_push(void) {
    SparkHist h;
    memset(&h, 0xA7, sizeof(h));          // a cold boot: RTC slow memory is garbage
    h.key = K;                            // ... even with a matching key
    float out[SPARK_HIST_N];
    TEST_ASSERT_EQUAL_UINT8(0, spark_hist_values(&h, out, SPARK_HIST_N));   // n / head out of range -> nothing
    spark_hist_push(&h, K, 7.0f);
    TEST_ASSERT_EQUAL_UINT8(1, spark_hist_values(&h, out, SPARK_HIST_N));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.0f, out[0]);
}

void test_non_finite_prices_are_ignored(void) {
    SparkHist h;
    spark_hist_reset(&h, K);
    spark_hist_push(&h, K, 3.0f);
    spark_hist_push(&h, K, NAN);
    spark_hist_push(&h, K, INFINITY);
    float out[SPARK_HIST_N];
    TEST_ASSERT_EQUAL_UINT8(1, spark_hist_values(&h, out, SPARK_HIST_N));
}

void test_flat_series_and_output_cap(void) {
    SparkHist h;
    spark_hist_reset(&h, K);
    for (int i = 0; i < 5; i++) spark_hist_push(&h, K, 42.0f);
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(3, spark_hist_values(&h, out, 3));      // bounded by the caller's buffer
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 42.0f, out[2]);
    // feeds the sparkline scaler: a flat series is a mid line
    float all[SPARK_HIST_N];
    uint8_t rows[TICKER_SPARK_MAX];
    uint8_t n = spark_hist_values(&h, all, SPARK_HIST_N);
    TEST_ASSERT_EQUAL_UINT8(5, ticker_spark_scale(all, n, rows, TICKER_SPARK_H));
    TEST_ASSERT_EQUAL_UINT8((TICKER_SPARK_H - 1) / 2, rows[0]);
}

void test_key_is_fnv1a_of_the_url(void) {
    TEST_ASSERT_EQUAL_UINT32(0x811c9dc5u, spark_hist_key(""));       // FNV offset basis
    TEST_ASSERT_EQUAL_UINT32(0xe40c292cu, spark_hist_key("a"));      // FNV-1a("a")
    TEST_ASSERT_NOT_EQUAL(spark_hist_key("https://x/BTCUSDT"), spark_hist_key("https://x/ETHUSDT"));
    TEST_ASSERT_EQUAL_UINT32(spark_hist_key(NULL), spark_hist_key(""));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_struct_is_the_documented_size);
    RUN_TEST(test_push_and_read_back_in_order);
    RUN_TEST(test_ring_keeps_the_last_48);
    RUN_TEST(test_range_widens_and_requantises);
    RUN_TEST(test_key_change_clears_the_ring);
    RUN_TEST(test_random_rtc_memory_is_reset_on_first_push);
    RUN_TEST(test_non_finite_prices_are_ignored);
    RUN_TEST(test_flat_series_and_output_cap);
    RUN_TEST(test_key_is_fnv1a_of_the_url);
    return UNITY_END();
}
