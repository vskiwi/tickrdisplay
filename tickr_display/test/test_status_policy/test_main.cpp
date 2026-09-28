// Unity tests for the badge refresh policy (src/logic/status_policy.cpp)
// and the screen-state names (src/logic/ui_strings.cpp) - docs/DEVICE_UI.md
// "E-ink refresh rules" and "Screens" (badges).
#include <unity.h>
#include <string.h>
#include "logic/status_policy.h"
#include "logic/ui_strings.h"

static DisplayStatus usb_online() {
    DisplayStatus s;
    s.wifi_connected = true;
    s.wifi_bars = 3;
    s.usb = true;
    s.power = BADGE_POWER_USB;
    s.batt_known = true;
    s.batt_pct = 100;
    strcpy(s.ip, "192.168.1.40");
    return s;
}

// --- badge_change ----------------------------------------------------------

void test_change_same_and_bars_ignored() {
    DisplayStatus a = usb_online(), b = a;
    TEST_ASSERT_EQUAL_UINT8(BADGE_SAME, badge_change(a, b));
    b.wifi_bars = 1;                       // RSSI jitter: a bench RSSI storm must be invisible
    TEST_ASSERT_EQUAL_UINT8(BADGE_SAME, badge_change(a, b));
}

void test_change_classification() {
    DisplayStatus a = usb_online(), b = a;
    b.batt_pct = 95;
    TEST_ASSERT_EQUAL_UINT8(BADGE_COSMETIC, badge_change(a, b));
    b = a; strcpy(b.ip, "192.168.1.41");
    TEST_ASSERT_EQUAL_UINT8(BADGE_COSMETIC, badge_change(a, b));
    b = a; b.wifi_connected = false; b.ip[0] = '\0';
    TEST_ASSERT_EQUAL_UINT8(BADGE_WIFI | BADGE_COSMETIC, badge_change(a, b));
    b = a; b.usb = false; b.power = BADGE_POWER_BATTERY; b.batt_pct = 80;
    TEST_ASSERT_EQUAL_UINT8(BADGE_POWER | BADGE_COSMETIC, badge_change(a, b));
    b = a; b.power = BADGE_POWER_UNKNOWN;  // detector lost its information: the glyph goes
    TEST_ASSERT_EQUAL_UINT8(BADGE_POWER, badge_change(a, b));
    b = a; b.batt_known = false;
    TEST_ASSERT_EQUAL_UINT8(BADGE_POWER, badge_change(a, b));
}

// --- badge_poll ------------------------------------------------------------

void test_cosmetic_is_adopted_but_never_refreshes() {
    BadgePolicy p;
    // long after the last refresh, a battery step alone still costs nothing
    TEST_ASSERT_EQUAL_UINT8(BADGE_ADOPT, badge_poll(&p, BADGE_COSMETIC, 3600000u, false));
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, 3600001u, false));
    TEST_ASSERT_FALSE(p.due);
}

void test_power_change_debounced_then_one_refresh() {
    BadgePolicy p;
    for (unsigned i = 1; i < BADGE_POWER_DEBOUNCE_POLLS; i++) {
        TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_POWER, 600000u, false));
        TEST_ASSERT_EQUAL_UINT8(i, p.count);
    }
    // 5th consecutive poll: adopted and, with the minute long gone, refreshed at once
    TEST_ASSERT_EQUAL_UINT8(BADGE_REFRESH, badge_poll(&p, BADGE_POWER, 600000u, false));
    TEST_ASSERT_FALSE(p.due);
    TEST_ASSERT_EQUAL_UINT8(0, p.count);
}

void test_adapter_dip_shorter_than_debounce_draws_nothing() {
    BadgePolicy p;
    for (unsigned i = 1; i < BADGE_POWER_DEBOUNCE_POLLS; i++) {
        TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_POWER, 600000u, false));
    }
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, 600000u, false));   // back to normal
    TEST_ASSERT_EQUAL_UINT8(0, p.count);
    TEST_ASSERT_FALSE(p.due);
}

void test_wifi_uses_the_long_debounce() {
    BadgePolicy p;
    for (unsigned i = 1; i < BADGE_WIFI_DEBOUNCE_POLLS; i++) {
        TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_WIFI | BADGE_COSMETIC, 600000u, false));
    }
    TEST_ASSERT_EQUAL_UINT8(BADGE_REFRESH, badge_poll(&p, BADGE_WIFI | BADGE_COSMETIC, 600000u, false));
    // a router that flaps for less than a minute never reaches the panel
    BadgePolicy q;
    for (unsigned i = 0; i < 40; i++) TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&q, BADGE_WIFI, 600000u, false));
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&q, BADGE_SAME, 600000u, false));
    TEST_ASSERT_EQUAL_UINT8(0, q.count);
}

void test_state_change_soon_after_a_refresh_waits_for_the_minute() {
    BadgePolicy p;
    // the content refreshed 10 s ago: the change is adopted (folded) but not drawn yet
    for (unsigned i = 1; i < BADGE_POWER_DEBOUNCE_POLLS; i++) badge_poll(&p, BADGE_POWER, 10000u + i * 1000u, false);
    TEST_ASSERT_EQUAL_UINT8(BADGE_ADOPT, badge_poll(&p, BADGE_POWER, 15000u, false));
    TEST_ASSERT_TRUE(p.due);
    // once adopted, the snapshot equals the shown one -> SAME; still waiting
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, BADGE_REFRESH_MIN_MS - 1, false));
    TEST_ASSERT_TRUE(p.due);
    // the minute is up: exactly one refresh
    TEST_ASSERT_EQUAL_UINT8(BADGE_REFRESH, badge_poll(&p, BADGE_SAME, BADGE_REFRESH_MIN_MS, false));
    TEST_ASSERT_FALSE(p.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, BADGE_REFRESH_MIN_MS + 1000, true));
}

void test_a_content_refresh_meanwhile_cancels_the_pending_refresh() {
    BadgePolicy p;
    for (unsigned i = 1; i < BADGE_POWER_DEBOUNCE_POLLS; i++) badge_poll(&p, BADGE_POWER, 5000u, false);
    TEST_ASSERT_EQUAL_UINT8(BADGE_ADOPT, badge_poll(&p, BADGE_POWER, 5000u, false));
    TEST_ASSERT_TRUE(p.due);
    // a payload arrived and was drawn with the stored snapshot: nothing owed
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, 500u, true));
    TEST_ASSERT_FALSE(p.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, 120000u, false));
}

void test_pending_state_change_restarts_when_its_kind_changes() {
    BadgePolicy p;
    badge_poll(&p, BADGE_POWER, 600000u, false);
    badge_poll(&p, BADGE_POWER, 600000u, false);
    TEST_ASSERT_EQUAL_UINT8(2, p.count);
    badge_poll(&p, BADGE_POWER | BADGE_WIFI, 600000u, false);   // Wi-Fi joined the change: count anew, long debounce
    TEST_ASSERT_EQUAL_UINT8(1, p.count);
    TEST_ASSERT_EQUAL_UINT8(BADGE_POWER | BADGE_WIFI, p.pending);
}

void test_cosmetic_while_debouncing_does_not_leak_through() {
    BadgePolicy p;
    // a power flip in progress plus a battery step: the whole change waits for the debounce
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_POWER | BADGE_COSMETIC, 600000u, false));
    TEST_ASSERT_EQUAL_UINT8(1, p.count);
}

// The ticker's stale crossing: one refresh owed, no debounce,
// under the same minute rule as a state change.
void test_stale_crossing_is_one_refresh_under_the_minute_rule() {
    BadgePolicy p;
    // idle for long: the crossing refreshes at once
    TEST_ASSERT_EQUAL_UINT8(BADGE_REFRESH, badge_poll(&p, BADGE_STALE, 600000u, false));
    TEST_ASSERT_FALSE(p.due);
    // the age keeps growing, but the crossing is reported only once by the caller: nothing more
    for (unsigned t = 0; t < 180; t++) TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, BADGE_SAME, 1000u + t * 1000u, t == 0));
    TEST_ASSERT_FALSE(p.due);
    // soon after a refresh: owed (nothing to adopt - the snapshot is the same), drawn when the minute is up
    BadgePolicy q;
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&q, BADGE_STALE, 20000u, false));
    TEST_ASSERT_TRUE(q.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&q, BADGE_SAME, BADGE_REFRESH_MIN_MS - 1000u, false));
    TEST_ASSERT_EQUAL_UINT8(BADGE_REFRESH, badge_poll(&q, BADGE_SAME, BADGE_REFRESH_MIN_MS, false));
    TEST_ASSERT_FALSE(q.due);
    // a content refresh meanwhile drew the marker itself: the debt is settled
    BadgePolicy r;
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&r, BADGE_STALE, 20000u, false));
    TEST_ASSERT_TRUE(r.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&r, BADGE_SAME, 500u, true));
    TEST_ASSERT_FALSE(r.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&r, BADGE_SAME, 120000u, false));
    // a power flip being debounced is not bypassed by the crossing: the owed
    // refresh waits for the debounce and both go out in the one refresh
    BadgePolicy s;
    badge_poll(&s, BADGE_POWER, 600000u, false);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&s, BADGE_POWER | BADGE_STALE, 600000u, false));
    TEST_ASSERT_TRUE(s.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_POWER, s.pending);
    TEST_ASSERT_EQUAL_UINT8(2, s.count);
    for (unsigned i = 3; i < BADGE_POWER_DEBOUNCE_POLLS; i++) TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&s, BADGE_POWER, 600000u, false));
    TEST_ASSERT_EQUAL_UINT8(BADGE_REFRESH, badge_poll(&s, BADGE_POWER, 600000u, false));
    TEST_ASSERT_FALSE(s.due);
    TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&s, BADGE_SAME, 1000u, true));
}

// Storm regression, end to end: bars flipping every poll for 20 minutes
// (the shape of the bench trace) produce no action at all.
void test_bars_storm_is_invisible() {
    BadgePolicy p;
    DisplayStatus shown = usb_online();
    for (unsigned t = 0; t < 1200; t++) {
        DisplayStatus cur = shown;
        cur.wifi_bars = (int8_t)(1 + (t & 1));
        TEST_ASSERT_EQUAL_UINT8(BADGE_KEEP, badge_poll(&p, badge_change(shown, cur), t * 1000u, false));
    }
    TEST_ASSERT_FALSE(p.due);
}

// --- screen_state_str --------------------------------------------------------

void test_screen_state_names_match_the_api_contract() {
    TEST_ASSERT_EQUAL_STRING("boot", screen_state_str(SCREEN_BOOT));
    TEST_ASSERT_EQUAL_STRING("setup", screen_state_str(SCREEN_SETUP));
    TEST_ASSERT_EQUAL_STRING("waiting", screen_state_str(SCREEN_WAITING));
    TEST_ASSERT_EQUAL_STRING("content", screen_state_str(SCREEN_CONTENT));
    TEST_ASSERT_EQUAL_STRING("pairing", screen_state_str(SCREEN_PAIRING));
    TEST_ASSERT_EQUAL_STRING("recovery", screen_state_str(SCREEN_RECOVERY));
    TEST_ASSERT_EQUAL_STRING("ota", screen_state_str(SCREEN_OTA));
    TEST_ASSERT_EQUAL_STRING("identify", screen_state_str(SCREEN_IDENTIFY));
    TEST_ASSERT_EQUAL_STRING("unknown", screen_state_str(SCREEN_STATE_COUNT));
    // Later frames (additive: the names above keep their values)
    TEST_ASSERT_EQUAL_STRING("offline", screen_state_str(SCREEN_OFFLINE));
    TEST_ASSERT_EQUAL_STRING("battery_empty", screen_state_str(SCREEN_BATTERY_EMPTY));
    TEST_ASSERT_EQUAL_STRING("power_to_battery", screen_state_str(SCREEN_POWER_BATTERY));
    // every name is unique and lower-case snake_case ASCII (a JSON enum the web keys on)
    for (int i = 0; i < SCREEN_STATE_COUNT; i++) {
        const char* a = screen_state_str((ScreenState)i);
        for (const char* c = a; *c; c++) TEST_ASSERT_TRUE((*c >= 'a' && *c <= 'z') || *c == '_');
        for (int j = i + 1; j < SCREEN_STATE_COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, screen_state_str((ScreenState)j)) != 0);
        }
    }
}

void setUp() {}
void tearDown() {}

int runUnityTests() {
    UNITY_BEGIN();
    RUN_TEST(test_change_same_and_bars_ignored);
    RUN_TEST(test_change_classification);
    RUN_TEST(test_cosmetic_is_adopted_but_never_refreshes);
    RUN_TEST(test_power_change_debounced_then_one_refresh);
    RUN_TEST(test_adapter_dip_shorter_than_debounce_draws_nothing);
    RUN_TEST(test_wifi_uses_the_long_debounce);
    RUN_TEST(test_state_change_soon_after_a_refresh_waits_for_the_minute);
    RUN_TEST(test_a_content_refresh_meanwhile_cancels_the_pending_refresh);
    RUN_TEST(test_pending_state_change_restarts_when_its_kind_changes);
    RUN_TEST(test_cosmetic_while_debouncing_does_not_leak_through);
    RUN_TEST(test_stale_crossing_is_one_refresh_under_the_minute_rule);
    RUN_TEST(test_bars_storm_is_invisible);
    RUN_TEST(test_screen_state_names_match_the_api_contract);
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
