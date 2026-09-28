#pragma once
//
// Power-cycle recovery counter (docs/WEB_UI.md "Recovery mode").
//
// The device has no button, only the rear power switch. Switching it off and
// on RECOVERY_THRESHOLD times, each time within RECOVERY_WINDOW_MS of the
// previous start, enters recovery mode. Pure logic: the caller supplies a
// tiny key/value store (NVS on the device, an int in the host tests) and the
// uptime; nothing here knows about Wi-Fi or the display.
//
//   boot:  n = load(); if the reset counts as a power cycle: n++, save(n);
//          n >= threshold -> recovery, save(0). Any other reset clears a
//          leftover count at once (a deep-sleep wake-up is >= 1 min after the
//          previous start, so the series is over anyway; on battery the device
//          sleeps before the window below could close it).
//   tick:  RECOVERY_WINDOW_MS after a counted boot the series is over -> save(0).
//
// Only power-on, external-pin and brownout resets count: a software restart
// (OTA, /api/system/restart), a deep-sleep wake-up or a crash/watchdog reset
// must never look like the user at the switch - otherwise firmware updates
// and crash loops would end in recovery mode.
//
#include <stdint.h>
#include <stdbool.h>

#ifndef RECOVERY_THRESHOLD
#define RECOVERY_THRESHOLD 3
#endif
#ifndef RECOVERY_WINDOW_MS
#define RECOVERY_WINDOW_MS 20000u
#endif

// Reset causes as the counter sees them (mapped from esp_reset_reason()).
enum RecoveryResetKind : uint8_t {
    RECOVERY_RST_POWERON = 0,
    RECOVERY_RST_EXT,        // external pin (EN) - no button on this board, counted anyway
    RECOVERY_RST_BROWNOUT,   // the switch cuts the rail: often reported as brownout
    RECOVERY_RST_SW,         // ESP.restart(): OTA, boot-partition switch, portal timeout
    RECOVERY_RST_DEEPSLEEP,  // battery wake-up
    RECOVERY_RST_CRASH,      // panic / any watchdog
    RECOVERY_RST_OTHER,
};

// True for the kinds that count as "the user toggled the switch".
bool recovery_reset_is_cold(RecoveryResetKind kind);

struct RecoveryStore {
    void*   ctx;
    uint8_t (*load)(void* ctx);              // stored count, 0 when absent
    void    (*save)(void* ctx, uint8_t n);
};

struct RecoveryCounter {
    RecoveryStore store;
    uint8_t position;    // 1-based position of this boot in a series, 0 = not a counted boot
    bool    recovery;    // threshold reached on this boot
    bool    armed;       // save(0) still due when the window closes
};

// Call once at boot, as early as possible. Returns true when recovery mode
// is to be entered (the store is already reset to 0 in that case).
bool recovery_counter_boot(RecoveryCounter* c, const RecoveryStore* store, bool cold);

// Call periodically with the uptime in ms. Closes the window once: saves 0
// and returns true on the call that did it, false otherwise.
bool recovery_counter_tick(RecoveryCounter* c, uint32_t uptime_ms);
