// Unity tests for the derived LED model (src/logic/led_model.cpp) -
// docs/DEVICE_UI.md "LED and sound": the derivation (mode x alert.led x
// direction), the held colour at boot, the setting words and their
// aliases (the migration of led_rule / led_battery), the sleep hold, the
// success blink and the brightness scale.
#include <unity.h>
#include <string.h>
#include "logic/led_model.h"

void setUp(void) { led_model_reset(); }
void tearDown(void) {}

// --- the derivation --------------------------------------------------------------

static uint8_t derive(uint8_t mode, bool has_led, uint8_t pr, uint8_t pg, uint8_t pb, bool has_dir, TickerDir dir,
                      uint8_t* r, uint8_t* g, uint8_t* b) {
    return led_derive(mode, true, has_led, pr, pg, pb, has_dir, dir, 0, 0, 0, r, g, b);
}

void test_derive_off_ignores_everything(void) {
    uint8_t r = 7, g = 7, b = 7;
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, derive(LED_MODE_OFF, true, 0x12, 0x34, 0x56, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);   // dark, not "untouched": nothing is sticky
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, derive(LED_MODE_OFF, false, 0, 0, 0, true, 1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
}

void test_derive_payload_mode(void) {
    uint8_t r, g, b;
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_PAYLOAD, derive(LED_MODE_PAYLOAD, true, 0x12, 0x34, 0x56, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0x12, r); TEST_ASSERT_EQUAL_UINT8(0x34, g); TEST_ASSERT_EQUAL_UINT8(0x56, b);
    // no alert.led: dark, whatever the direction says
    r = g = b = 9;
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, derive(LED_MODE_PAYLOAD, false, 0, 0, 0, true, 1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
    // an explicit black is a colour too (source payload, dark pins)
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_PAYLOAD, derive(LED_MODE_PAYLOAD, true, 0, 0, 0, false, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
}

void test_derive_sign_table(void) {
    uint8_t r, g, b;
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_SIGN, derive(LED_MODE_SIGN, false, 0, 0, 0, true, 1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r); TEST_ASSERT_EQUAL_UINT8(255, g); TEST_ASSERT_EQUAL_UINT8(0, b);   // up = green
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_SIGN, derive(LED_MODE_SIGN, false, 0, 0, 0, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, r); TEST_ASSERT_EQUAL_UINT8(0, g); TEST_ASSERT_EQUAL_UINT8(0, b);   // down = red
    r = g = b = 9;
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, derive(LED_MODE_SIGN, false, 0, 0, 0, true, 0, &r, &g, &b));    // flat = dark
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, derive(LED_MODE_SIGN, false, 0, 0, 0, false, 1, &r, &g, &b));   // no direction = dark
    // an explicit alert.led wins over the direction
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_PAYLOAD, derive(LED_MODE_SIGN, true, 0, 0, 255, true, -1, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r); TEST_ASSERT_EQUAL_UINT8(0, g); TEST_ASSERT_EQUAL_UINT8(255, b);
}

void test_derive_held_before_first_render(void) {
    uint8_t r, g, b;
    // the held colour stands in until a render - at any mode but off
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_HELD, led_derive(LED_MODE_SIGN, false, false, 0, 0, 0, false, 0, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, r); TEST_ASSERT_EQUAL_UINT8(0, g | b);
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_HELD, led_derive(LED_MODE_PAYLOAD, false, false, 0, 0, 0, false, 0, 0, 255, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, led_derive(LED_MODE_OFF, false, false, 0, 0, 0, false, 0, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
    // nothing held: dark
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, led_derive(LED_MODE_SIGN, false, false, 0, 0, 0, false, 0, 0, 0, 0, &r, &g, &b));
    // once rendered the held colour is history, even when the render has nothing
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, led_derive(LED_MODE_SIGN, true, false, 0, 0, 0, false, 0, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
}

// The stateful wrapper: mode, held colour, last render -> the same derivation.
void test_model_state_recomputes_from_last_render(void) {
    uint8_t r, g, b;
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_OFF, led_model_mode());
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, led_model_derive(&r, &g, &b));
    led_model_set_held(0, 0, 255);
    led_model_set_mode(LED_MODE_SIGN);
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_HELD, led_model_derive(&r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, b);
    led_model_render(false, 0, 0, 0, true, -1);                 // a ticker down
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_SIGN, led_model_derive(&r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, r); TEST_ASSERT_EQUAL_UINT8(0, g | b);
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_SIGN, led_model_source());
    led_model_set_mode(LED_MODE_OFF);                            // the setting changes: dark at once
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, led_model_derive(&r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
    led_model_set_mode(LED_MODE_SIGN);                           // ... and back: the last render is still there
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_SIGN, led_model_derive(&r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, r);
    led_model_render(false, 0, 0, 0, false, 0);                  // a text payload: nothing to show
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_OFF, led_model_derive(&r, &g, &b));
    led_model_set_mode(LED_MODE_PAYLOAD);
    led_model_render(true, 1, 2, 3, true, 1);
    TEST_ASSERT_EQUAL_UINT8(LED_SRC_PAYLOAD, led_model_derive(&r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(1, r); TEST_ASSERT_EQUAL_UINT8(2, g); TEST_ASSERT_EQUAL_UINT8(3, b);
}

// --- the setting words and the migration of the old keys ---------------------------

void test_mode_strings_and_aliases(void) {
    TEST_ASSERT_EQUAL_STRING("off", led_mode_str(LED_MODE_OFF));
    TEST_ASSERT_EQUAL_STRING("payload", led_mode_str(LED_MODE_PAYLOAD));
    TEST_ASSERT_EQUAL_STRING("sign", led_mode_str(LED_MODE_SIGN));
    TEST_ASSERT_EQUAL_STRING("off", led_mode_str(200));
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_SIGN, led_mode_parse("sign"));
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_PAYLOAD, led_mode_parse("payload"));
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_OFF, led_mode_parse("off"));
    // the former led_rule: off -> off (honest off, not payload), sign -> sign
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_OFF, led_mode_parse("off"));
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_SIGN, led_mode_parse("sign"));
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_OFF, led_mode_parse("alert"));
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_OFF, led_mode_parse(NULL));
    TEST_ASSERT_TRUE(led_mode_valid("off")); TEST_ASSERT_TRUE(led_mode_valid("payload")); TEST_ASSERT_TRUE(led_mode_valid("sign"));
    TEST_ASSERT_FALSE(led_mode_valid("hold")); TEST_ASSERT_FALSE(led_mode_valid("")); TEST_ASSERT_FALSE(led_mode_valid(NULL));
}

void test_sleep_strings_and_aliases(void) {
    TEST_ASSERT_EQUAL_STRING("off", led_sleep_str(LED_SLEEP_OFF));
    TEST_ASSERT_EQUAL_STRING("keep", led_sleep_str(LED_SLEEP_KEEP));
    TEST_ASSERT_EQUAL_STRING("off", led_sleep_str(77));
    TEST_ASSERT_EQUAL_UINT8(LED_SLEEP_KEEP, led_sleep_parse("keep"));
    TEST_ASSERT_EQUAL_UINT8(LED_SLEEP_KEEP, led_sleep_parse("hold"));   // the former led_battery word
    TEST_ASSERT_EQUAL_UINT8(LED_SLEEP_OFF, led_sleep_parse("off"));
    TEST_ASSERT_EQUAL_UINT8(LED_SLEEP_OFF, led_sleep_parse("always"));
    TEST_ASSERT_EQUAL_UINT8(LED_SLEEP_OFF, led_sleep_parse(NULL));
    TEST_ASSERT_TRUE(led_sleep_valid("off")); TEST_ASSERT_TRUE(led_sleep_valid("keep"));
    TEST_ASSERT_FALSE(led_sleep_valid("hold"));   // accepted through the alias key only
    TEST_ASSERT_FALSE(led_sleep_valid(NULL));
}

void test_source_strings(void) {
    TEST_ASSERT_EQUAL_STRING("off", led_source_str(LED_SRC_OFF));
    TEST_ASSERT_EQUAL_STRING("payload", led_source_str(LED_SRC_PAYLOAD));
    TEST_ASSERT_EQUAL_STRING("sign", led_source_str(LED_SRC_SIGN));
    TEST_ASSERT_EQUAL_STRING("held", led_source_str(LED_SRC_HELD));
    TEST_ASSERT_EQUAL_STRING("off", led_source_str(99));
}

// --- the LED through deep sleep ------------------------------------------------------

void test_sleep_hold_rule(void) {
    uint8_t r, g, b;
    // off (the default): dark whatever the level and the colour
    TEST_ASSERT_FALSE(led_sleep_hold(LED_SLEEP_OFF, true, 100, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
    // keep: the derived colour, thresholded per channel, from 30 % up
    TEST_ASSERT_TRUE(led_sleep_hold(LED_SLEEP_KEEP, true, 100, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(255, r); TEST_ASSERT_EQUAL_UINT8(0, g); TEST_ASSERT_EQUAL_UINT8(0, b);
    TEST_ASSERT_TRUE(led_sleep_hold(LED_SLEEP_KEEP, true, LED_SLEEP_MIN_PCT, 0, 200, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r); TEST_ASSERT_EQUAL_UINT8(255, g); TEST_ASSERT_EQUAL_UINT8(0, b);
    TEST_ASSERT_TRUE(led_sleep_hold(LED_SLEEP_KEEP, true, 50, 0x80, 0x7F, 0xFF, &r, &g, &b));   // >= 128 = on
    TEST_ASSERT_EQUAL_UINT8(255, r); TEST_ASSERT_EQUAL_UINT8(0, g); TEST_ASSERT_EQUAL_UINT8(255, b);
    // below the floor, an unknown level, or nothing to keep: dark
    TEST_ASSERT_FALSE(led_sleep_hold(LED_SLEEP_KEEP, true, LED_SLEEP_MIN_PCT - 1, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
    TEST_ASSERT_FALSE(led_sleep_hold(LED_SLEEP_KEEP, false, 100, 255, 0, 0, &r, &g, &b));
    TEST_ASSERT_FALSE(led_sleep_hold(LED_SLEEP_KEEP, true, 100, 0, 0, 0, &r, &g, &b));
    TEST_ASSERT_FALSE(led_sleep_hold(LED_SLEEP_KEEP, true, 100, 10, 100, 127, &r, &g, &b));    // too dim on every channel
    TEST_ASSERT_EQUAL_UINT8(0, r | g | b);
}

// The end-of-wake success blink: green unless the sign mode coloured the LED.
void test_success_blink(void) {
    TEST_ASSERT_TRUE(led_success_blink(LED_SRC_OFF));
    TEST_ASSERT_TRUE(led_success_blink(LED_SRC_PAYLOAD));   // a payload colour: blink as before
    TEST_ASSERT_TRUE(led_success_blink(LED_SRC_HELD));
    TEST_ASSERT_FALSE(led_success_blink(LED_SRC_SIGN));     // red / green: the colour is the news
}

// The brightness scale (payload alert.brightness, device led_brightness): linear, rounded.
void test_scale(void) {
    TEST_ASSERT_EQUAL_UINT8(255, led_scale(255, 255));
    TEST_ASSERT_EQUAL_UINT8(0, led_scale(255, 0));
    TEST_ASSERT_EQUAL_UINT8(0, led_scale(0, 255));
    TEST_ASSERT_EQUAL_UINT8(128, led_scale(255, 128));
    TEST_ASSERT_EQUAL_UINT8(64, led_scale(128, 128));     // 64.25 -> 64
    TEST_ASSERT_EQUAL_UINT8(1, led_scale(1, 255));        // the smallest step survives at full brightness
    TEST_ASSERT_EQUAL_UINT8(0, led_scale(1, 127));        // 0.498 -> 0
    TEST_ASSERT_EQUAL_UINT8(255, LED_BRIGHTNESS_DEFAULT);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_derive_off_ignores_everything);
    RUN_TEST(test_derive_payload_mode);
    RUN_TEST(test_derive_sign_table);
    RUN_TEST(test_derive_held_before_first_render);
    RUN_TEST(test_model_state_recomputes_from_last_render);
    RUN_TEST(test_mode_strings_and_aliases);
    RUN_TEST(test_sleep_strings_and_aliases);
    RUN_TEST(test_source_strings);
    RUN_TEST(test_sleep_hold_rule);
    RUN_TEST(test_success_blink);
    RUN_TEST(test_scale);
    return UNITY_END();
}
