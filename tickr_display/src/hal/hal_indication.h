#pragma once
#include <Arduino.h>

// The LED driver (docs/DEVICE_UI.md "LED and sound"): four layers, the top one
// is written to the pins - a one-shot flash, the system overlays, the
// temporary test colour, the base colour - dimmed by the device setting.
//
// `r g b` = the base colour to start with: what the pads carry at boot
// (power_held_led() - the colour a deep sleep held, else 0/0/0 = off), so
// taking the pins over does not blink. led_recompute() replaces it.
void indication_init(uint8_t r, uint8_t g, uint8_t b);
// Base colour: the *derived* colour of the content (logic/led_model.h,
// written by renderer.cpp's led_recompute() only). `source` is the name
// GET /api/status reports as `led_source` while nothing is on top of it
// ("off" | "payload" | "sign" | "held"; a string literal - the pointer is
// kept) and what the one serial line per *change* of the colour logs.
void indication_led_rgb(uint8_t r, uint8_t g, uint8_t b, const char* source);
// The base colour (as set above; not what an active overlay shows, and not
// scaled by either brightness).
void indication_get_rgb(uint8_t* r, uint8_t* g, uint8_t* b);
// Brightness 0..255 (payload alert.brightness, default 255): a linear scale
// applied to the base colour when it is written. Kept until changed; the
// other layers are not scaled by it. Not held through deep sleep (no PWM there).
void indication_set_brightness(uint8_t bri);
uint8_t indication_brightness();
// The device setting `led_brightness` (AppConfig, 0..255, default 255): a
// persistent dimmer applied to everything written - every layer - so the
// bar is as dim as the user set it whatever the content does. Applied at
// boot from the config and after POST /config. 0 = the LED is dark.
void indication_set_device_brightness(uint8_t bri);
uint8_t indication_device_brightness();
// True when either brightness is 0: the LED is dark while awake, so a deep
// sleep must not hold a colour either (main.cpp).
bool indication_dark();
// The colour actually driven to the LEDs right now - the top layer dimmed
// by the device setting (what /api/status reports as `led`).
void indication_get_written_rgb(uint8_t* r, uint8_t* g, uint8_t* b);
// The name of the layer on top right now (/api/status `led_source`): the
// base colour's source, "test", an overlay's name (ota, recovery, setup,
// pairing, identify, battery_empty, battery_low, offline) or "flash".
const char* indication_source();
// The active overlays as a bit mask, bit (1 << LedOverlay) per overlay
// (/api/status `led_overlays`; 0 = no overlay).
uint16_t indication_overlays();

// Test layer: a colour above the base and below the overlays, for
// INDICATION_TEST_MS (or `ms`), then the base colour is back. Nothing is
// stored. /api/test/led_*_pwm and the panel's test colour write it;
// indication_test_channel() sets one channel (the others keep their test
// value, 0 when the layer was off) and restarts the timeout.
#define INDICATION_TEST_MS 10000u
void indication_test_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t ms = INDICATION_TEST_MS);
void indication_test_channel(uint8_t channel, uint8_t v);   // 0 = red, 1 = green, 2 = blue

// Overlay layer: a state borrows the LEDs, the layer below comes back when
// it ends. Several may be active at once; the lowest number (highest
// priority) is shown. Patterns that move (breathe, blink, pulse) are
// driven by indication_loop() from the main loop; where the main loop is
// blocked (set-up portal) the pattern is steady. Shown at every led_mode.
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
// Main-loop tick for the moving patterns, the end of a flash / test colour;
// also re-writes the PWM duty once a minute so the hardware cannot drift
// from what the driver believes it shows.
void indication_loop();
void indication_set_volume(uint8_t vol);
void indication_beep(int freq, int duration_ms);
// Blocking. Validates the melody first; returns false (and plays nothing) if invalid.
bool indication_play_rtttl(const char *melody);
