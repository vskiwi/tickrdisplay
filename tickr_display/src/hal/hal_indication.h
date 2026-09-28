#pragma once
#include <Arduino.h>

void indication_init();
// Base colour - the user's (payload alert.led, /api/test/led). Stored, and
// written to the LEDs unless an overlay is active (docs/DEVICE_UI.md "LED and sound").
void indication_led(bool r, bool g, bool b);
void indication_led_rgb(uint8_t r, uint8_t g, uint8_t b);
// The base colour (as set above; not what an active overlay shows).
void indication_get_rgb(uint8_t* r, uint8_t* g, uint8_t* b);
// The colour actually driven to the LEDs right now - flash / overlay / base,
// whichever is on top (what /api/status reports as `led`).
void indication_get_written_rgb(uint8_t* r, uint8_t* g, uint8_t* b);

// Overlay layer (docs/DEVICE_UI.md "LED and sound"): a state borrows the LEDs, the base
// colour comes back when it ends. Several may be active at once; the
// lowest number (highest priority) is shown. Patterns that move (breathe,
// blink, pulse) are driven by indication_loop() from the main loop; where
// the main loop is blocked (set-up portal) the pattern is steady.
enum LedOverlay : uint8_t {
    LED_OVL_OTA        = 1,   // blue steady
    LED_OVL_RECOVERY   = 2,   // white, slow breathe (1 per 2 s)
    LED_OVL_SETUP      = 3,   // white steady (the stock's "connect to ... WiFi" light)
    LED_OVL_PAIRING    = 4,   // amber steady while the code is on the panel
    LED_OVL_IDENTIFY   = 5,   // white blink 1 Hz
    LED_OVL_BATT_EMPTY = 6,   // off
    LED_OVL_BATT_LOW   = 7,   // off (not dimmed)
    LED_OVL_OFFLINE    = 8,   // off, one amber pulse every 10 s
};
void indication_overlay(LedOverlay o, bool on);
// One-shot colour for `ms` above every overlay (pairing outcome green / red
// 1 s); then the layer below returns.
void indication_flash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
// Main-loop tick for the moving patterns and the end of a flash.
void indication_loop();
void indication_set_r(uint8_t r);
void indication_set_g(uint8_t g);
void indication_set_b(uint8_t b);
void indication_set_volume(uint8_t vol);
void indication_beep(int freq, int duration_ms);
// Blocking. Validates the melody first; returns false (and plays nothing) if invalid.
bool indication_play_rtttl(const char *melody);
