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

// The LED (docs/DEVICE_UI.md "LED and sound", logic/led_model.h): the base
// colour is derived from `led_mode` and the last render. renderer_apply()
// and fetch_grid() record the render; led_recompute() derives the colour
// and writes it as the base of hal_indication (the overlays still win).
// Main task only (the LED driver's). Called after every render, after
// every change of led_mode / led_brightness / led_sleep, and at boot.
void renderer_set_led_mode(uint8_t mode);
void led_recompute();

// Play a preset / RTTTL (blocking). `rtttl` is only used for SOUND_RTTTL.
void renderer_play_sound(PayloadSound sound, const char* rtttl);
