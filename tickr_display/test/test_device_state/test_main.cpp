// Unity tests for the screen state machine (src/logic/device_state.cpp) -
// docs/DEVICE_UI.md "State machine", "E-ink refresh rules" (timers, battery
// flow), "Power-mode switch". The machine is driven with synthetic 1 s timelines.
#include <unity.h>
#include <string.h>
#include "logic/device_state.h"
#include "logic/ui_strings.h"

// A timeline: one poll per second with the inputs of `in` (now_ms advances).
struct Sim {
    DeviceState  s;
    DeviceInputs in;
    DeviceOutputs out;
    uint32_t t = 0;
    Sim() {
        memset(&in, 0, sizeof(in));
        memset(&out, 0, sizeof(out));
        in.wifi_connected = true;
        in.power = BADGE_POWER_USB;
        in.batt_known = true;
        in.batt_pct = 100;
        in.content_seq = 1;
        in.switch_allowed = true;   // auto mode, Pull URL set, not locked
        poll();   // boot poll: everything nominal, no card
    }
    // The USB reading has stood long enough for a flip to earn the short grace.
    void settle() { hold(DS_POWER_STABLE_MS / 1000u + 1, CARD_NONE); }
    // One 1 s poll; returns the desired card.
    ScreenCard poll() {
        in.now_ms = t * 1000u;
        device_state_poll(&s, in, &out);
        t++;
        return out.card;
    }
    // Poll for `n` seconds asserting the card stays `expect` the whole time.
    void hold(unsigned n, ScreenCard expect) {
        for (unsigned i = 0; i < n; i++) TEST_ASSERT_EQUAL_UINT8(expect, poll());
    }
};

static const unsigned T_LONG_S = DS_T_LONG_MS / 1000u;
static const unsigned HOLD_S   = DS_WIFI_RESTORE_HOLD_MS / 1000u;

// --- Wi-Fi: staged loss -----------------------------------------------------

void test_wifi_drop_card_exactly_at_t_long_once() {
    Sim sim;
    sim.in.wifi_connected = false;
    sim.hold(T_LONG_S, CARD_NONE);              // 10 min - 1 s: badge territory only
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
    TEST_ASSERT_EQUAL_UINT32(T_LONG_S, sim.out.wifi_down_s);
    TEST_ASSERT_TRUE(sim.out.led_offline);
    sim.hold(3600, CARD_OFFLINE);               // stays for the whole outage
}

void test_wifi_up_before_t_long_never_a_card() {
    Sim sim;
    sim.in.wifi_connected = false;
    sim.hold(T_LONG_S - 5, CARD_NONE);
    sim.in.wifi_connected = true;
    // T_long passes 5 s into the restore hold: no card while the link is up
    // (it would be removed 25 s later - two wasted refreshes)
    sim.hold(HOLD_S + 5, CARD_NONE);            // outage over after the hold
    TEST_ASSERT_EQUAL_UINT32(0, sim.out.wifi_down_s);
    TEST_ASSERT_FALSE(sim.out.led_offline);
    sim.in.wifi_connected = false;              // a new outage starts its own timer
    sim.hold(T_LONG_S, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
}

void test_router_flapping_three_times_in_a_minute_is_one_outage() {
    Sim sim;
    // down 15 s, up 5 s, three times: one outage, the timer runs from the first drop
    for (int k = 0; k < 3; k++) {
        sim.in.wifi_connected = false; sim.hold(15, CARD_NONE);
        sim.in.wifi_connected = true;  sim.hold(5, CARD_NONE);
    }
    sim.in.wifi_connected = false;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    TEST_ASSERT_EQUAL_UINT32(60, sim.out.wifi_down_s);
    sim.hold(T_LONG_S - 61, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
    // a 20 s reconnection does not restore the content
    sim.in.wifi_connected = true;  sim.hold(20, CARD_OFFLINE);
    sim.in.wifi_connected = false; sim.hold(30, CARD_OFFLINE);
    // the link holds 30 s: one restore, timers reset
    sim.in.wifi_connected = true;
    sim.hold(HOLD_S, CARD_OFFLINE);
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    TEST_ASSERT_EQUAL_UINT32(0, sim.out.wifi_down_s);
    TEST_ASSERT_FALSE(sim.s.wifi_down);
}

void test_content_refresh_replaces_the_offline_card_for_this_outage() {
    Sim sim;
    sim.in.wifi_connected = false;
    sim.hold(T_LONG_S, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
    sim.in.content_seq++;                       // a payload was drawn
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    TEST_ASSERT_TRUE(sim.out.led_offline);      // the LED rule follows the condition, not the card
    sim.hold(3600, CARD_NONE);                  // once per outage
    // next outage: the card again
    sim.in.wifi_connected = true;  sim.hold(HOLD_S + 1, CARD_NONE);
    sim.in.wifi_connected = false; sim.hold(T_LONG_S, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
}

void test_wifi_down_seconds_reported_from_the_first_drop() {
    Sim sim;
    sim.in.wifi_connected = false;
    for (unsigned i = 0; i < 90; i++) {
        sim.poll();
        TEST_ASSERT_EQUAL_UINT32(i, sim.out.wifi_down_s);
    }
    sim.in.wifi_connected = true;
    sim.poll();
    TEST_ASSERT_EQUAL_UINT32(90, sim.out.wifi_down_s);   // still an outage while the hold runs
}

// --- Battery low --------------------------------------------------------------

void test_battery_low_badge_at_15_percent_on_battery_only() {
    TEST_ASSERT_FALSE(device_battery_low(BADGE_POWER_BATTERY, true, 20));
    TEST_ASSERT_TRUE(device_battery_low(BADGE_POWER_BATTERY, true, 15));
    TEST_ASSERT_TRUE(device_battery_low(BADGE_POWER_BATTERY, true, 0));
    TEST_ASSERT_FALSE(device_battery_low(BADGE_POWER_USB, true, 15));      // charging: the bolt, no alarm
    TEST_ASSERT_FALSE(device_battery_low(BADGE_POWER_UNKNOWN, true, 15));  // no information, no alarm
    TEST_ASSERT_FALSE(device_battery_low(BADGE_POWER_BATTERY, false, 15)); // board ?: level unknown
}

void test_battery_decline_sets_low_and_led_off_without_a_card() {
    Sim sim;
    sim.s.power_cards = false;                  // the USB -> battery transition card is tested below
    sim.in.power = BADGE_POWER_BATTERY;
    for (uint8_t pct = 100; pct > 15; pct -= 5) {
        sim.in.batt_pct = pct;
        TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
        TEST_ASSERT_FALSE(sim.out.batt_low);
    }
    sim.in.batt_pct = 15;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());   // a badge, never a card (BATTERY LOW)
    TEST_ASSERT_TRUE(sim.out.batt_low);
    sim.in.batt_pct = 10;
    sim.poll();
    TEST_ASSERT_TRUE(sim.out.batt_low);
    // hysteresis is the 5 % display step: the next quantized value clears it
    sim.in.batt_pct = 20;
    sim.poll();
    TEST_ASSERT_FALSE(sim.out.batt_low);
    // plugged in: the alarm ends with the bolt even at a low level
    sim.in.batt_pct = 10; sim.in.power = BADGE_POWER_USB;
    sim.poll();
    TEST_ASSERT_FALSE(sim.out.batt_low);
}

// --- OTA card -------------------------------------------------------------------

void test_ota_card_for_the_whole_flash_then_restore() {
    Sim sim;
    sim.in.ota_active = true;
    TEST_ASSERT_EQUAL_UINT8(CARD_OTA, sim.poll());
    sim.hold(120, CARD_OTA);
    sim.in.ota_active = false;                  // failed / aborted: the base frame comes back
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    sim.in.ota_active = true;                   // a second attempt draws it again
    TEST_ASSERT_EQUAL_UINT8(CARD_OTA, sim.poll());
}

void test_ota_card_replaced_by_content_does_not_come_back() {
    Sim sim;
    sim.in.ota_active = true;
    TEST_ASSERT_EQUAL_UINT8(CARD_OTA, sim.poll());
    sim.in.content_seq++;
    sim.hold(60, CARD_NONE);
}

void test_ota_outranks_offline_and_offline_returns_after_it() {
    Sim sim;
    sim.in.wifi_connected = false;
    sim.hold(T_LONG_S, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
    sim.in.ota_active = true;                   // (a LAN update while the router is down)
    sim.hold(10, CARD_OTA);
    sim.in.ota_active = false;
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());   // the condition re-evaluates
}

// --- Power transitions (the card is on by default, with the mode switch) --

void test_power_cards_are_on_by_default_and_can_be_turned_off() {
    Sim sim;
    TEST_ASSERT_TRUE(sim.s.power_cards);
    sim.s.power_cards = false;                  // badge only, the switch still runs
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY;
    sim.hold(10, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);
    sim.in.power = BADGE_POWER_USB;
    sim.hold(10, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(PSW_IDLE, sim.out.power_switch);
}

void test_usb_to_battery_card_five_seconds_no_card_back_to_usb() {
    Sim sim;
    sim.s.power_cards = true;
    sim.in.power = BADGE_POWER_BATTERY;         // debounced flip (status_policy did the 5 s)
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    sim.hold(DS_POWER_CARD_HOLD_MS / 1000u - 1, CARD_POWER_BATTERY);
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());     // 5 s: restore = content with the battery
    sim.hold(60, CARD_NONE);
    sim.in.power = BADGE_POWER_USB;             // content with the bolt, no ON-USB card
    sim.hold(30, CARD_NONE);
    TEST_ASSERT_FALSE(sim.out.power_flapping);
}

void test_boot_on_battery_is_not_a_transition() {
    Sim sim;
    // a fresh machine whose first poll already says battery (battery device awake for config)
    DeviceState s; s.power_cards = true;
    DeviceInputs in; memset(&in, 0, sizeof(in));
    in.wifi_connected = true; in.power = BADGE_POWER_BATTERY; in.batt_known = true; in.batt_pct = 60;
    DeviceOutputs out;
    device_state_poll(&s, in, &out);
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, out.card);
    in.now_ms = 1000;
    device_state_poll(&s, in, &out);
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, out.card);
}

void test_power_flapping_one_card_per_window_then_badge_only() {
    Sim sim;
    sim.s.power_cards = true;
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());       // flip 1: card
    sim.hold(DS_POWER_CARD_HOLD_MS / 1000u - 1, CARD_POWER_BATTERY);
    sim.hold(5, CARD_NONE);
    sim.in.power = BADGE_POWER_USB;  sim.poll();                    // flip 2
    TEST_ASSERT_FALSE(sim.out.power_flapping);
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());                 // flip 3: badge only
    TEST_ASSERT_TRUE(sim.out.power_flapping);
    sim.hold(10, CARD_NONE);
    // after the window a fresh flip earns a card again
    sim.in.power = BADGE_POWER_USB;  sim.poll();
    sim.hold(DS_POWER_CARD_WINDOW_MS / 1000u, CARD_NONE);
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    TEST_ASSERT_FALSE(sim.out.power_flapping);
}

void test_transitional_card_dropped_under_a_service_frame_not_queued() {
    Sim sim;
    sim.s.power_cards = true;
    sim.in.service = SERVICE_IDENTIFY;
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    sim.in.service = SERVICE_NONE;              // identify ends: nothing owed
    sim.hold(30, CARD_NONE);
}

void test_condition_card_survives_a_service_frame() {
    Sim sim;
    sim.in.service = SERVICE_AP;                // recovery card up during the whole outage
    sim.in.wifi_connected = false;
    sim.hold(T_LONG_S, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());   // reported; hal draws it when the service ends
    sim.in.service = SERVICE_NONE;
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
}

void test_unknown_power_never_a_card_never_low() {
    Sim sim;
    sim.s.power_cards = true;
    sim.in.power = BADGE_POWER_UNKNOWN; sim.in.batt_pct = 5;
    sim.hold(10, CARD_NONE);
    TEST_ASSERT_FALSE(sim.out.batt_low);
    sim.in.power = BADGE_POWER_BATTERY;         // information came back: not a transition
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    TEST_ASSERT_TRUE(sim.out.batt_low);
    sim.in.power = BADGE_POWER_UNKNOWN;
    sim.hold(10, CARD_NONE);
    TEST_ASSERT_FALSE(sim.out.power_flapping);
}

void test_power_card_over_offline_then_offline_returns() {
    Sim sim;
    sim.s.power_cards = true;
    sim.in.wifi_connected = false;
    sim.hold(T_LONG_S, CARD_NONE);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    sim.hold(DS_POWER_CARD_HOLD_MS / 1000u - 1, CARD_POWER_BATTERY);
    TEST_ASSERT_EQUAL_UINT8(CARD_OFFLINE, sim.poll());
}

// --- Runtime mode switch (docs/DEVICE_UI.md "Power-mode switch") ---------------------------------

static const unsigned GRACE_S      = DS_POWER_GRACE_MS / 1000u;
static const unsigned GRACE_LONG_S = DS_POWER_GRACE_LONG_MS / 1000u;
static const unsigned CARD_S       = DS_POWER_CARD_HOLD_MS / 1000u;

// Polls `n` seconds asserting the switch stays `sw` and never asks for a restart.
static void hold_switch(Sim& sim, unsigned n, uint8_t sw) {
    for (unsigned i = 0; i < n; i++) {
        sim.poll();
        TEST_ASSERT_EQUAL_UINT8(sw, sim.out.power_switch);
        TEST_ASSERT_FALSE(sim.out.power_restart);
    }
}

void test_cable_out_card_then_grace_then_restart_at_two_minutes() {
    Sim sim;
    sim.settle();
    TEST_ASSERT_EQUAL_UINT8(PSW_IDLE, sim.out.power_switch);
    sim.in.power = BADGE_POWER_BATTERY;         // t0: the debounced flip
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);
    TEST_ASSERT_EQUAL_UINT32(GRACE_S, sim.out.power_grace_s);
    TEST_ASSERT_FALSE(sim.out.power_restart);
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);   // card 5 s
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());               // t0 + 5: content with the battery
    TEST_ASSERT_EQUAL_UINT32(GRACE_S - CARD_S, sim.out.power_grace_s);
    hold_switch(sim, GRACE_S - CARD_S - 2, PSW_GRACE);            // ... t0 + 118
    TEST_ASSERT_EQUAL_UINT32(2, sim.out.power_grace_s);
    sim.poll();                                                   // t0 + 119: last second
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);
    TEST_ASSERT_EQUAL_UINT32(1, sim.out.power_grace_s);
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());               // t0 + 120: restart
    TEST_ASSERT_EQUAL_UINT8(PSW_RESTART, sim.out.power_switch);
    TEST_ASSERT_TRUE(sim.out.power_restart);
    TEST_ASSERT_EQUAL_UINT32(0, sim.out.power_grace_s);
}

void test_cable_back_at_30_s_cancels_silently() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, 25, PSW_GRACE);
    sim.in.power = BADGE_POWER_USB;             // t0 + 30
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());               // no ON-USB card
    TEST_ASSERT_EQUAL_UINT8(PSW_IDLE, sim.out.power_switch);
    TEST_ASSERT_EQUAL_UINT32(0, sim.out.power_grace_s);
    hold_switch(sim, 2 * GRACE_S, PSW_IDLE);    // nothing owed, ever
}

void test_cable_back_at_119_s_cancels_silently() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY;
    sim.poll();
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, GRACE_S - CARD_S, PSW_GRACE);                // t0 + 119 reached
    TEST_ASSERT_EQUAL_UINT32(1, sim.out.power_grace_s);
    sim.in.power = BADGE_POWER_USB;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());               // t0 + 120: cancelled, not restarted
    TEST_ASSERT_EQUAL_UINT8(PSW_IDLE, sim.out.power_switch);
    TEST_ASSERT_FALSE(sim.out.power_restart);
    hold_switch(sim, 60, PSW_IDLE);
}

void test_third_flip_inside_the_window_is_flapping_no_card_long_grace() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY; sim.poll();                // flip 1: card + grace
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, 60, PSW_GRACE);
    sim.in.power = BADGE_POWER_USB;     sim.poll();                // flip 2 at t0 + 65: cancel
    TEST_ASSERT_FALSE(sim.out.power_flapping);
    sim.hold(70, CARD_NONE);                                       // USB held > DS_POWER_STABLE_MS
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());                // flip 3 inside 10 min: flapping, badge only
    TEST_ASSERT_TRUE(sim.out.power_flapping);
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);
    TEST_ASSERT_EQUAL_UINT32(GRACE_LONG_S, sim.out.power_grace_s); // ... and the long grace, not 2 min
}

void test_flapping_source_needs_ten_stable_minutes_before_a_restart() {
    Sim sim;
    sim.settle();
    for (int k = 0; k < 2; k++) {                                  // out, in, out, in
        sim.in.power = BADGE_POWER_BATTERY; for (int i = 0; i < 21; i++) sim.poll();
        sim.in.power = BADGE_POWER_USB;     sim.poll(); sim.hold(70, CARD_NONE);
    }
    sim.in.power = BADGE_POWER_BATTERY;                            // 5th flip: flapping
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    TEST_ASSERT_TRUE(sim.out.power_flapping);
    TEST_ASSERT_EQUAL_UINT32(GRACE_LONG_S, sim.out.power_grace_s);
    hold_switch(sim, GRACE_S + 60, PSW_GRACE);                     // 2 min is not enough now
    hold_switch(sim, GRACE_LONG_S - GRACE_S - 60 - 1, PSW_GRACE);
    sim.poll();                                                    // 10 min of uninterrupted battery
    TEST_ASSERT_EQUAL_UINT8(PSW_RESTART, sim.out.power_switch);
    TEST_ASSERT_TRUE(sim.out.power_restart);
}

void test_flip_right_after_boot_uses_the_long_grace() {
    Sim sim;                                    // boot poll at t = 0 said USB
    sim.hold(5, CARD_NONE);
    sim.in.power = BADGE_POWER_BATTERY;         // 6 s later: the boot detector settling, or the user - unknowable
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);
    TEST_ASSERT_EQUAL_UINT32(GRACE_LONG_S, sim.out.power_grace_s);
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, GRACE_LONG_S - CARD_S, PSW_GRACE);
    sim.poll();
    TEST_ASSERT_TRUE(sim.out.power_restart);
}

void test_ota_in_progress_defers_the_restart_until_it_ends() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY; sim.poll();
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, 30, PSW_GRACE);
    sim.in.ota_active = true;                   // a LAN update starts on the cell
    for (unsigned i = 0; i < GRACE_S - CARD_S - 30; i++) TEST_ASSERT_EQUAL_UINT8(CARD_OTA, sim.poll());
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);
    TEST_ASSERT_EQUAL_UINT32(1, sim.out.power_grace_s);
    TEST_ASSERT_EQUAL_UINT8(CARD_OTA, sim.poll());                // t0 + 120: due, but blocked
    TEST_ASSERT_EQUAL_UINT8(PSW_BLOCKED, sim.out.power_switch);
    TEST_ASSERT_FALSE(sim.out.power_restart);
    for (unsigned i = 0; i < 300; i++) { sim.poll(); TEST_ASSERT_FALSE(sim.out.power_restart); }
    sim.in.ota_active = false;                  // aborted (a success reboots by itself)
    sim.poll();
    TEST_ASSERT_EQUAL_UINT8(PSW_RESTART, sim.out.power_switch);
    TEST_ASSERT_TRUE(sim.out.power_restart);
}

void test_pairing_and_identify_frames_defer_the_restart() {
    Sim sim;
    sim.settle();
    sim.in.service = SERVICE_PAIRING;           // the code is on the panel when the cable goes
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());               // card dropped ...
    TEST_ASSERT_EQUAL_UINT8(PSW_GRACE, sim.out.power_switch);     // ... the grace runs regardless
    hold_switch(sim, GRACE_S - 1, PSW_GRACE);
    hold_switch(sim, 90, PSW_BLOCKED);                            // the session outlives the grace
    sim.in.service = SERVICE_IDENTIFY;                            // straight into an identify
    hold_switch(sim, 30, PSW_BLOCKED);
    sim.in.service = SERVICE_NONE;
    sim.poll();
    TEST_ASSERT_TRUE(sim.out.power_restart);
}

void test_recovery_hold_defers_and_cable_back_while_blocked_cancels() {
    Sim sim;
    sim.settle();
    sim.in.hold = true;                         // recovery AP up (the card may already be gone)
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, GRACE_S - CARD_S, PSW_GRACE);
    hold_switch(sim, 600, PSW_BLOCKED);
    sim.in.power = BADGE_POWER_USB;             // plugged in meanwhile
    sim.poll();
    TEST_ASSERT_EQUAL_UINT8(PSW_IDLE, sim.out.power_switch);
    sim.in.hold = false;
    hold_switch(sim, 300, PSW_IDLE);            // nothing owed
}

void test_unknown_board_never_restarts() {
    Sim sim;
    sim.in.batt_known = false;                  // board ?: no level, no rule
    sim.settle();
    TEST_ASSERT_EQUAL_UINT8(PSW_OFF, sim.out.power_switch);
    sim.in.power = BADGE_POWER_BATTERY;         // (an override could still say battery)
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());
    TEST_ASSERT_EQUAL_UINT8(PSW_OFF, sim.out.power_switch);
    for (unsigned i = 0; i < 2 * GRACE_LONG_S; i++) {
        sim.poll();
        TEST_ASSERT_EQUAL_UINT8(PSW_OFF, sim.out.power_switch);
        TEST_ASSERT_FALSE(sim.out.power_restart);
    }
}

void test_switch_not_allowed_card_yes_restart_never() {
    Sim sim;                                    // override `usb`, no Pull URL, or locked after a misread
    sim.in.switch_allowed = false;
    sim.settle();
    TEST_ASSERT_EQUAL_UINT8(PSW_OFF, sim.out.power_switch);
    sim.in.power = BADGE_POWER_BATTERY;
    TEST_ASSERT_EQUAL_UINT8(CARD_POWER_BATTERY, sim.poll());      // the fact is still shown
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    for (unsigned i = 0; i < 2 * GRACE_LONG_S; i++) {
        TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
        TEST_ASSERT_EQUAL_UINT8(PSW_OFF, sim.out.power_switch);
        TEST_ASSERT_FALSE(sim.out.power_restart);
    }
}

void test_switch_revoked_during_the_grace_cancels_it() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY; sim.poll();
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, 40, PSW_GRACE);
    sim.in.switch_allowed = false;              // Pull URL removed / override set at runtime
    hold_switch(sim, 2 * GRACE_S, PSW_OFF);
    sim.in.switch_allowed = true;               // allowed again: no flip, no grace
    hold_switch(sim, 2 * GRACE_S, PSW_IDLE);
}

void test_unknown_reading_during_the_grace_cancels_it() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY; sim.poll();
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    hold_switch(sim, 40, PSW_GRACE);
    sim.in.power = BADGE_POWER_UNKNOWN;         // the detector lost its channel
    hold_switch(sim, 30, PSW_IDLE);
    sim.in.power = BADGE_POWER_BATTERY;         // information back: not a transition
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    hold_switch(sim, 2 * GRACE_LONG_S, PSW_IDLE);
}

void test_boot_on_battery_never_restarts() {
    // A battery device awake for configuration (no Pull URL yet): its first
    // reading is battery - no transition, no grace, even once a URL arrives.
    DeviceState s;
    DeviceInputs in; memset(&in, 0, sizeof(in));
    in.wifi_connected = true; in.power = BADGE_POWER_BATTERY; in.batt_known = true; in.batt_pct = 60;
    in.content_seq = 1; in.switch_allowed = false;
    DeviceOutputs out;
    for (uint32_t t = 0; t < 2 * GRACE_LONG_S; t++) {
        if (t == 30) in.switch_allowed = true;  // POST /config with a Pull URL
        in.now_ms = t * 1000u;
        device_state_poll(&s, in, &out);
        TEST_ASSERT_EQUAL_UINT8(CARD_NONE, out.card);
        TEST_ASSERT_FALSE(out.power_restart);
        TEST_ASSERT_EQUAL_UINT8(t < 30 ? PSW_OFF : PSW_IDLE, out.power_switch);
    }
}

void test_grace_survives_a_content_refresh_and_an_offline_card() {
    Sim sim;
    sim.settle();
    sim.in.power = BADGE_POWER_BATTERY; sim.poll();
    sim.hold(CARD_S - 1, CARD_POWER_BATTERY);
    sim.in.content_seq++;                       // a pull landed
    TEST_ASSERT_EQUAL_UINT8(CARD_NONE, sim.poll());
    sim.in.wifi_connected = false;              // and the router went down
    hold_switch(sim, GRACE_S - CARD_S - 1, PSW_GRACE);
    sim.poll();
    TEST_ASSERT_TRUE(sim.out.power_restart);    // the battery flow copes with a dead router by itself
}

void test_power_switch_names() {
    TEST_ASSERT_EQUAL_STRING("off", power_switch_str(PSW_OFF));
    TEST_ASSERT_EQUAL_STRING("idle", power_switch_str(PSW_IDLE));
    TEST_ASSERT_EQUAL_STRING("grace", power_switch_str(PSW_GRACE));
    TEST_ASSERT_EQUAL_STRING("blocked", power_switch_str(PSW_BLOCKED));
    TEST_ASSERT_EQUAL_STRING("restart", power_switch_str(PSW_RESTART));
    TEST_ASSERT_EQUAL_STRING("unknown", power_switch_str(PSW_COUNT));
    // the card's second line fits the 12 pt line (3-digit interval = 257 px)
    TEST_ASSERT_TRUE(strlen("updates every 240 min") <= 21);
}

// --- Battery flow helpers ---------------------------------------------------------

void test_offline_card_on_second_failed_wake_then_every_fourth() {
    TEST_ASSERT_FALSE(device_offline_card_due(0));
    TEST_ASSERT_FALSE(device_offline_card_due(1));
    TEST_ASSERT_TRUE(device_offline_card_due(2));
    TEST_ASSERT_FALSE(device_offline_card_due(3));
    TEST_ASSERT_FALSE(device_offline_card_due(4));
    TEST_ASSERT_FALSE(device_offline_card_due(5));
    TEST_ASSERT_TRUE(device_offline_card_due(6));
    TEST_ASSERT_TRUE(device_offline_card_due(10));
    TEST_ASSERT_FALSE(device_offline_card_due(11));
}

void test_offline_age_line_minutes_hours_unknown() {
    char buf[40];
    device_offline_age_line(42 * 60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("last update 42 min ago", buf);
    device_offline_age_line(59 * 60 + 59, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("last update 59 min ago", buf);
    device_offline_age_line(60 * 60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("last update 1 h ago", buf);
    device_offline_age_line(26 * 3600 + 100, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("last update 26 h ago", buf);
    device_offline_age_line(30, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("last update 0 min ago", buf);
    device_offline_age_line(DS_AGE_UNKNOWN, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    // never longer than the 12 pt line allows (2-digit minutes = 262 px)
    TEST_ASSERT_TRUE(strlen("last update 59 min ago") <= 22);
}

void test_stale_is_three_intervals_at_least_ten_minutes() {
    TEST_ASSERT_EQUAL_UINT32(DS_T_STALE_MIN_MS, device_stale_ms(0));
    TEST_ASSERT_EQUAL_UINT32(DS_T_STALE_MIN_MS, device_stale_ms(1));
    TEST_ASSERT_EQUAL_UINT32(DS_T_STALE_MIN_MS, device_stale_ms(3));
    TEST_ASSERT_EQUAL_UINT32(15u * 60000u, device_stale_ms(5));
    TEST_ASSERT_EQUAL_UINT32(180u * 60000u, device_stale_ms(60));
}

// --- Names for the API ---------------------------------------------------------------

void test_card_and_state_names() {
    TEST_ASSERT_EQUAL_STRING("none", screen_card_str(CARD_NONE));
    TEST_ASSERT_EQUAL_STRING("offline", screen_card_str(CARD_OFFLINE));
    TEST_ASSERT_EQUAL_STRING("power_to_battery", screen_card_str(CARD_POWER_BATTERY));
    TEST_ASSERT_EQUAL_STRING("ota", screen_card_str(CARD_OTA));
    TEST_ASSERT_EQUAL_STRING("battery_empty", screen_card_str(CARD_BATTERY_EMPTY));
    TEST_ASSERT_EQUAL_STRING("unknown", screen_card_str(CARD_COUNT));
    // the card frame kinds, and the original names untouched
    TEST_ASSERT_EQUAL_STRING("offline", screen_state_str(SCREEN_OFFLINE));
    TEST_ASSERT_EQUAL_STRING("battery_empty", screen_state_str(SCREEN_BATTERY_EMPTY));
    TEST_ASSERT_EQUAL_STRING("power_to_battery", screen_state_str(SCREEN_POWER_BATTERY));
    TEST_ASSERT_EQUAL_STRING("ota", screen_state_str(SCREEN_OTA));
    TEST_ASSERT_EQUAL_STRING("identify", screen_state_str(SCREEN_IDENTIFY));
}

void setUp() {}
void tearDown() {}

int runUnityTests() {
    UNITY_BEGIN();
    RUN_TEST(test_wifi_drop_card_exactly_at_t_long_once);
    RUN_TEST(test_wifi_up_before_t_long_never_a_card);
    RUN_TEST(test_router_flapping_three_times_in_a_minute_is_one_outage);
    RUN_TEST(test_content_refresh_replaces_the_offline_card_for_this_outage);
    RUN_TEST(test_wifi_down_seconds_reported_from_the_first_drop);
    RUN_TEST(test_battery_low_badge_at_15_percent_on_battery_only);
    RUN_TEST(test_battery_decline_sets_low_and_led_off_without_a_card);
    RUN_TEST(test_ota_card_for_the_whole_flash_then_restore);
    RUN_TEST(test_ota_card_replaced_by_content_does_not_come_back);
    RUN_TEST(test_ota_outranks_offline_and_offline_returns_after_it);
    RUN_TEST(test_power_cards_are_on_by_default_and_can_be_turned_off);
    RUN_TEST(test_usb_to_battery_card_five_seconds_no_card_back_to_usb);
    RUN_TEST(test_boot_on_battery_is_not_a_transition);
    RUN_TEST(test_power_flapping_one_card_per_window_then_badge_only);
    RUN_TEST(test_transitional_card_dropped_under_a_service_frame_not_queued);
    RUN_TEST(test_condition_card_survives_a_service_frame);
    RUN_TEST(test_unknown_power_never_a_card_never_low);
    RUN_TEST(test_power_card_over_offline_then_offline_returns);
    RUN_TEST(test_cable_out_card_then_grace_then_restart_at_two_minutes);
    RUN_TEST(test_cable_back_at_30_s_cancels_silently);
    RUN_TEST(test_cable_back_at_119_s_cancels_silently);
    RUN_TEST(test_third_flip_inside_the_window_is_flapping_no_card_long_grace);
    RUN_TEST(test_flapping_source_needs_ten_stable_minutes_before_a_restart);
    RUN_TEST(test_flip_right_after_boot_uses_the_long_grace);
    RUN_TEST(test_ota_in_progress_defers_the_restart_until_it_ends);
    RUN_TEST(test_pairing_and_identify_frames_defer_the_restart);
    RUN_TEST(test_recovery_hold_defers_and_cable_back_while_blocked_cancels);
    RUN_TEST(test_unknown_board_never_restarts);
    RUN_TEST(test_switch_not_allowed_card_yes_restart_never);
    RUN_TEST(test_switch_revoked_during_the_grace_cancels_it);
    RUN_TEST(test_unknown_reading_during_the_grace_cancels_it);
    RUN_TEST(test_boot_on_battery_never_restarts);
    RUN_TEST(test_grace_survives_a_content_refresh_and_an_offline_card);
    RUN_TEST(test_power_switch_names);
    RUN_TEST(test_offline_card_on_second_failed_wake_then_every_fourth);
    RUN_TEST(test_offline_age_line_minutes_hours_unknown);
    RUN_TEST(test_stale_is_three_intervals_at_least_ten_minutes);
    RUN_TEST(test_card_and_state_names);
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
