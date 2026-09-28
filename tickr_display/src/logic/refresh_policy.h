#pragma once
// Refresh policy v2 - partial-first (docs/DEVICE_UI.md "E-ink refresh rules"). Pure logic,
// unit-tested on the host (test/test_refresh_policy).
//
// Decides, once per frame request, what the panel gets: nothing (the
// rendered frame is byte-identical to the shown one), a deferral (too soon
// after the previous refresh - hal_display redraws when the spacing has
// passed), a whole-frame DIFFERENTIAL refresh (SSD1680 "partial": the
// controller drives only the pixels that differ between RAM 0x26 = the
// shown frame and RAM 0x24 = the new one; 0.75 s, no inversion flash) or a
// FULL refresh (4.1 s, OTP waveform, cleans ghosting). hal_display runs it
// and owns the counters; the copy of the shown frame and the partial
// counter live in RTC slow memory so the battery flow can resume with a
// partial after deep sleep.
//
// Rules:
//   * the copy of the shown frame must be trustworthy (CRC, sane reset
//     kind) - else FULL, never garbage on the panel;
//   * content of the same layout kind, badges / stale, service frames
//     (identify, pairing, the transitional ON-BATTERY card) and the base
//     frame back after any card: PARTIAL;
//   * condition cards (OFFLINE / OTA / BATTERY EMPTY), a layout-kind or
//     source change (text <-> ticker <-> WAITING), boot frames, SETUP /
//     RECOVERY and the 24 h hygiene rule: FULL;
//   * forced FULL every RP_FORCE_FULL_EVERY-th partial or RP_FULL_MAX_AGE_MS
//     after the last full (USB); on battery every RP_FORCE_FULL_BATT-th
//     wake drawn partial (no clock across sleeps);
//   * content / badge partials at least RP_PARTIAL_MIN_MS after the previous
//     partial (deferred, never dropped; a full cleaned the panel, so the
//     first partial after one is never held); service frames and restores
//     are exempt (the ON-BATTERY card is up 5 s);
//   * no price threshold: an identical rendered frame is simply not drawn.
#include <stdint.h>

// Constants - tuned on the bench, not settings.
#define RP_FORCE_FULL_EVERY       8u          // partials before a forced full (USB; vendor 5-10)
#define RP_FORCE_FULL_BATT        6u          // wakes drawn partial before a forced full (battery)
#define RP_FULL_MAX_AGE_MS        3600000u    // 60 min since the last full on USB -> the next refresh is full
#define RP_PARTIAL_MIN_MS         30000u      // minimum spacing of content / badge partials (deferred)
// 1: a FULL that replaces a condition card clears to white first
// ("double full after a long-lived card"; default off, bench decides).
#ifndef RP_DOUBLE_FULL_AFTER_CARD
#define RP_DOUBLE_FULL_AFTER_CARD 0
#endif

enum RefreshKind : uint8_t {
    REFRESH_NONE = 0,      // the panel already shows this frame
    REFRESH_DEFER,         // too soon after the previous refresh: draw when RP_PARTIAL_MIN_MS has passed
    REFRESH_PARTIAL,       // whole-frame differential refresh
    REFRESH_FULL,          // full refresh
};

// What asks for the frame.
enum RefreshEvent : uint8_t {
    RP_EV_CONTENT = 0,     // a content frame (payload / MQTT / pull / ticker) or the WAITING card
    RP_EV_BADGE,           // base redraw for a badge change / stale crossing (status_policy's minute rule already applied)
    RP_EV_SERVICE,         // identify digit, pairing code / outcome, the transitional ON-BATTERY card
    RP_EV_RESTORE,         // the base frame back after a service frame or a card
    RP_EV_CONDITION,       // OFFLINE / OTA / BATTERY EMPTY card
    RP_EV_BOOT,            // boot frames, the SETUP and RECOVERY cards
    RP_EV_HYGIENE,         // FULL_REFRESH_MAX_AGE_MS (24 h) without a refresh
    RP_EV_COUNT
};

struct RefreshInputs {
    uint8_t  event = RP_EV_CONTENT;    // RefreshEvent
    bool     prev_valid = false;       // the copy of the shown frame is trustworthy (CRC ok, sane reset kind, drawn once)
    bool     layout_changed = false;   // layout kind (text / ticker / waiting) or ticker source differs from the shown base frame
    bool     frame_identical = false;  // the rendered frame equals the shown one byte for byte
    bool     battery = false;          // battery flow: one frame per wake, the wake counter rules, no clock across sleeps
    uint8_t  partials_since_full = 0;
    uint32_t ms_since_full = 0;        // since the last full refresh (this boot)
    uint32_t ms_since_partial = 0;     // since the last PARTIAL refresh (UINT32_MAX = none on this boot: a full cleaned the panel)
};

RefreshKind refresh_decide(const RefreshInputs& in);
// partials_since_full after a refresh of `kind` (FULL resets, PARTIAL counts, saturating).
uint8_t refresh_count_after(uint8_t partials_since_full, RefreshKind kind);
// Whether a FULL that replaces a condition card is preceded by a clear to
// white (`enabled` = RP_DOUBLE_FULL_AFTER_CARD; a parameter so both settings are tested).
bool refresh_double_full(bool enabled, bool prev_condition_card, RefreshKind kind);
// "none" | "defer" | "partial" | "full" - the log line and the API.
const char* refresh_kind_str(RefreshKind k);
