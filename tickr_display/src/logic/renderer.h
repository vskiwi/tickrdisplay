#pragma once
#include <Arduino.h>
#include "payload.h"

void renderer_init();

// Parse + validate + apply. Blocking (e-ink refresh, sounds): call from the main
// loop task only. Returns false if the payload was rejected (reason is logged).
bool renderer_process_payload(const char* json, size_t len);
bool renderer_process_payload(const String& json_payload);

// Apply an already validated payload to the hardware (blocking).
void renderer_apply(const ScreenPayload& p);

// LED rule (docs/TICKERS.md "What the screen shows", AppConfig::led_rule): LED_RULE_OFF = the
// payload's alert.led as today; LED_RULE_SIGN = red / green by the payload's
// direction when it carries no alert.led. Sets the LED *base* colour, so
// the overlays of hal_indication still win and the base returns after them.
void    renderer_set_led_rule(uint8_t rule);
uint8_t renderer_led_rule();

// Play a preset / RTTTL (blocking). `rtttl` is only used for SOUND_RTTTL.
void renderer_play_sound(PayloadSound sound, const char* rtttl);
