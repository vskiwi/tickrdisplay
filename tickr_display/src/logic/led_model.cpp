#include "led_model.h"
#include <string.h>

const char* led_mode_str(uint8_t v) {
    return v == LED_MODE_SIGN ? "sign" : v == LED_MODE_PAYLOAD ? "payload" : "off";
}

uint8_t led_mode_parse(const char* s) {
    if (!s) return LED_MODE_OFF;
    if (strcmp(s, "sign") == 0) return LED_MODE_SIGN;
    if (strcmp(s, "payload") == 0) return LED_MODE_PAYLOAD;
    return LED_MODE_OFF;
}

bool led_mode_valid(const char* s) {
    return s && (strcmp(s, "off") == 0 || strcmp(s, "payload") == 0 || strcmp(s, "sign") == 0);
}

const char* led_sleep_str(uint8_t v) {
    return v == LED_SLEEP_KEEP ? "keep" : "off";
}

uint8_t led_sleep_parse(const char* s) {
    return (s && (strcmp(s, "keep") == 0 || strcmp(s, "hold") == 0)) ? LED_SLEEP_KEEP : LED_SLEEP_OFF;
}

bool led_sleep_valid(const char* s) {
    return s && (strcmp(s, "off") == 0 || strcmp(s, "keep") == 0);
}

const char* led_source_str(uint8_t v) {
    switch (v) {
        case LED_SRC_PAYLOAD: return "payload";
        case LED_SRC_SIGN:    return "sign";
        case LED_SRC_HELD:    return "held";
        default:              return "off";
    }
}

uint8_t led_derive(uint8_t mode, bool rendered, bool has_led, uint8_t pr, uint8_t pg, uint8_t pb,
                   bool has_dir, TickerDir dir, uint8_t held_r, uint8_t held_g, uint8_t held_b,
                   uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = *g = *b = 0;
    if (mode == LED_MODE_OFF) return LED_SRC_OFF;                 // the content never lights the LED
    if (!rendered) {                                              // before the first render: what the sleep held
        if ((held_r | held_g | held_b) == 0) return LED_SRC_OFF;
        *r = held_r; *g = held_g; *b = held_b;
        return LED_SRC_HELD;
    }
    if (has_led) { *r = pr; *g = pg; *b = pb; return LED_SRC_PAYLOAD; }   // an explicit colour wins
    if (mode != LED_MODE_SIGN || !has_dir || dir == 0) return LED_SRC_OFF;
    *r = dir < 0 ? 255 : 0;
    *g = dir > 0 ? 255 : 0;
    return LED_SRC_SIGN;
}

// --- state --------------------------------------------------------------------
static uint8_t   s_mode = LED_MODE_OFF;
static uint8_t   s_held[3] = { 0, 0, 0 };
static bool      s_rendered = false;
static bool      s_has_led = false;
static uint8_t   s_led[3] = { 0, 0, 0 };
static bool      s_has_dir = false;
static TickerDir s_dir = 0;
static uint8_t   s_source = LED_SRC_OFF;

void led_model_set_mode(uint8_t mode) { s_mode = mode; }
uint8_t led_model_mode() { return s_mode; }

void led_model_set_held(uint8_t r, uint8_t g, uint8_t b) {
    s_held[0] = r; s_held[1] = g; s_held[2] = b;
}

void led_model_render(bool has_led, uint8_t r, uint8_t g, uint8_t b, bool has_dir, TickerDir dir) {
    s_rendered = true;
    s_has_led = has_led;
    s_led[0] = r; s_led[1] = g; s_led[2] = b;
    s_has_dir = has_dir;
    s_dir = dir;
}

uint8_t led_model_derive(uint8_t* r, uint8_t* g, uint8_t* b) {
    s_source = led_derive(s_mode, s_rendered, s_has_led, s_led[0], s_led[1], s_led[2], s_has_dir, s_dir,
                          s_held[0], s_held[1], s_held[2], r, g, b);
    return s_source;
}

uint8_t led_model_source() { return s_source; }

void led_model_reset() {
    s_mode = LED_MODE_OFF;
    s_held[0] = s_held[1] = s_held[2] = 0;
    s_rendered = false;
    s_has_led = false;
    s_led[0] = s_led[1] = s_led[2] = 0;
    s_has_dir = false;
    s_dir = 0;
    s_source = LED_SRC_OFF;
}

bool led_sleep_hold(uint8_t setting, bool batt_known, uint8_t batt_pct,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t* hr, uint8_t* hg, uint8_t* hb) {
    *hr = *hg = *hb = 0;
    if (setting != LED_SLEEP_KEEP || !batt_known || batt_pct < LED_SLEEP_MIN_PCT) return false;
    *hr = r >= 128 ? 255 : 0;
    *hg = g >= 128 ? 255 : 0;
    *hb = b >= 128 ? 255 : 0;
    return (*hr | *hg | *hb) != 0;
}

bool led_success_blink(uint8_t source) {
    return source != LED_SRC_SIGN;
}

uint8_t led_scale(uint8_t v, uint8_t bri) {
    return (uint8_t)(((uint32_t)v * bri + 127u) / 255u);
}
