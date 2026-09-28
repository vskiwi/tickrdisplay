// Unity tests for the USB-mode pull scheduler (src/logic/pull_scheduler.cpp) -
// docs/TICKERS.md "Fetch schedule and errors": interval + jitter, back-off on failures with a cap,
// reset on success, fetch-now on a configuration change.
#include <unity.h>
#include <string.h>
#include "logic/pull_scheduler.h"

static const uint32_t MIN_MS = 60000u;

void setUp(void) {}
void tearDown(void) {}

void test_backoff_growth_and_cap(void) {
    const uint32_t iv = 5 * MIN_MS;
    TEST_ASSERT_EQUAL_UINT32(iv,     pull_backoff_ms(iv, 0));
    TEST_ASSERT_EQUAL_UINT32(iv,     pull_backoff_ms(iv, 1));   // first miss: retry after one interval
    TEST_ASSERT_EQUAL_UINT32(2 * iv, pull_backoff_ms(iv, 2));
    TEST_ASSERT_EQUAL_UINT32(4 * iv, pull_backoff_ms(iv, 3));
    TEST_ASSERT_EQUAL_UINT32(8 * iv, pull_backoff_ms(iv, 4));
    TEST_ASSERT_EQUAL_UINT32(8 * iv, pull_backoff_ms(iv, 5));   // capped at 8x
    TEST_ASSERT_EQUAL_UINT32(8 * iv, pull_backoff_ms(iv, 255));
    // the longest interval (24 h) x 8 still fits 32 bits
    TEST_ASSERT_EQUAL_UINT32(8u * 1440u * MIN_MS, pull_backoff_ms(1440u * MIN_MS, 200));
}

void test_jitter_bounds(void) {
    const uint32_t d = 5 * MIN_MS;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    uint32_t rnd = 12345;
    for (int i = 0; i < 5000; i++) {
        rnd = rnd * 1664525u + 1013904223u;
        uint32_t j = pull_jitter_ms(d, rnd >> 8);
        if (j < lo) lo = j;
        if (j > hi) hi = j;
        TEST_ASSERT_TRUE(j >= d - d / 10 && j <= d + d / 10);
    }
    TEST_ASSERT_TRUE(lo < d - d / 20);   // both halves of the window are used
    TEST_ASSERT_TRUE(hi > d + d / 20);
    TEST_ASSERT_EQUAL_UINT32(d - d / 10, pull_jitter_ms(d, 0));          // rnd 0 -> -10 %
    TEST_ASSERT_EQUAL_UINT32(d, pull_jitter_ms(d, 1000));                // centre -> exact
    TEST_ASSERT_EQUAL_UINT32(d + d / 10, pull_jitter_ms(d, 2000));       // -> +10 %
    // the largest delay (24 h x 8) with the largest offset does not overflow
    uint32_t big = 8u * 1440u * MIN_MS;
    TEST_ASSERT_EQUAL_UINT32(big + big / 10, pull_jitter_ms(big, 2000));
}

void test_first_fetch_after_start(void) {
    PullScheduler s;
    pull_sched_start(&s, 1000, 5, true, 42);
    TEST_ASSERT_FALSE(pull_sched_due(&s, 1000));
    TEST_ASSERT_FALSE(pull_sched_due(&s, 1000 + PULL_FIRST_DELAY_MS - 1));
    TEST_ASSERT_TRUE(pull_sched_due(&s, 1000 + PULL_FIRST_DELAY_MS));
    TEST_ASSERT_EQUAL_UINT32(5 * MIN_MS, s.interval_ms);
    // no URL: never due
    pull_sched_start(&s, 1000, 5, false, 42);
    TEST_ASSERT_FALSE(pull_sched_due(&s, 1000 + 10 * MIN_MS));
}

void test_success_schedules_interval_with_jitter(void) {
    PullScheduler s;
    pull_sched_start(&s, 0, 5, true, 7);
    uint32_t t = PULL_FIRST_DELAY_MS;
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_TRUE(pull_sched_due(&s, t));
        pull_sched_done(&s, t, true);
        uint32_t wait = s.next_ms - t;
        TEST_ASSERT_TRUE(wait >= 5 * MIN_MS - 5 * MIN_MS / 10 && wait <= 5 * MIN_MS + 5 * MIN_MS / 10);
        TEST_ASSERT_FALSE(pull_sched_due(&s, s.next_ms - 1));
        TEST_ASSERT_EQUAL_UINT8(0, s.fails);
        t = s.next_ms;
    }
}

void test_failures_back_off_and_reset(void) {
    PullScheduler s;
    pull_sched_start(&s, 0, 10, true, 3);
    const uint32_t iv = 10 * MIN_MS;
    uint32_t t = PULL_FIRST_DELAY_MS;
    static const uint32_t expect_x[] = { 1, 2, 4, 8, 8, 8 };
    for (int i = 0; i < 6; i++) {
        pull_sched_done(&s, t, false);
        TEST_ASSERT_EQUAL_UINT8(i + 1, s.fails);
        uint32_t wait = s.next_ms - t, base = expect_x[i] * iv;
        TEST_ASSERT_TRUE(wait >= base - base / 10 && wait <= base + base / 10);
        t = s.next_ms;
    }
    // one success resets the back-off to the plain interval
    pull_sched_done(&s, t, true);
    TEST_ASSERT_EQUAL_UINT8(0, s.fails);
    uint32_t wait = s.next_ms - t;
    TEST_ASSERT_TRUE(wait >= iv - iv / 10 && wait <= iv + iv / 10);
}

void test_config_change_fetches_now(void) {
    PullScheduler s;
    pull_sched_start(&s, 0, 60, true, 1);
    uint32_t t = PULL_FIRST_DELAY_MS;
    pull_sched_done(&s, t, false);
    pull_sched_done(&s, t, false);
    pull_sched_done(&s, t, false);            // 4x back-off pending
    TEST_ASSERT_FALSE(pull_sched_due(&s, t + 60 * MIN_MS));
    pull_sched_config(&s, t + 1000, 15, true);
    TEST_ASSERT_TRUE(pull_sched_due(&s, t + 1000));     // at once
    TEST_ASSERT_EQUAL_UINT8(0, s.fails);                // failures forgotten
    TEST_ASSERT_EQUAL_UINT32(15 * MIN_MS, s.interval_ms);
    pull_sched_done(&s, t + 1000, true);
    uint32_t wait = s.next_ms - (t + 1000);
    TEST_ASSERT_TRUE(wait >= 15 * MIN_MS - 15 * MIN_MS / 10 && wait <= 15 * MIN_MS + 15 * MIN_MS / 10);
    // URL removed: nothing is due any more; added again: at once
    pull_sched_config(&s, t + 2000, 15, false);
    TEST_ASSERT_FALSE(pull_sched_due(&s, t + 2000 + 100 * MIN_MS));
    pull_sched_config(&s, t + 3000, 15, true);
    TEST_ASSERT_TRUE(pull_sched_due(&s, t + 3000));
}

void test_millis_wraparound(void) {
    PullScheduler s;
    uint32_t t = 0xFFFFFFFFu - 1000;
    pull_sched_start(&s, t, 1, true, 5);
    TEST_ASSERT_FALSE(pull_sched_due(&s, t + 1000));                // before the wrap, not yet due
    TEST_ASSERT_TRUE(pull_sched_due(&s, t + PULL_FIRST_DELAY_MS));  // wrapped past zero, due
    pull_sched_done(&s, t + PULL_FIRST_DELAY_MS, true);
    TEST_ASSERT_FALSE(pull_sched_due(&s, t + PULL_FIRST_DELAY_MS + 1000));
    TEST_ASSERT_TRUE(pull_sched_due(&s, t + PULL_FIRST_DELAY_MS + MIN_MS + MIN_MS / 10));
}

void test_defer_keeps_failures_and_interval(void) {
    PullScheduler s;
    pull_sched_start(&s, 0, 5, true, 9);
    uint32_t t = PULL_FIRST_DELAY_MS;
    pull_sched_done(&s, t, false);
    pull_sched_done(&s, t, false);            // fails = 2, next ~ 2x interval
    t = s.next_ms;
    TEST_ASSERT_TRUE(pull_sched_due(&s, t));
    pull_sched_defer(&s, t, PULL_DEFER_NO_LINK_MS);
    TEST_ASSERT_FALSE(pull_sched_due(&s, t + PULL_DEFER_NO_LINK_MS - 1));
    TEST_ASSERT_TRUE(pull_sched_due(&s, t + PULL_DEFER_NO_LINK_MS));
    TEST_ASSERT_EQUAL_UINT8(2, s.fails);      // a deferral is not a miss
    TEST_ASSERT_EQUAL_UINT32(5 * MIN_MS, s.interval_ms);
    pull_sched_done(&s, t + PULL_DEFER_NO_LINK_MS, false);
    TEST_ASSERT_EQUAL_UINT8(3, s.fails);      // the back-off continues where it was
}

void test_interval_sanitised(void) {
    PullScheduler s;
    pull_sched_start(&s, 0, 0, true, 5);
    TEST_ASSERT_EQUAL_UINT32(60 * MIN_MS, s.interval_ms);       // 0 = unknown -> the 60-min default
    pull_sched_config(&s, 0, 100000, true);
    TEST_ASSERT_EQUAL_UINT32(1440 * MIN_MS, s.interval_ms);     // clamped to 24 h
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_backoff_growth_and_cap);
    RUN_TEST(test_jitter_bounds);
    RUN_TEST(test_first_fetch_after_start);
    RUN_TEST(test_success_schedules_interval_with_jitter);
    RUN_TEST(test_failures_back_off_and_reset);
    RUN_TEST(test_config_change_fetches_now);
    RUN_TEST(test_millis_wraparound);
    RUN_TEST(test_defer_keeps_failures_and_interval);
    RUN_TEST(test_interval_sanitised);
    return UNITY_END();
}
