// Host tests for the power-cycle recovery counter (src/logic/recovery_counter.cpp).
#include <unity.h>
#include "logic/recovery_counter.h"

void setUp() {}
void tearDown() {}

// Fake NVS: one byte plus a write counter.
struct FakeStore { uint8_t value; unsigned writes; };
static FakeStore S;
static uint8_t fk_load(void* ctx) { return ((FakeStore*)ctx)->value; }
static void fk_save(void* ctx, uint8_t n) { ((FakeStore*)ctx)->value = n; ((FakeStore*)ctx)->writes++; }
static const RecoveryStore STORE = { &S, fk_load, fk_save };

static void reset_store(uint8_t v = 0) { S.value = v; S.writes = 0; }

// One boot: returns the recovery decision; `uptime_before_off` = how long the
// device ran before the next power cycle (ticks are fed up to that point).
static bool boot(RecoveryCounter* c, bool cold, uint32_t uptime_before_off) {
    bool rec = recovery_counter_boot(c, &STORE, cold);
    for (uint32_t t = 0; t <= uptime_before_off; t += 1000) recovery_counter_tick(c, t);
    return rec;
}

static void test_reset_kinds() {
    TEST_ASSERT_TRUE(recovery_reset_is_cold(RECOVERY_RST_POWERON));
    TEST_ASSERT_TRUE(recovery_reset_is_cold(RECOVERY_RST_EXT));
    TEST_ASSERT_TRUE(recovery_reset_is_cold(RECOVERY_RST_BROWNOUT));
    TEST_ASSERT_FALSE(recovery_reset_is_cold(RECOVERY_RST_SW));
    TEST_ASSERT_FALSE(recovery_reset_is_cold(RECOVERY_RST_DEEPSLEEP));
    TEST_ASSERT_FALSE(recovery_reset_is_cold(RECOVERY_RST_CRASH));
    TEST_ASSERT_FALSE(recovery_reset_is_cold(RECOVERY_RST_OTHER));
}

static void test_three_quick_power_cycles_enter_recovery() {
    reset_store();
    RecoveryCounter c;
    TEST_ASSERT_FALSE(boot(&c, true, 5000));      // 1st start, off after 5 s
    TEST_ASSERT_EQUAL_UINT8(1, c.position);
    TEST_ASSERT_EQUAL_UINT8(1, S.value);
    TEST_ASSERT_FALSE(boot(&c, true, 3000));      // 2nd start
    TEST_ASSERT_EQUAL_UINT8(2, c.position);
    TEST_ASSERT_EQUAL_UINT8(2, S.value);
    TEST_ASSERT_TRUE(boot(&c, true, 0));          // 3rd start -> recovery
    TEST_ASSERT_TRUE(c.recovery);
    TEST_ASSERT_EQUAL_UINT8(3, c.position);
    TEST_ASSERT_EQUAL_UINT8(0, S.value);          // series consumed
    TEST_ASSERT_FALSE(c.armed);                   // nothing left to do at 20 s
}

static void test_window_closes_the_series() {
    reset_store();
    RecoveryCounter c;
    TEST_ASSERT_FALSE(boot(&c, true, 19000));     // 19 s: still armed
    TEST_ASSERT_EQUAL_UINT8(1, S.value);
    TEST_ASSERT_TRUE(c.armed);
    TEST_ASSERT_TRUE(recovery_counter_tick(&c, 20000));   // 20 s: window closes, one save(0)
    TEST_ASSERT_EQUAL_UINT8(0, S.value);
    TEST_ASSERT_FALSE(recovery_counter_tick(&c, 25000));  // idempotent
    TEST_ASSERT_EQUAL_UINT(2, S.writes);          // save(1) + save(0), nothing more
    TEST_ASSERT_FALSE(boot(&c, true, 0));         // next power-on starts over
    TEST_ASSERT_EQUAL_UINT8(1, c.position);
}

static void test_slow_cycles_never_enter_recovery() {
    reset_store();
    RecoveryCounter c;
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(boot(&c, true, 30000));
        TEST_ASSERT_EQUAL_UINT8(1, c.position);
        TEST_ASSERT_EQUAL_UINT8(0, S.value);
    }
}

static void test_software_restart_does_not_count() {
    reset_store();
    RecoveryCounter c;
    // OTA: three software restarts in a row, however quick.
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_FALSE(boot(&c, false, 1000));
        TEST_ASSERT_EQUAL_UINT8(0, c.position);
        TEST_ASSERT_EQUAL_UINT8(0, S.value);
    }
    TEST_ASSERT_EQUAL_UINT(0, S.writes);          // an untouched store is not rewritten
}

static void test_deep_sleep_wakeups_do_not_count() {
    reset_store();
    RecoveryCounter c;
    TEST_ASSERT_FALSE(boot(&c, true, 2000));      // battery: switched on, sleeps after 2 s
    TEST_ASSERT_EQUAL_UINT8(1, S.value);          // the window never closed before sleep
    // The first wake-up (>= 1 min later) is not counted and clears the leftover...
    TEST_ASSERT_FALSE(boot(&c, false, 3000));
    TEST_ASSERT_EQUAL_UINT8(0, c.position);
    TEST_ASSERT_EQUAL_UINT8(0, S.value);
    unsigned writes = S.writes;
    // ...further wake-ups touch nothing...
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_FALSE(boot(&c, false, 3000));
        TEST_ASSERT_EQUAL_UINT8(0, c.position);
    }
    TEST_ASSERT_EQUAL_UINT(writes, S.writes);
    // ...and a later single power-on is the start of a fresh series, not "2 of 3".
    TEST_ASSERT_FALSE(boot(&c, true, 2000));
    TEST_ASSERT_EQUAL_UINT8(1, c.position);
}

static void test_battery_series_still_reaches_threshold() {
    // Three quick toggles on battery (the device sleeps after each pull): counted.
    reset_store();
    RecoveryCounter c;
    TEST_ASSERT_FALSE(boot(&c, true, 3000));
    TEST_ASSERT_FALSE(boot(&c, true, 3000));
    TEST_ASSERT_EQUAL_UINT8(2, c.position);
    TEST_ASSERT_TRUE(boot(&c, true, 0));          // the caller shows "USB only" on battery
    TEST_ASSERT_EQUAL_UINT8(0, S.value);
}

static void test_non_cold_boot_clears_a_stale_count() {
    reset_store();
    RecoveryCounter c;
    TEST_ASSERT_FALSE(boot(&c, true, 1000));      // count 1 left behind
    TEST_ASSERT_EQUAL_UINT8(1, S.value);
    TEST_ASSERT_FALSE(recovery_counter_boot(&c, &STORE, false));   // e.g. a crash reset
    TEST_ASSERT_FALSE(c.armed);
    TEST_ASSERT_EQUAL_UINT8(0, S.value);          // cleared at once
    TEST_ASSERT_FALSE(recovery_counter_tick(&c, RECOVERY_WINDOW_MS));   // nothing left to do
}

static void test_crash_loop_never_enters_recovery() {
    reset_store();
    RecoveryCounter c;
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_FALSE(boot(&c, false, 500));  // panic every 0.5 s
    }
    TEST_ASSERT_EQUAL_UINT8(0, S.value);
}

static void test_corrupt_store_value_is_consumed() {
    reset_store(200);                             // garbage / leftover from an older build
    RecoveryCounter c;
    TEST_ASSERT_TRUE(boot(&c, true, 0));          // >= threshold -> recovery once
    TEST_ASSERT_EQUAL_UINT8(0, S.value);
    TEST_ASSERT_FALSE(boot(&c, true, 0));         // and then a normal series
    TEST_ASSERT_EQUAL_UINT8(1, c.position);
}

static void test_position_reports_series_index() {
    reset_store();
    RecoveryCounter c;
    boot(&c, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, c.position);
    boot(&c, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(2, c.position);
    TEST_ASSERT_FALSE(c.recovery);
    recovery_counter_boot(&c, &STORE, false);
    TEST_ASSERT_EQUAL_UINT8(0, c.position);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_reset_kinds);
    RUN_TEST(test_three_quick_power_cycles_enter_recovery);
    RUN_TEST(test_window_closes_the_series);
    RUN_TEST(test_slow_cycles_never_enter_recovery);
    RUN_TEST(test_software_restart_does_not_count);
    RUN_TEST(test_deep_sleep_wakeups_do_not_count);
    RUN_TEST(test_battery_series_still_reaches_threshold);
    RUN_TEST(test_non_cold_boot_clears_a_stale_count);
    RUN_TEST(test_crash_loop_never_enters_recovery);
    RUN_TEST(test_corrupt_store_value_is_consumed);
    RUN_TEST(test_position_reports_series_index);
    return UNITY_END();
}
