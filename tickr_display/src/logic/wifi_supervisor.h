#pragma once
// Wi-Fi link supervisor (docs/DEVICE_UI.md "Wi-Fi link supervision"): when
// does the awake device re-issue a connect attempt itself, and when does it
// give up and restart. Pure logic, unit-tested on the host
// (test/test_wifi_supervisor); managers/wifi_link.cpp feeds it once a second
// with the driver's status and executes the action.
//
// Why it exists: the Arduino-ESP32 core's own auto-reconnect is one
// WiFi.begin() per disconnect event and stops for good on several reason
// codes (an AP that rejects the authentication while it boots, a band-steering
// disassociation, ...). Without this module a device that lost the router
// for a few minutes could stay on the OFFLINE card until a power cycle.
//
//   * the link is down for WIFI_SUP_DOWN_GRACE_MS (the core's own retry gets
//     its chance) -> reconnect; then again after 15 s, 30 s, 60 s, 60 s, ...
//     (WIFI_SUP_BACKOFF_MIN_MS doubling to WIFI_SUP_BACKOFF_MAX_MS);
//   * the driver reports that it gave up (WL_CONNECT_FAILED, WL_NO_SSID_AVAIL,
//     STA stopped) -> the first reconnect after WIFI_SUP_GAVE_UP_GRACE_MS;
//   * associated without an IP address for WIFI_SUP_NO_IP_GRACE_MS (a DHCP
//     lease lost and not renewed) -> the same reconnect, which also restarts
//     the DHCP client; a fresh association is never interrupted earlier;
//   * WIFI_SUP_RESTART_AFTER_MS without WL_CONNECTED -> restart the device
//     (a software reset: no recovery-counter step, the e-ink resumes with a
//     partial refresh from the RTC copy of the frame);
//   * the link back (WL_CONNECTED) ends the outage and resets the back-off.
#include <stdint.h>
#include <stdbool.h>

#define WIFI_SUP_DOWN_GRACE_MS      25000u     // down this long before the first own attempt
#define WIFI_SUP_GAVE_UP_GRACE_MS   10000u     // the core stopped retrying: sooner
#define WIFI_SUP_NO_IP_GRACE_MS     120000u    // associated, no IP: DHCP gets 2 min
#define WIFI_SUP_BACKOFF_MIN_MS     15000u     // wait after the 1st attempt
#define WIFI_SUP_BACKOFF_MAX_MS     60000u     // ceiling
#define WIFI_SUP_RESTART_AFTER_MS   1800000u   // 30 min: the last line of defence

// The driver's status as the supervisor sees it (mapped from wl_status_t).
enum WifiLinkState : uint8_t {
    WIFI_LINK_UP = 0,       // WL_CONNECTED: associated and an IP address
    WIFI_LINK_ASSOCIATED,   // WL_IDLE_STATUS: associated (or connecting), no IP yet / IP lost
    WIFI_LINK_DOWN,         // WL_DISCONNECTED, WL_CONNECTION_LOST: the core may still be retrying
    WIFI_LINK_GAVE_UP,      // WL_CONNECT_FAILED, WL_NO_SSID_AVAIL, WL_NO_SHIELD: the core stopped
};

enum WifiSupervisorAction : uint8_t {
    WIFI_SUP_NONE = 0,
    WIFI_SUP_RECONNECT,     // WiFi.disconnect(); WiFi.begin() with the saved network
    WIFI_SUP_RESTART,       // ESP.restart()
};

struct WifiSupervisor {
    bool     down = false;          // an outage is in progress
    uint32_t down_at = 0;           // when it began (millis)
    uint32_t assoc_at = 0;          // when the link last became ASSOCIATED (no-IP grace)
    bool     assoc = false;         // the previous poll saw ASSOCIATED
    uint8_t  attempts = 0;          // reconnects issued in this outage (saturating)
    uint32_t last_attempt_ms = 0;   // when the last one was issued
    bool     restarted = false;     // WIFI_SUP_RESTART already returned (once per outage)
};

// One poll (once a second). Returns the action to take now; *recovered is
// set when this poll saw the link back after an outage in which at least one
// own reconnect was issued (the "wifi_reconnects" counter). `hold` = do not
// act (an access point is up, an image is being flashed); the timers run on.
WifiSupervisorAction wifi_sup_poll(WifiSupervisor* s, uint32_t now_ms, WifiLinkState link, bool hold, bool* recovered);

// Seconds since the outage began, 0 when the link is up.
uint32_t wifi_sup_down_s(const WifiSupervisor* s, uint32_t now_ms);

// Pure piece: the wait after the n-th attempt (n >= 1): 15 s, 30 s, 60 s, 60 s, ...
uint32_t wifi_sup_backoff_ms(uint8_t attempts);
