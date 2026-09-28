// Unity tests for refresh policy v2 - partial-first (src/logic/refresh_policy.cpp),
// docs/DEVICE_UI.md "E-ink refresh rules": the kind decision table per event class, the forced-
// full counters (N = 8 / 60 min on USB, 6 wakes on battery, 24 h hygiene), the
// 30 s spacing, the identical-frame skip, the invalid-copy gate and the
// double-full constant.
#include <unity.h>
#include <string.h>
#include "logic/refresh_policy.h"

// A USB content frame right after a full: the copy is valid, nothing changed
// in layout, plenty of time since the last refresh.
static RefreshInputs usb(uint8_t ev = RP_EV_CONTENT) {
    RefreshInputs in;
    in.event = ev;
    in.prev_valid = true;
    in.layout_changed = false;
    in.frame_identical = false;
    in.battery = false;
    in.partials_since_full = 0;
    in.ms_since_full = 120000;
    in.ms_since_partial = 120000;
    return in;
}

static RefreshInputs batt(uint8_t ev = RP_EV_CONTENT) {
    RefreshInputs in = usb(ev);
    in.battery = true;
    in.ms_since_full = 0;        // millis() restarts on every wake
    in.ms_since_partial = 0;
    return in;
}

// --- the invalid-copy gate ---------------------------------------------------

void test_invalid_previous_is_always_full() {
    for (uint8_t ev = 0; ev < RP_EV_COUNT; ev++) {
        RefreshInputs in = usb(ev);
        in.prev_valid = false;
        TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
        in.frame_identical = true;   // "identical" to an untrusted copy means nothing
        TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
        in = batt(ev);
        in.prev_valid = false;       // battery wake after a power cycle / CRC miss
        TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
    }
}

// --- the decision table --------------------------------------------------------

void test_content_same_layout_is_partial_and_layout_change_is_full() {
    RefreshInputs in = usb(RP_EV_CONTENT);
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    in.layout_changed = true;    // text <-> ticker <-> WAITING, or another ticker source
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
}

void test_badges_stale_and_service_frames_are_partial() {
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(usb(RP_EV_BADGE)));     // badge change / stale crossing
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(usb(RP_EV_SERVICE)));   // identify, pairing, ON-BATTERY card
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(usb(RP_EV_RESTORE)));   // the base frame back
}

void test_condition_cards_boot_frames_are_full() {
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(usb(RP_EV_CONDITION)));    // OFFLINE / OTA / EMPTY
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(usb(RP_EV_BOOT)));         // splash, SETUP, RECOVERY
    RefreshInputs in = batt(RP_EV_CONDITION);                                 // battery OFFLINE card (fail 2)
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
}

void test_restore_after_a_condition_card_is_partial_when_counters_allow() {
    // The OFFLINE card was drawn full; its removal is a
    // small outline change, so the content comes back partial ...
    RefreshInputs in = usb(RP_EV_RESTORE);
    in.partials_since_full = 3;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    // ... unless the forced-full counters say otherwise.
    in.partials_since_full = RP_FORCE_FULL_EVERY;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
    in = usb(RP_EV_RESTORE);
    in.ms_since_full = RP_FULL_MAX_AGE_MS;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
}

// --- identical frame -----------------------------------------------------------

void test_identical_frame_is_none_except_for_hygiene() {
    RefreshInputs in = usb(RP_EV_CONTENT);
    in.frame_identical = true;   // the ticker fetched the same price, the sparkline did not move
    TEST_ASSERT_EQUAL(REFRESH_NONE, refresh_decide(in));
    in.event = RP_EV_BADGE;
    TEST_ASSERT_EQUAL(REFRESH_NONE, refresh_decide(in));
    in.event = RP_EV_RESTORE;
    TEST_ASSERT_EQUAL(REFRESH_NONE, refresh_decide(in));
    in.event = RP_EV_CONDITION;  // the same card again: nothing to draw
    TEST_ASSERT_EQUAL(REFRESH_NONE, refresh_decide(in));
    // Identical and a full due: still nothing - the panel keeps a valid
    // image and the next real change pays the full.
    in.event = RP_EV_CONTENT;
    in.partials_since_full = RP_FORCE_FULL_EVERY;
    TEST_ASSERT_EQUAL(REFRESH_NONE, refresh_decide(in));
    // The 24 h hygiene refresh is about exercising the panel: never skipped.
    in = usb(RP_EV_HYGIENE);
    in.frame_identical = true;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
}

// --- forced full: N = 8, 60 min, 24 h floor -----------------------------------

void test_every_eighth_partial_is_a_full() {
    RefreshInputs in = usb(RP_EV_CONTENT);
    in.partials_since_full = RP_FORCE_FULL_EVERY - 1;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    in.partials_since_full = RP_FORCE_FULL_EVERY;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
    in.event = RP_EV_SERVICE;    // service frames count too
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
}

void test_sixty_minutes_since_the_full_forces_one_on_usb() {
    RefreshInputs in = usb(RP_EV_CONTENT);
    in.ms_since_full = RP_FULL_MAX_AGE_MS - 1;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    in.ms_since_full = RP_FULL_MAX_AGE_MS;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
    // the 24 h floor (FULL_REFRESH_MAX_AGE_MS in hal_display) arrives as its own event
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(usb(RP_EV_HYGIENE)));
}

void test_counter_bookkeeping() {
    TEST_ASSERT_EQUAL_UINT8(1, refresh_count_after(0, REFRESH_PARTIAL));
    TEST_ASSERT_EQUAL_UINT8(8, refresh_count_after(7, REFRESH_PARTIAL));
    TEST_ASSERT_EQUAL_UINT8(0, refresh_count_after(7, REFRESH_FULL));
    TEST_ASSERT_EQUAL_UINT8(5, refresh_count_after(5, REFRESH_NONE));
    TEST_ASSERT_EQUAL_UINT8(5, refresh_count_after(5, REFRESH_DEFER));
    TEST_ASSERT_EQUAL_UINT8(255, refresh_count_after(255, REFRESH_PARTIAL));   // saturates
}

void test_one_minute_ticker_flashes_every_eighth_frame() {
    // 24 content frames, each with a new price: 8 partials, then a full
    // (frames 9 and 18) - one inversion flash per 9 min at a 1-min ticker.
    uint8_t n = 0;
    int fulls = 0, partials = 0;
    for (int i = 0; i < 24; i++) {
        RefreshInputs in = usb(RP_EV_CONTENT);
        in.partials_since_full = n;
        in.ms_since_full = 60000u * (uint32_t)(n + 1);
        in.ms_since_partial = 60000;
        RefreshKind k = refresh_decide(in);
        if (k == REFRESH_FULL) fulls++;
        if (k == REFRESH_PARTIAL) partials++;
        n = refresh_count_after(n, k);
    }
    TEST_ASSERT_EQUAL_INT(2, fulls);
    TEST_ASSERT_EQUAL_INT(22, partials);
}

// --- battery: every 6th wake, no clock ------------------------------------------

void test_battery_wake_partial_until_the_sixth() {
    RefreshInputs in = batt(RP_EV_CONTENT);
    for (uint8_t w = 0; w < RP_FORCE_FULL_BATT; w++) {
        in.partials_since_full = w;
        TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    }
    in.partials_since_full = RP_FORCE_FULL_BATT;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
    // the USB clock rules do not apply: millis() is fresh on every wake
    in = batt(RP_EV_CONTENT);
    in.partials_since_full = RP_FORCE_FULL_BATT - 1;
    in.ms_since_full = 0;
    in.ms_since_partial = 0;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
}

void test_battery_ignores_the_usb_eight_and_the_hour() {
    RefreshInputs in = batt(RP_EV_CONTENT);
    in.partials_since_full = RP_FORCE_FULL_BATT - 1;   // 5 < 8 on USB, but the battery rule is 6
    in.ms_since_full = RP_FULL_MAX_AGE_MS * 2;         // meaningless across sleeps
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
}

// --- spacing -------------------------------------------------------------------------

void test_content_partials_are_thirty_seconds_apart() {
    RefreshInputs in = usb(RP_EV_CONTENT);
    in.ms_since_partial = RP_PARTIAL_MIN_MS - 1;
    TEST_ASSERT_EQUAL(REFRESH_DEFER, refresh_decide(in));   // drawn later, not dropped
    in.ms_since_partial = RP_PARTIAL_MIN_MS;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    in.event = RP_EV_BADGE;
    in.ms_since_partial = 1000;
    TEST_ASSERT_EQUAL(REFRESH_DEFER, refresh_decide(in));   // (status_policy's minute rule normally prevents this)
    // The spacing counts from the last PARTIAL: the pull 3 s after the boot
    // WAITING card (a full) draws the content at once.
    in.event = RP_EV_CONTENT;
    in.ms_since_partial = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
}

void test_service_frames_and_restores_ignore_the_spacing() {
    RefreshInputs in = usb(RP_EV_SERVICE);
    in.ms_since_partial = 0;                                // ON-BATTERY card right after a content frame
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    in.event = RP_EV_RESTORE;
    in.ms_since_partial = 5000;                             // and its restore 5 s later
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
    // a forced full or a layout change still wins over "partial"
    in.layout_changed = true;
    TEST_ASSERT_EQUAL(REFRESH_FULL, refresh_decide(in));
}

void test_spacing_does_not_apply_on_battery() {
    RefreshInputs in = batt(RP_EV_CONTENT);
    in.ms_since_partial = 0;
    TEST_ASSERT_EQUAL(REFRESH_PARTIAL, refresh_decide(in));
}

// --- the double-full constant ----------------------------------------------------

void test_double_full_after_a_card_only_when_enabled() {
    TEST_ASSERT_FALSE(refresh_double_full(false, true, REFRESH_FULL));     // default: off
    TEST_ASSERT_TRUE(refresh_double_full(true, true, REFRESH_FULL));       // the full that ends a card
    TEST_ASSERT_FALSE(refresh_double_full(true, false, REFRESH_FULL));     // a full over content
    TEST_ASSERT_FALSE(refresh_double_full(true, true, REFRESH_PARTIAL));   // a partial never doubles
    TEST_ASSERT_EQUAL_INT(0, RP_DOUBLE_FULL_AFTER_CARD);                   // shipped default
}

void test_kind_names() {
    TEST_ASSERT_EQUAL_STRING("none", refresh_kind_str(REFRESH_NONE));
    TEST_ASSERT_EQUAL_STRING("defer", refresh_kind_str(REFRESH_DEFER));
    TEST_ASSERT_EQUAL_STRING("partial", refresh_kind_str(REFRESH_PARTIAL));
    TEST_ASSERT_EQUAL_STRING("full", refresh_kind_str(REFRESH_FULL));
}

void test_constants_match_the_documented_rules() {
    TEST_ASSERT_EQUAL_UINT(8, RP_FORCE_FULL_EVERY);
    TEST_ASSERT_EQUAL_UINT(6, RP_FORCE_FULL_BATT);
    TEST_ASSERT_EQUAL_UINT32(60u * 60u * 1000u, RP_FULL_MAX_AGE_MS);
    TEST_ASSERT_EQUAL_UINT32(30u * 1000u, RP_PARTIAL_MIN_MS);
}

void setUp() {}
void tearDown() {}

int runUnityTests() {
    UNITY_BEGIN();
    RUN_TEST(test_invalid_previous_is_always_full);
    RUN_TEST(test_content_same_layout_is_partial_and_layout_change_is_full);
    RUN_TEST(test_badges_stale_and_service_frames_are_partial);
    RUN_TEST(test_condition_cards_boot_frames_are_full);
    RUN_TEST(test_restore_after_a_condition_card_is_partial_when_counters_allow);
    RUN_TEST(test_identical_frame_is_none_except_for_hygiene);
    RUN_TEST(test_every_eighth_partial_is_a_full);
    RUN_TEST(test_sixty_minutes_since_the_full_forces_one_on_usb);
    RUN_TEST(test_counter_bookkeeping);
    RUN_TEST(test_one_minute_ticker_flashes_every_eighth_frame);
    RUN_TEST(test_battery_wake_partial_until_the_sixth);
    RUN_TEST(test_battery_ignores_the_usb_eight_and_the_hour);
    RUN_TEST(test_content_partials_are_thirty_seconds_apart);
    RUN_TEST(test_service_frames_and_restores_ignore_the_spacing);
    RUN_TEST(test_spacing_does_not_apply_on_battery);
    RUN_TEST(test_double_full_after_a_card_only_when_enabled);
    RUN_TEST(test_kind_names);
    RUN_TEST(test_constants_match_the_documented_rules);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
    delay(2000);
    runUnityTests();
}
void loop() {}
#else
int main() { return runUnityTests(); }
#endif
