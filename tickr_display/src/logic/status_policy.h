#pragma once
// Badge refresh policy - pure logic, unit-tested on the host
// (test/test_status_policy). docs/DEVICE_UI.md "Screens" (badges), "E-ink refresh rules".
//
// There is no status bar and no partial-refresh channel for it. The
// facts it would show are badges drawn *into* the content frame by the content
// renderer, so a badge change costs nothing while the content refreshes
// anyway. This module decides what a change of the snapshot is allowed to
// do on its own:
//   * cosmetic (battery step, IP): adopt at once, never refresh - the next
//     content refresh draws it ("badge change alone: 0");
//   * state (Wi-Fi up/down, power source): adopt after it survived a
//     debounce of consecutive 1 s polls (60 s = T_short for Wi-Fi,
//     5 s for USB/battery), then one refresh of the content (partial under
//     refresh_policy v2) - at most
//     one badge-only refresh per BADGE_REFRESH_MIN_MS.
// Lesson of a bench refresh storm (164 partials + 17 fulls in 20 min from
// RSSI jitter): the Wi-Fi *bars* are not drawn at all any more; the
// snapshot keeps them for GET /api/screen/state only.
#include <stdint.h>

// What the badges know. main.cpp collects it once a second; hal_display
// draws it into the bottom-right corner of every base frame.
enum BadgePower : uint8_t {
    BADGE_POWER_UNKNOWN = 0,   // detector INVALID in auto mode: no glyph at all (POWER UNKNOWN)
    BADGE_POWER_USB     = 1,   // lightning bolt
    BADGE_POWER_BATTERY = 2,   // battery outline, fill + "NN%" when the level is known
};
struct DisplayStatus {
    bool    wifi_connected = false;
    int8_t  wifi_bars = -1;      // API only (-1 = no network, 0..3); not drawn, not compared
    bool    usb = false;         // kept for the API ("usb": true/false)
    uint8_t power = BADGE_POWER_UNKNOWN;
    bool    batt_known = true;   // false on board ?: outline without a level
    uint8_t batt_pct = 0;        // quantized to BATTERY_DISPLAY_STEP_PCT
    char    ip[16] = "";         // WAITING card address; "" when not connected
};

// Debounce (consecutive 1 s polls) before a state change reaches the frame.
#define BADGE_WIFI_DEBOUNCE_POLLS   60u    // T_short: a router that flaps for a minute draws nothing
#define BADGE_POWER_DEBOUNCE_POLLS  5u     // an adapter dipping under load must not redraw
// Minimum spacing between two refreshes caused by a badge alone.
#define BADGE_REFRESH_MIN_MS        60000u

// Classification of shown -> cur as a bitmask.
enum BadgeChange : uint8_t {
    BADGE_SAME     = 0,
    BADGE_COSMETIC = 1,   // batt_pct, ip
    BADGE_POWER    = 2,   // power / usb / batt_known
    BADGE_WIFI     = 4,   // wifi_connected
    BADGE_STALE    = 8,   // the ticker's age line crossed T_stale (ticker_stale_crossing): owed one refresh, no debounce
};
// Compares the snapshots (never sets BADGE_STALE - the caller ORs it in).
uint8_t badge_change(const DisplayStatus& shown, const DisplayStatus& cur);

// What the caller should do after one poll.
enum BadgeAction : uint8_t {
    BADGE_KEEP    = 0,   // nothing (or only a state change still being debounced)
    BADGE_ADOPT   = 1,   // store `cur` as the badge snapshot; no refresh of its own
    BADGE_REFRESH = 2,   // store `cur` and redraw the base frame now (one refresh; its kind is refresh_policy's)
};
struct BadgePolicy {
    uint8_t pending = 0;   // state bits being debounced (0 = none)
    uint8_t count   = 0;   // consecutive polls they were seen
    bool    due     = false; // an adopted state change still waits for its refresh
};
// `change` = badge_change(shown, cur), plus BADGE_STALE once when the ticker's
// age crossed T_stale (that crossing is owed exactly one refresh,
// under the same one-per-minute rule as a state change - a refresh of any
// kind meanwhile draws the marker and settles the debt); `since_refresh_ms`
// = time since the panel was last refreshed by anything; `refreshed` = the
// panel refreshed since the previous poll (the stored snapshot went with it).
BadgeAction badge_poll(BadgePolicy* p, uint8_t change, uint32_t since_refresh_ms, bool refreshed);
