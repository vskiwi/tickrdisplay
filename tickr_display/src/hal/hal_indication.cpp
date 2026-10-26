#include "hal_indication.h"
#include "../log.h"
#include "hal_pins.h"
#include "../logic/rtttl.h"
#include "../logic/led_model.h"   // led_scale(), LED_BRIGHTNESS_DEFAULT

// LED PWM Channels
#define LED_PWM_CHAN_R 0
#define LED_PWM_CHAN_G 1
#define LED_PWM_CHAN_B 2
#define LED_PWM_FREQ   5000
#define LED_PWM_RES    8

// Internal state
static uint8_t _current_r = 0;       // base colour (the derived one, led_recompute())
static uint8_t _current_g = 0;
static uint8_t _current_b = 0;
static const char* _base_source = "off";   // its name (/api/status led_source)
static uint8_t _current_bri = 255;   // payload alert.brightness: scales the base colour only
static uint8_t _device_bri = LED_BRIGHTNESS_DEFAULT;   // config led_brightness: scales everything written
static uint8_t _current_volume = 255;
// Overlay layer: bit (1 << priority) per active overlay; a one-shot flash above it.
static uint16_t _overlays = 0;
static uint32_t _flash_until = 0;
static uint8_t  _flash_rgb[3];
// Test layer (between the overlays and the base): 0 = off.
static uint32_t _test_until = 0;
static uint8_t  _test_rgb[3];
// Layer ids of top_layer(): 1..8 = the overlay of that priority.
#define LAYER_BASE  0
#define LAYER_TEST  9
#define LAYER_FLASH 10
// Last value written to the PWM (skip repeats). WRITTEN_NONE = the cache
// does not vouch for the hardware: the next refresh() writes whatever it
// computes. The cache is dropped after the pins are attached (below) and
// once per LED_REWRITE_MS from indication_loop(), so a duty the hardware
// did not take - or lost - is written again rather than trusted forever
// (docs/DEVICE_UI.md "LED and sound").
#define WRITTEN_NONE   0xFFFFFFFFu
#define LED_REWRITE_MS 60000u
static uint32_t _written = WRITTEN_NONE;
static uint32_t _rewrite_at = 0;

// Active LOW: 255 = OFF, 0 = ON. The API is intuitive (255 = full on), so
// the duty is inverted here.
static void write_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (v == _written) return;
    _written = v;
    ledcWrite(LED_PWM_CHAN_R, 255 - r);
    ledcWrite(LED_PWM_CHAN_G, 255 - g);
    ledcWrite(LED_PWM_CHAN_B, 255 - b);
}

// Which layer is on top right now: flash > lowest active overlay > test >
// base. Ends an expired flash / test colour on the way.
static uint8_t top_layer(uint32_t now) {
    if (_flash_until) {
        if ((int32_t)(now - _flash_until) < 0) return LAYER_FLASH;
        _flash_until = 0;
    }
    for (uint8_t i = 1; i <= 8; i++) if (_overlays & (1u << i)) return i;
    if (_test_until) {
        if ((int32_t)(now - _test_until) < 0) return LAYER_TEST;
        _test_until = 0;
        Serial.println("LED: test colour over");
    }
    return LAYER_BASE;
}

// The colour of the top layer. On the intuitive 0..255 values;
// write_rgb() inverts for the active-LOW pins.
static void layer_rgb(uint32_t now, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t w;
    *r = *g = *b = 0;
    switch (top_layer(now)) {
        case LAYER_FLASH:      *r = _flash_rgb[0]; *g = _flash_rgb[1]; *b = _flash_rgb[2]; break;
        case LAYER_TEST:       *r = _test_rgb[0]; *g = _test_rgb[1]; *b = _test_rgb[2]; break;
        case LED_OVL_OTA:      *b = 255; break;
        case LED_OVL_RECOVERY: {                       // triangle wave, period 2 s
            uint32_t ph = now % 2000u;
            w = (uint8_t)((ph < 1000u ? ph : 2000u - ph) * 255u / 1000u);
            *r = *g = *b = w;
            break;
        }
        case LED_OVL_SETUP:    *r = *g = *b = 255; break;
        case LED_OVL_PAIRING:  *r = 255; *g = 120; break;
        case LED_OVL_IDENTIFY: w = (now % 1000u) < 500u ? 255 : 0; *r = *g = *b = w; break;
        case LED_OVL_BATT_EMPTY:
        case LED_OVL_BATT_LOW: break;                  // off
        case LED_OVL_OFFLINE:  if ((now % 10000u) < 200u) { *r = 255; *g = 120; } break;
        default:               // base colour, scaled by the payload's brightness
            *r = led_scale(_current_r, _current_bri);
            *g = led_scale(_current_g, _current_bri);
            *b = led_scale(_current_b, _current_bri);
            break;
    }
}

// What the LEDs should show right now: the top layer, dimmed by the device
// setting (led_brightness) - every layer, so a 0 keeps the bar dark.
static void refresh() {
    uint8_t r, g, b;
    layer_rgb(millis(), &r, &g, &b);
    write_rgb(led_scale(r, _device_bri), led_scale(g, _device_bri), led_scale(b, _device_bri));
}

void indication_overlay(LedOverlay o, bool on) {
    uint16_t bit = 1u << o;
    uint16_t next = on ? (_overlays | bit) : (_overlays & ~bit);
    if (next == _overlays) return;
    _overlays = next;
    refresh();
}

void indication_flash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    _flash_rgb[0] = r; _flash_rgb[1] = g; _flash_rgb[2] = b;
    _flash_until = millis() + ms;
    if (!_flash_until) _flash_until = 1;
    refresh();
}

void indication_loop() {
    // Periodic re-write: drop the cache so the computed colour reaches the
    // hardware again (three ledcWrite a minute). The moving patterns and a
    // flash are unaffected - refresh() recomputes the layer every tick anyway.
    uint32_t now = millis();
    if ((int32_t)(now - _rewrite_at) >= 0) {
        _rewrite_at = now + LED_REWRITE_MS;
        _written = WRITTEN_NONE;
    }
    refresh();
}

// One base-colour log line per change, with where the colour came from.
static void set_base(uint8_t r, uint8_t g, uint8_t b, const char* source) {
    bool changed = r != _current_r || g != _current_g || b != _current_b;
    _current_r = r;
    _current_g = g;
    _current_b = b;
    _base_source = source ? source : "?";
    if (changed) Serial.printf("LED: base %u/%u/%u <- %s\n", r, g, b, _base_source);
}

void indication_init(uint8_t r, uint8_t g, uint8_t b) {
    // The PWM channels start at the colour the pads already carry (the one
    // the deep sleep held, else off), so taking the pins over does not blink.
    set_base(r, g, b, (r | g | b) ? "held" : "off");
    ledcSetup(LED_PWM_CHAN_R, LED_PWM_FREQ, LED_PWM_RES);
    ledcSetup(LED_PWM_CHAN_G, LED_PWM_FREQ, LED_PWM_RES);
    ledcSetup(LED_PWM_CHAN_B, LED_PWM_FREQ, LED_PWM_RES);
    write_rgb(r, g, b);   // duty before the pins are attached: no off glitch
    // ledcAttachPin() copies the duty the hardware *reports*, and that
    // register follows a write only at the next PWM period: give it two
    // periods, so the pins come up at the duty just written.
    delayMicroseconds(2u * 1000000u / LED_PWM_FREQ);

    // Attach pins
    ledcAttachPin(PIN_LED_RED, LED_PWM_CHAN_R);
    ledcAttachPin(PIN_LED_GREEN, LED_PWM_CHAN_G);
    ledcAttachPin(PIN_LED_BLUE, LED_PWM_CHAN_B);
    // Whatever duty the attach took, the channels now hold the colour the
    // cache says they do: write it once more, unconditionally. An attach
    // that copied a stale 0 ("on" on the active-LOW pins) would otherwise be
    // believed off and never corrected.
    _written = WRITTEN_NONE;
    refresh();
    _rewrite_at = millis() + LED_REWRITE_MS;   // the battery wake (seconds) never gets there

    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW); // Amp OFF
}

void indication_led_rgb(uint8_t r, uint8_t g, uint8_t b, const char* source) {
    set_base(r, g, b, source);
    refresh();
}

void indication_test_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    _test_rgb[0] = r; _test_rgb[1] = g; _test_rgb[2] = b;
    _test_until = millis() + ms;
    if (!_test_until) _test_until = 1;
    Serial.printf("LED: test %u/%u/%u for %lu ms\n", r, g, b, (unsigned long)ms);
    refresh();
}

void indication_test_channel(uint8_t channel, uint8_t v) {
    if (channel > 2) return;
    bool active = _test_until && (int32_t)(millis() - _test_until) < 0;
    if (!active) _test_rgb[0] = _test_rgb[1] = _test_rgb[2] = 0;   // a fresh test starts from dark
    _test_rgb[channel] = v;
    indication_test_rgb(_test_rgb[0], _test_rgb[1], _test_rgb[2]);
}

void indication_get_rgb(uint8_t* r, uint8_t* g, uint8_t* b) {
    if (r) *r = _current_r;
    if (g) *g = _current_g;
    if (b) *b = _current_b;
}

void indication_set_brightness(uint8_t bri) {
    if (bri == _current_bri) return;
    _current_bri = bri;
    refresh();
}

uint8_t indication_brightness() {
    return _current_bri;
}

void indication_set_device_brightness(uint8_t bri) {
    if (bri == _device_bri) return;
    _device_bri = bri;
    refresh();
}

uint8_t indication_device_brightness() {
    return _device_bri;
}

bool indication_dark() {
    return _current_bri == 0 || _device_bri == 0;
}

void indication_get_written_rgb(uint8_t* r, uint8_t* g, uint8_t* b) {
    uint32_t v = (_written == WRITTEN_NONE) ? 0 : _written;   // nothing written yet = off
    if (r) *r = (uint8_t)(v >> 16);
    if (g) *g = (uint8_t)(v >> 8);
    if (b) *b = (uint8_t)v;
}

uint16_t indication_overlays() {
    return _overlays;
}

const char* indication_source() {
    switch (top_layer(millis())) {
        case LAYER_FLASH:        return "flash";
        case LAYER_TEST:         return "test";
        case LED_OVL_OTA:        return "ota";
        case LED_OVL_RECOVERY:   return "recovery";
        case LED_OVL_SETUP:      return "setup";
        case LED_OVL_PAIRING:    return "pairing";
        case LED_OVL_IDENTIFY:   return "identify";
        case LED_OVL_BATT_EMPTY: return "battery_empty";
        case LED_OVL_BATT_LOW:   return "battery_low";
        case LED_OVL_OFFLINE:    return "offline";
        default:                 return _base_source;
    }
}

void indication_set_volume(uint8_t vol) {
    _current_volume = vol;
}

// Internal tone function that doesn't toggle AMP_EN every time
static void _play_tone(int freq, int duration_ms) {
    if (freq == 0) {
        delay(duration_ms);
        return;
    }
    
    unsigned long start = millis();
    int period_us = 1000000 / freq;
    
    // Simple square wave with volume control
    while (millis() - start < (unsigned long)duration_ms) {
        dacWrite(PIN_AUDIO_DAC, _current_volume);
        delayMicroseconds(period_us / 2);
        dacWrite(PIN_AUDIO_DAC, 0);
        delayMicroseconds(period_us / 2);
    }
}

void indication_beep(int freq, int duration_ms) {
    digitalWrite(PIN_AMP_EN, HIGH);
    _play_tone(freq, duration_ms);
    dacWrite(PIN_AUDIO_DAC, 0);
    digitalWrite(PIN_AMP_EN, LOW);
}

// RTTTL Player: parsing/validation lives in logic/rtttl.cpp (pure, host-testable);
// this function only drives the hardware. Blocking: must be called from the main
// loop task (via the command queue), never from async_tcp / MQTT callbacks.
bool indication_play_rtttl(const char *melody) {
    RtttlSong song;
    if (!rtttl_parse(melody, &song)) {
        Serial.println("RTTTL: invalid melody, not playing");
        return false;
    }
    LOGV("RTTTL: playing '%s' (%u notes, %lu ms)\n",
                  song.name, (unsigned)song.note_count, (unsigned long)song.total_ms);

    digitalWrite(PIN_AMP_EN, HIGH);

    RtttlIter it;
    RtttlNote n;
    rtttl_iter_init(&it, &song);
    while (rtttl_iter_next(&it, &n)) {
        _play_tone(n.freq_hz, n.duration_ms);
    }

    digitalWrite(PIN_AMP_EN, LOW);
    dacWrite(PIN_AUDIO_DAC, 0);
    return true;
}
