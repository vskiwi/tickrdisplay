#pragma once
// Screen state machine - which CARD interrupts the content, and when.
// Pure logic, no Arduino dependencies, unit-tested on the host
// (test/test_device_state). docs/DEVICE_UI.md "State machine" (priorities),
// "E-ink refresh rules" (debounce / rate limits, battery mode) and
// "Power-mode switch".
//
// Division of labour:
//   * logic/status_policy  - the BADGES: debounces the snapshot (60 s Wi-Fi,
//     5 s power) and allows at most one badge-only refresh per minute.
//     The machine below takes the *debounced* power state
//     from it and only the raw link state for Wi-Fi (its own timers are
//     >= 60 s anyway).
//   * this module - the CARDS: a condition (OFFLINE, OTA, BATTERY EMPTY) is
//     shown until it ends or something replaces it, a transition (ON
//     BATTERY) is shown for a few seconds. It never draws: main.cpp polls it
//     once a second, hands the desired card to hal_display (whose base
//     frame becomes the card) and refreshes when the card changed - one
//     refresh per change, never a queue - full for a condition card,
//     partial for the transitional ON-BATTERY card (refresh_policy v2).
//   * this module also holds the runtime MODE SWITCH (it shares the power
//     flip / flapping bookkeeping of the ON-BATTERY card): a debounced
//     USB -> battery flip starts a grace; when the reading held for it and
//     nothing blocks, `power_restart` asks main.cpp to restart into the
//     battery flow (docs/DEVICE_UI.md "Power-mode switch"). Pure decision, no restart here.
//
// The machine is fed by main.cpp on the USB flow (and on a battery device
// that stays awake for configuration). The battery flow (wake -> fetch ->
// sleep) does not run the machine; it uses the two pure helpers at the end
// (device_offline_card_due(), device_offline_age_line()).
#include <stdint.h>
#include <stddef.h>
#include "status_policy.h"   // BadgePower

// --- Timers and thresholds (docs/DEVICE_UI.md "E-ink refresh rules") ---------
// Wi-Fi: badge after T_short (= BADGE_WIFI_DEBOUNCE_POLLS s, status_policy),
// OFFLINE card after T_long of continuous outage. A reconnection counts only
// after it held for DS_WIFI_RESTORE_HOLD_MS ("a router that flaps 3x in a
// minute yields one card and one restore, not six"); shorter ones neither
// end the outage nor restart its timer.
#define DS_T_LONG_MS                600000u   // 10 min
#define DS_WIFI_RESTORE_HOLD_MS     30000u    // 30 s of link before the content comes back
// Power transition card: held 5 s, one card per DS_POWER_CARD_WINDOW_MS;
// a third flip inside the window is "power flapping" - badge only, log it.
#define DS_POWER_CARD_HOLD_MS       5000u
#define DS_POWER_CARD_WINDOW_MS     600000u
#define DS_POWER_FLAPPING_FLIPS     3u
// The ON-BATTERY card comes with the runtime USB->battery mode switch
// (docs/DEVICE_UI.md "Power-mode switch"): a card announcing battery operation while
// the radio stays on would be a false promise, so both are on together.
// 0 = never emit CARD_POWER_BATTERY (badge only; the switch still runs).
#ifndef DS_POWER_CARDS_ENABLED
#define DS_POWER_CARDS_ENABLED      1
#endif
// Runtime mode switch: after a debounced USB -> battery flip the
// device restarts into the battery flow once the reading held for the grace.
// A cable re-inserted inside it cancels silently (no ON-USB card).
#define DS_POWER_GRACE_MS           120000u   // 2 min
// Guards against a restart loop on a misread source (the worst failure):
// the reading *before* the flip must have held DS_POWER_STABLE_MS (a flip
// seconds after boot is the detector settling, not the user), and a
// flapping source (DS_POWER_FLAPPING_FLIPS inside the window) is not
// trusted either - both cases use the long grace instead: the source must
// then read battery for DS_POWER_GRACE_LONG_MS without a break.
#define DS_POWER_STABLE_MS          60000u
#define DS_POWER_GRACE_LONG_MS      600000u   // 10 min = the flapping window
// BATTERY LOW badge + LED off at <= 15 %. The percentage arrives
// quantized to BATTERY_DISPLAY_STEP_PCT (5) with the display hysteresis of
// battery_level_changed(), so the badge clears at the next step (20 %).
#define DS_BATTERY_LOW_PCT          15u
// Content stale marker: T_stale = 3 x refresh interval, at least 10 min.
// Drawn by the ticker's age line; the value is here so it is tested.
#define DS_T_STALE_MIN_MS           600000u
// Battery flow: no frame on failed wake 1, OFFLINE card
// on failure DS_OFFLINE_FAIL_FIRST, then every DS_OFFLINE_FAIL_EVERY-th.
#define DS_OFFLINE_FAIL_FIRST       2u
#define DS_OFFLINE_FAIL_EVERY       4u

// What interrupts the base frame (content or WAITING). Priority as in docs/DEVICE_UI.md "State machine":
// a higher card wins the panel; a transitional card that becomes due under
// a higher one is dropped, a condition comes back when the higher one ends.
enum ScreenCard : uint8_t {
    CARD_NONE = 0,           // base frame: content or WAITING, badges on it
    CARD_OFFLINE,            // condition: no Wi-Fi for T_long, "last update N min ago"
    CARD_POWER_BATTERY,      // transition: USB -> battery, 5 s (see DS_POWER_CARDS_ENABLED)
    CARD_OTA,                // condition: a firmware image is being written
    CARD_BATTERY_EMPTY,      // condition: cell below BATTERY_LOW_MV (battery flow only)
    CARD_COUNT
};
// "none" | "offline" | "power_to_battery" | "ota" | "battery_empty" - the
// `card` field of GET /api/screen/state.
const char* screen_card_str(ScreenCard c);

// Service frames that own the panel: the machine only needs to know
// that one is up, to drop a transitional card (a condition survives it and
// is drawn by the restore).
enum ServiceLevel : uint8_t {
    SERVICE_NONE = 0,
    SERVICE_IDENTIFY,        // identify number (temporary frame)
    SERVICE_PAIRING,         // pairing code / outcome
    SERVICE_AP,              // set-up portal or recovery card
};

// Runtime mode switch, the `power_switch` field of GET /api/screen/state.
enum PowerSwitch : uint8_t {
    PSW_OFF = 0,             // not allowed here: override, no Pull URL, board ?, locked after a misread
    PSW_IDLE,                // allowed, nothing pending
    PSW_GRACE,               // on battery, counting down (power_grace_s)
    PSW_BLOCKED,             // grace over, waiting for OTA / a service frame / recovery to end
    PSW_RESTART,             // restart into the battery flow now (power_restart)
    PSW_COUNT
};
// "off" | "idle" | "grace" | "blocked" | "restart"
const char* power_switch_str(uint8_t s);

struct DeviceInputs {
    uint32_t now_ms;
    bool     wifi_connected;    // raw link state (WiFi.isConnected())
    uint8_t  power;             // BadgePower, *debounced* (the badge snapshot's)
    bool     batt_known;        // false on board ?
    uint8_t  batt_pct;          // quantized percentage (badge snapshot)
    uint32_t content_seq;       // incremented by every content frame (display_content_seq())
    bool     ota_active;        // an image is being written to flash
    uint8_t  service;           // ServiceLevel of the frame on the panel
    // The mode switch. switch_allowed: power_source is `auto`, a Pull
    // URL is configured (the battery flow has something to wake for) and the
    // previous switch did not land back in USB mode (main.cpp's lock).
    // hold: something else that must finish first (recovery mode, portal).
    bool     switch_allowed;
    bool     hold;
};

struct DeviceOutputs {
    ScreenCard card;            // what the base frame should be now
    bool     batt_low;          // BATTERY LOW badge + LED off
    bool     led_offline;       // OFFLINE LED rule: off, amber pulse
    bool     power_flapping;    // >= 3 flips inside the window: log it, badge only
    uint32_t wifi_down_s;       // seconds since the outage began, 0 when up (API)
    uint8_t  power_switch;      // PowerSwitch (API)
    uint32_t power_grace_s;     // seconds left in the grace, 0 outside it (API)
    bool     power_restart;     // restart into the battery flow now
};

struct DeviceState {
    bool     power_cards = DS_POWER_CARDS_ENABLED != 0;
    // Wi-Fi outage
    bool     wifi_down = false;      // an outage is in progress
    bool     wifi_up = false;        // the link is back, holding
    uint32_t wifi_down_at = 0;
    uint32_t wifi_up_at = 0;
    bool     offline_up = false;     // the OFFLINE card is the desired base
    bool     offline_done = false;   // drawn (or replaced) in this outage
    // Power transitions
    bool     power_seen = false;
    uint8_t  power_last = BADGE_POWER_UNKNOWN;
    bool     power_card_up = false;
    uint32_t power_card_at = 0;
    bool     power_window_open = false;
    uint32_t power_window_at = 0;
    uint8_t  power_flips = 0;        // flips inside the window
    bool     power_card_in_window = false;
    // Mode switch
    uint32_t power_last_at = 0;      // when the debounced reading last changed (or was first seen)
    bool     grace_on = false;       // on battery, a restart is pending
    uint32_t grace_at = 0;
    uint32_t grace_ms = 0;           // DS_POWER_GRACE_MS or DS_POWER_GRACE_LONG_MS
    // OTA
    bool     ota_up = false;
    bool     ota_done = false;
    // Content
    bool     content_seen = false;
    uint32_t content_seq = 0;
};

// One poll (main.cpp: once a second). Fills `out`; the caller compares
// out->card with what hal_display holds and redraws on a change.
void device_state_poll(DeviceState* s, const DeviceInputs& in, DeviceOutputs* out);

// BATTERY LOW predicate on a badge snapshot (also used by the renderer).
bool device_battery_low(uint8_t power, bool batt_known, uint8_t batt_pct);

// Battery flow: should the `fail_count`-th consecutive failed
// wake draw the OFFLINE card? 1 -> no, 2 -> yes, then 6, 10, ...
bool device_offline_card_due(uint32_t fail_count);

// "last update N min ago" / "N h ago" for the OFFLINE card's second line;
// age_s = DS_AGE_UNKNOWN (no content shown yet) -> "" (line omitted).
#define DS_AGE_UNKNOWN 0xFFFFFFFFu
void device_offline_age_line(uint32_t age_s, char* buf, size_t len);

// T_stale for a refresh interval in minutes (0 = unknown -> the minimum).
uint32_t device_stale_ms(uint32_t interval_min);
