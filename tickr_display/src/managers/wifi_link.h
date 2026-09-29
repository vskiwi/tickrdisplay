#pragma once
// Wi-Fi link keeper (docs/DEVICE_UI.md "Wi-Fi link supervision"): the
// hostname, the driver's events as diagnostics counters, and the supervisor
// (logic/wifi_supervisor.h) that re-issues WiFi.begin() when the core's own
// auto-reconnect stopped and restarts the device after a long outage.
//
// The counters live in RTC memory (RTC_NOINIT_ATTR, magic + CRC): they
// survive the supervisor's own restart, an OTA restart and deep sleep, and
// start from zero on a power-on / EN / brownout reset. GET /api/status
// reports them (docs/API.md "Status and configuration").
#include <stdint.h>
#include <stdbool.h>

struct WifiLinkStats {
    uint32_t disconnects;    // link lost while it had an IP (the core's retries are not counted)
    uint32_t reconnects;     // outages ended after at least one supervisor attempt
    uint32_t restarts;       // ESP.restart() by the supervisor
    uint8_t  last_reason;    // wifi_err_reason_t of the last counted disconnect, 0 = none
    uint32_t down_s;         // seconds since the current outage began, 0 when up
};

// Before WiFi.mode(): hostname "<device-name>-XXXXXX" (logic/hostname.h -
// the core applies it when the STA starts, so it must be set first), the
// event handler, the RTC counters (validated / zeroed by reset kind).
void wifi_link_begin(const char* device_name);

// The initial connect succeeded (USB flow): supervise the link from here on.
// Not called on the battery flow - each wake-up is a fresh WiFi.begin().
void wifi_link_arm();

// Once per loop(). `hold` = an access point is up (set-up portal, recovery)
// or a firmware image is being written: the timers run, nothing is done.
void wifi_link_loop(bool hold);

void wifi_link_stats(WifiLinkStats* out);
