// Unity tests for the 401 challenge policy (src/logic/auth_policy.h):
// the Basic challenge goes only to navigations (Accept: text/html), never to
// fetch()/XHR/curl ("*/*"), so the pages never see the browser's prompt.
#include <unity.h>
#include "logic/auth_policy.h"

void test_navigation_gets_challenge() {
    // Chrome / Firefox / Safari address-bar and link navigations
    TEST_ASSERT_TRUE(auth_challenge_wanted("text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"));
    TEST_ASSERT_TRUE(auth_challenge_wanted("text/html;q=0.9"));
    TEST_ASSERT_TRUE(auth_challenge_wanted("text/html"));
}

void test_xhr_and_scripts_get_bare_401() {
    TEST_ASSERT_FALSE(auth_challenge_wanted("*/*"));                 // fetch, XHR, curl default
    TEST_ASSERT_FALSE(auth_challenge_wanted("application/json"));
    TEST_ASSERT_FALSE(auth_challenge_wanted("application/json, text/plain, */*"));
    TEST_ASSERT_FALSE(auth_challenge_wanted("image/avif,image/webp,*/*"));  // <img>
    TEST_ASSERT_FALSE(auth_challenge_wanted("text/plain"));
}

void test_missing_header_gets_bare_401() {
    TEST_ASSERT_FALSE(auth_challenge_wanted(""));
    TEST_ASSERT_FALSE(auth_challenge_wanted(nullptr));
}

void setUp() {}
void tearDown() {}

int runUnityTests() {
    UNITY_BEGIN();
    RUN_TEST(test_navigation_gets_challenge);
    RUN_TEST(test_xhr_and_scripts_get_bare_401);
    RUN_TEST(test_missing_header_gets_bare_401);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
    delay(2000);
    runUnityTests();
}
void loop() {}
#else
int main() { return runUnityTests(); }
#endif
