#include "hal_indication.h"
#include "../log.h"
#include "hal_pins.h"
#include "../logic/rtttl.h"

// LED PWM Channels
#define LED_PWM_CHAN_R 0
#define LED_PWM_CHAN_G 1
#define LED_PWM_CHAN_B 2
#define LED_PWM_FREQ   5000
#define LED_PWM_RES    8

// Internal state
static uint8_t _current_r = 0;       // base colour (the user's)
static uint8_t _current_g = 0;
static uint8_t _current_b = 0;
static uint8_t _current_volume = 255;
// Overlay layer: bit (1 << priority) per active overlay; a one-shot flash above it.
static uint16_t _overlays = 0;
static uint32_t _flash_until = 0;
static uint8_t  _flash_rgb[3];
static uint32_t _written = 0xFFFFFFFF;   // last value written to the PWM (skip repeats)

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

// What the LEDs should show right now: flash > lowest active overlay > base.
static void refresh() {
    uint32_t now = millis();
    if (_flash_until) {
        if ((int32_t)(now - _flash_until) < 0) { write_rgb(_flash_rgb[0], _flash_rgb[1], _flash_rgb[2]); return; }
        _flash_until = 0;
    }
    uint8_t p = 0;
    for (uint8_t i = 1; i <= 8 && !p; i++) if (_overlays & (1u << i)) p = i;
    uint8_t w;
    switch (p) {
        case LED_OVL_OTA:      write_rgb(0, 0, 255); break;
        case LED_OVL_RECOVERY: {                       // triangle wave, period 2 s
            uint32_t ph = now % 2000u;
            w = (uint8_t)((ph < 1000u ? ph : 2000u - ph) * 255u / 1000u);
            write_rgb(w, w, w);
            break;
        }
        case LED_OVL_SETUP:    write_rgb(255, 255, 255); break;
        case LED_OVL_PAIRING:  write_rgb(255, 120, 0); break;
        case LED_OVL_IDENTIFY: w = (now % 1000u) < 500u ? 255 : 0; write_rgb(w, w, w); break;
        case LED_OVL_BATT_EMPTY:
        case LED_OVL_BATT_LOW: write_rgb(0, 0, 0); break;
        case LED_OVL_OFFLINE:  w = (now % 10000u) < 200u; write_rgb(w ? 255 : 0, w ? 120 : 0, 0); break;
        default:               write_rgb(_current_r, _current_g, _current_b); break;
    }
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
    refresh();
}

void indication_init() {
    // Setup PWM channels
    ledcSetup(LED_PWM_CHAN_R, LED_PWM_FREQ, LED_PWM_RES);
    ledcSetup(LED_PWM_CHAN_G, LED_PWM_FREQ, LED_PWM_RES);
    ledcSetup(LED_PWM_CHAN_B, LED_PWM_FREQ, LED_PWM_RES);

    // Attach pins
    ledcAttachPin(PIN_LED_RED, LED_PWM_CHAN_R);
    ledcAttachPin(PIN_LED_GREEN, LED_PWM_CHAN_G);
    ledcAttachPin(PIN_LED_BLUE, LED_PWM_CHAN_B);
    
    // LEDs OFF (Active LOW -> HIGH = OFF)
    indication_led_rgb(0, 0, 0);

    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW); // Amp OFF
}

void indication_led(bool r, bool g, bool b) {
    indication_led_rgb(r ? 255 : 0, g ? 255 : 0, b ? 255 : 0);
}

void indication_led_rgb(uint8_t r, uint8_t g, uint8_t b) {
    _current_r = r;
    _current_g = g;
    _current_b = b;
    refresh();
}

void indication_get_rgb(uint8_t* r, uint8_t* g, uint8_t* b) {
    if (r) *r = _current_r;
    if (g) *g = _current_g;
    if (b) *b = _current_b;
}

void indication_get_written_rgb(uint8_t* r, uint8_t* g, uint8_t* b) {
    uint32_t v = (_written == 0xFFFFFFFF) ? 0 : _written;   // nothing written yet = off
    if (r) *r = (uint8_t)(v >> 16);
    if (g) *g = (uint8_t)(v >> 8);
    if (b) *b = (uint8_t)v;
}

void indication_set_r(uint8_t r) {
    indication_led_rgb(r, _current_g, _current_b);
}

void indication_set_g(uint8_t g) {
    indication_led_rgb(_current_r, g, _current_b);
}

void indication_set_b(uint8_t b) {
    indication_led_rgb(_current_r, _current_g, b);
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
