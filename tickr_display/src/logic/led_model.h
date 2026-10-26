#pragma once
// The LED model (docs/DEVICE_UI.md "LED and sound") - the pure part, unit-tested
// on the host (test/test_led_model). The base colour of the LED is *derived*:
// from the device setting `led_mode` and the inputs of the last render (the
// payload's alert.led, the lead ticker's direction), recomputed after every
// render and after every settings change; nothing a payload or a test once
// wrote survives on its own. hal_indication drives the pins (overlays, the
// test layer and the dimmers on top); renderer.cpp's led_recompute() is the glue.
#include <stdint.h>
#include "ticker.h"   // TickerDir

// What the content may do with the LED (AppConfig::led_mode).
enum LedMode : uint8_t {
    LED_MODE_OFF     = 0,   // the content never lights the LED (alert.led is ignored) - the default
    LED_MODE_PAYLOAD = 1,   // the colour of the current screen's alert.led; none = dark
    LED_MODE_SIGN    = 2,   // red / green by the lead ticker's change; alert.led wins when present
};
// "off" | "payload" | "sign" <-> value. The parser also takes the values of
// the former `led_rule` setting (same words: off, sign); unknown strings -> off.
const char* led_mode_str(uint8_t v);
uint8_t     led_mode_parse(const char* s);
bool        led_mode_valid(const char* s);   // one of the three canonical words

// The LED through deep sleep (AppConfig::led_sleep, the former `led_battery`).
enum LedSleep : uint8_t {
    LED_SLEEP_OFF  = 0,   // the LED pins are held dark between wake-ups (default)
    LED_SLEEP_KEEP = 1,   // the derived colour stays lit through the sleep (>= LED_SLEEP_MIN_PCT)
};
#define LED_SLEEP_MIN_PCT 30u
// "off" | "keep" <-> value; "hold" (the former led_battery word) parses as keep; unknown -> off.
const char* led_sleep_str(uint8_t v);
uint8_t     led_sleep_parse(const char* s);
bool        led_sleep_valid(const char* s);  // "off" | "keep"

// Where the derived base colour came from (GET /api/status `led_source`
// when no overlay / test colour is on top).
enum LedSource : uint8_t {
    LED_SRC_OFF     = 0,   // nothing to show: the base is dark
    LED_SRC_PAYLOAD = 1,   // alert.led of the last render
    LED_SRC_SIGN    = 2,   // red / green by the lead ticker's direction
    LED_SRC_HELD    = 3,   // the colour a deep sleep held, until the first render of this boot
};
const char* led_source_str(uint8_t v);

// The pure derivation. `held_*` is the colour the pads carried at boot (0/0/0
// = none) and `rendered` says whether a render happened since: before the
// first render the held colour stands in for the derived one (so a wake-up
// does not blink), after it only the render inputs count. Returns the
// LedSource; *r *g *b are the colour (0/0/0 with LED_SRC_OFF).
uint8_t led_derive(uint8_t mode, bool rendered, bool has_led, uint8_t pr, uint8_t pg, uint8_t pb,
                   bool has_dir, TickerDir dir, uint8_t held_r, uint8_t held_g, uint8_t held_b,
                   uint8_t* r, uint8_t* g, uint8_t* b);

// --- the model's state (one device) -------------------------------------------
// The mode (from the configuration), the colour a deep sleep held, the
// inputs of the last render. led_model_derive() runs led_derive() on them.
void    led_model_set_mode(uint8_t mode);
uint8_t led_model_mode();
void    led_model_set_held(uint8_t r, uint8_t g, uint8_t b);
// A render happened: its alert.led (has_led + colour) and the direction of
// the lead ticker / first tile / first grid cell (has_dir + dir).
void    led_model_render(bool has_led, uint8_t r, uint8_t g, uint8_t b, bool has_dir, TickerDir dir);
// The derived colour from the stored state; returns the LedSource (also
// kept for led_model_source()).
uint8_t led_model_derive(uint8_t* r, uint8_t* g, uint8_t* b);
uint8_t led_model_source();   // the result of the last led_model_derive()
void    led_model_reset();    // tests: back to boot state

// The pins through deep sleep (docs/DEVICE_UI.md "LED and sound"): the
// derived colour thresholded per channel (>= 128 = on; deep sleep has no
// PWM) when `setting` is keep, the level is known and not below the floor;
// dark otherwise (also when the colour is dark - nothing to keep). Returns
// true when any channel is lit.
bool led_sleep_hold(uint8_t setting, bool batt_known, uint8_t batt_pct,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t* hr, uint8_t* hg, uint8_t* hb);
// The end-of-wake blink after a successful fetch: skipped when the sign
// mode coloured the LED - the colour is the news.
bool led_success_blink(uint8_t source);

// LED brightness: one channel value scaled linearly by a 0..255 brightness,
// rounded (255 = unchanged, 0 = dark).
uint8_t led_scale(uint8_t v, uint8_t bri);
// The device setting `led_brightness` (AppConfig, 0..255, default 255):
// a persistent dimmer over everything the LED shows - the derived colour,
// the test colour, the overlays and the flashes. 0 = the LED is dark.
#define LED_BRIGHTNESS_DEFAULT 255u
