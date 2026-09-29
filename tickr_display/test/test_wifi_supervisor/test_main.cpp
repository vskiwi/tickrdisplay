// Unity tests for the Wi-Fi link supervisor (src/logic/wifi_supervisor.cpp) -
// docs/DEVICE_UI.md "Wi-Fi link supervision": grace before the first own
// attempt, 15 / 30 / 60 s back-off, the shorter grace when the core gave up,
// the DHCP grace on an association without an IP, the 30-min restart, hold,
// and the recovered flag that feeds the wifi_reconnects counter.
#include <unity.h>
#include "logic/wifi_supervisor.h"

void setUp(void) {}
void tearDown(void) {}

static WifiSupervisorAction poll(WifiSupervisor* s, uint32_t t, WifiLinkState link, bool* rec = nullptr) {
    bool r = false;
    WifiSupervisorAction a = wifi_sup_poll(s, t, link, false, &r);
    if (rec) *rec = r;
    return a;
}

// Runs the supervisor second by second from `from` to `to` (inclusive) with a
// constant link state; returns the number of RECONNECT actions and records
// the time of the last one.
static int run(WifiSupervisor* s, uint32_t from, uint32_t to, WifiLinkState link, uint32_t* last_at) {
    int n = 0;
    for (uint32_t t = from; (int32_t)(to - t) >= 0; t += 1000) {   // wrap-safe
        if (poll(s, t, link) == WIFI_SUP_RECONNECT) { n++; if (last_at) *last_at = t; }
    }
    return n;
}

void test_backoff_sequence(void) {
    TEST_ASSERT_EQUAL_UINT32(0, wifi_sup_backoff_ms(0));
    TEST_ASSERT_EQUAL_UINT32(15000, wifi_sup_backoff_ms(1));
    TEST_ASSERT_EQUAL_UINT32(30000, wifi_sup_backoff_ms(2));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_sup_backoff_ms(3));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_sup_backoff_ms(4));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_sup_backoff_ms(255));
}

void test_link_up_does_nothing(void) {
    WifiSupervisor s;
    for (uint32_t t = 0; t < 3600000u; t += 1000) {
        TEST_ASSERT_EQUAL(WIFI_SUP_NONE, poll(&s, t, WIFI_LINK_UP));
    }
    TEST_ASSERT_FALSE(s.down);
    TEST_ASSERT_EQUAL_UINT32(0, wifi_sup_down_s(&s, 3600000u));
}

void test_short_drop_within_grace_is_left_to_the_core(void) {
    WifiSupervisor s;
    poll(&s, 1000, WIFI_LINK_UP);
    uint32_t last = 0;
    TEST_ASSERT_EQUAL(0, run(&s, 2000, 2000 + WIFI_SUP_DOWN_GRACE_MS - 1000, WIFI_LINK_DOWN, &last));
    bool rec = false;
    TEST_ASSERT_EQUAL(WIFI_SUP_NONE, poll(&s, 2000 + WIFI_SUP_DOWN_GRACE_MS, WIFI_LINK_UP, &rec));
    TEST_ASSERT_FALSE(rec);            // the core brought it back, not us
    TEST_ASSERT_FALSE(s.down);
}

void test_first_attempt_after_grace_then_backoff(void) {
    WifiSupervisor s;
    poll(&s, 0, WIFI_LINK_UP);
    uint32_t t0 = 1000;                // outage begins
    uint32_t last = 0;
    // exactly one attempt inside [grace, grace + 15 s)
    TEST_ASSERT_EQUAL(1, run(&s, t0, t0 + WIFI_SUP_DOWN_GRACE_MS + 14000, WIFI_LINK_DOWN, &last));
    TEST_ASSERT_EQUAL_UINT32(t0 + WIFI_SUP_DOWN_GRACE_MS, last);
    uint32_t a1 = last;
    // 2nd after 15 s, 3rd after 30 s more, 4th and 5th after 60 s each
    TEST_ASSERT_EQUAL(1, run(&s, a1 + 1000, a1 + 15000, WIFI_LINK_DOWN, &last));
    TEST_ASSERT_EQUAL_UINT32(a1 + 15000, last);
    uint32_t a2 = last;
    TEST_ASSERT_EQUAL(1, run(&s, a2 + 1000, a2 + 30000, WIFI_LINK_DOWN, &last));
    TEST_ASSERT_EQUAL_UINT32(a2 + 30000, last);
    uint32_t a3 = last;
    TEST_ASSERT_EQUAL(1, run(&s, a3 + 1000, a3 + 60000, WIFI_LINK_DOWN, &last));
    TEST_ASSERT_EQUAL_UINT32(a3 + 60000, last);
    uint32_t a4 = last;
    TEST_ASSERT_EQUAL(1, run(&s, a4 + 1000, a4 + 60000, WIFI_LINK_DOWN, &last));
    TEST_ASSERT_EQUAL_UINT32(a4 + 60000, last);
    TEST_ASSERT_EQUAL_UINT8(5, s.attempts);
    TEST_ASSERT_EQUAL_UINT32((last - t0) / 1000, wifi_sup_down_s(&s, last));
}

void test_gave_up_state_acts_sooner(void) {
    WifiSupervisor s;
    poll(&s, 0, WIFI_LINK_UP);
    uint32_t last = 0;
    // one attempt at the short grace, none before the 15 s back-off
    TEST_ASSERT_EQUAL(1, run(&s, 1000, 1000 + WIFI_SUP_GAVE_UP_GRACE_MS + 14000, WIFI_LINK_GAVE_UP, &last));
    TEST_ASSERT_EQUAL_UINT32(1000 + WIFI_SUP_GAVE_UP_GRACE_MS, last);
    TEST_ASSERT_EQUAL(WIFI_SUP_RECONNECT, poll(&s, last + 15000, WIFI_LINK_GAVE_UP));
}

void test_recovered_after_own_attempt(void) {
    WifiSupervisor s;
    poll(&s, 0, WIFI_LINK_UP);
    uint32_t last = 0;
    run(&s, 1000, 1000 + WIFI_SUP_GAVE_UP_GRACE_MS, WIFI_LINK_GAVE_UP, &last);
    TEST_ASSERT_EQUAL_UINT8(1, s.attempts);
    // our begin(): associating, then an IP
    TEST_ASSERT_EQUAL(WIFI_SUP_NONE, poll(&s, last + 1000, WIFI_LINK_ASSOCIATED));
    bool rec = false;
    TEST_ASSERT_EQUAL(WIFI_SUP_NONE, poll(&s, last + 4000, WIFI_LINK_UP, &rec));
    TEST_ASSERT_TRUE(rec);
    TEST_ASSERT_FALSE(s.down);
    TEST_ASSERT_EQUAL_UINT8(0, s.attempts);
    // the flag is reported once
    TEST_ASSERT_EQUAL(WIFI_SUP_NONE, poll(&s, last + 5000, WIFI_LINK_UP, &rec));
    TEST_ASSERT_FALSE(rec);
    // the next outage starts from the grace again (back-off reset)
    TEST_ASSERT_EQUAL(1, run(&s, last + 6000, last + 6000 + WIFI_SUP_DOWN_GRACE_MS, WIFI_LINK_DOWN, &last));
}

void test_associated_without_ip_gets_dhcp_grace(void) {
    WifiSupervisor s;
    poll(&s, 0, WIFI_LINK_UP);
    // IP lost while associated: nothing for the DHCP grace, then one reconnect
    uint32_t t0 = 1000, last = 0;
    TEST_ASSERT_EQUAL(0, run(&s, t0, t0 + WIFI_SUP_NO_IP_GRACE_MS - 1000, WIFI_LINK_ASSOCIATED, &last));
    TEST_ASSERT_EQUAL(WIFI_SUP_RECONNECT, poll(&s, t0 + WIFI_SUP_NO_IP_GRACE_MS, WIFI_LINK_ASSOCIATED));
    uint32_t a1 = t0 + WIFI_SUP_NO_IP_GRACE_MS;
    // a fresh association after our attempt is not torn down inside its grace,
    // even though the 15 s back-off has passed
    TEST_ASSERT_EQUAL(WIFI_SUP_NONE, poll(&s, a1 + 1000, WIFI_LINK_DOWN));
    TEST_ASSERT_EQUAL(0, run(&s, a1 + 2000, a1 + 2000 + WIFI_SUP_NO_IP_GRACE_MS - 1000, WIFI_LINK_ASSOCIATED, &last));
    TEST_ASSERT_EQUAL(WIFI_SUP_RECONNECT, poll(&s, a1 + 2000 + WIFI_SUP_NO_IP_GRACE_MS, WIFI_LINK_ASSOCIATED));
}

void test_restart_after_long_outage_once(void) {
    WifiSupervisor s;
    poll(&s, 0, WIFI_LINK_UP);
    uint32_t t0 = 1000;
    int restarts = 0, reconnects = 0;
    uint32_t restart_at = 0;
    for (uint32_t t = t0; t <= t0 + WIFI_SUP_RESTART_AFTER_MS + 120000; t += 1000) {
        WifiSupervisorAction a = poll(&s, t, WIFI_LINK_DOWN);
        if (a == WIFI_SUP_RESTART) { restarts++; restart_at = t; }
        else if (a == WIFI_SUP_RECONNECT && restarts == 0) reconnects++;
    }
    TEST_ASSERT_EQUAL(1, restarts);                     // once per outage (the device reboots anyway)
    TEST_ASSERT_EQUAL_UINT32(t0 + WIFI_SUP_RESTART_AFTER_MS, restart_at);
    // before it: 25 s grace, 15 s, 30 s, then every 60 s until 30 min: 3 + (1800 - 70) / 60
    TEST_ASSERT_EQUAL(3 + (1800 - 70) / 60, reconnects);
}

void test_hold_suspends_actions_but_not_timers(void) {
    WifiSupervisor s;
    poll(&s, 0, WIFI_LINK_UP);
    uint32_t t0 = 1000;
    bool r;
    for (uint32_t t = t0; t <= t0 + WIFI_SUP_RESTART_AFTER_MS + 60000; t += 1000) {
        TEST_ASSERT_EQUAL(WIFI_SUP_NONE, wifi_sup_poll(&s, t, WIFI_LINK_DOWN, true, &r));
    }
    TEST_ASSERT_EQUAL_UINT8(0, s.attempts);
    // hold released after > 30 min of outage: the restart is due at once
    TEST_ASSERT_EQUAL(WIFI_SUP_RESTART, wifi_sup_poll(&s, t0 + WIFI_SUP_RESTART_AFTER_MS + 61000, WIFI_LINK_DOWN, false, &r));
}

void test_millis_wraparound(void) {
    WifiSupervisor s;
    uint32_t t0 = 0xFFFFFFFFu - 10000;
    poll(&s, t0, WIFI_LINK_UP);
    uint32_t last = 0;
    TEST_ASSERT_EQUAL(1, run(&s, t0 + 1000, t0 + 1000 + WIFI_SUP_DOWN_GRACE_MS, WIFI_LINK_DOWN, &last));
    TEST_ASSERT_EQUAL_UINT32(t0 + 1000 + WIFI_SUP_DOWN_GRACE_MS, last);   // past zero
    TEST_ASSERT_EQUAL_UINT32(WIFI_SUP_DOWN_GRACE_MS / 1000, wifi_sup_down_s(&s, last));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_backoff_sequence);
    RUN_TEST(test_link_up_does_nothing);
    RUN_TEST(test_short_drop_within_grace_is_left_to_the_core);
    RUN_TEST(test_first_attempt_after_grace_then_backoff);
    RUN_TEST(test_gave_up_state_acts_sooner);
    RUN_TEST(test_recovered_after_own_attempt);
    RUN_TEST(test_associated_without_ip_gets_dhcp_grace);
    RUN_TEST(test_restart_after_long_outage_once);
    RUN_TEST(test_hold_suspends_actions_but_not_timers);
    RUN_TEST(test_millis_wraparound);
    return UNITY_END();
}
