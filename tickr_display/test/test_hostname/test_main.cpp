// Unity tests for the hostname builder (src/logic/hostname.cpp): RFC 1123
// label from the user's device name + "-XXXXXX" from the STA MAC, at most
// HOSTNAME_MAX_LEN characters.
#include <unity.h>
#include <string.h>
#include "logic/hostname.h"

static const uint8_t MAC[6] = { 0x02, 0x00, 0x00, 0xA1, 0xB2, 0xC3 };

void setUp(void) {}
void tearDown(void) {}

static const char* build(const char* name) {
    static char out[HOSTNAME_MAX_LEN + 1];
    size_t n = hostname_build(name, MAC, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(strlen(out), n);
    TEST_ASSERT_TRUE(n <= HOSTNAME_MAX_LEN);
    return out;
}

void test_already_valid(void) {
    TEST_ASSERT_EQUAL_STRING("shelf-left-A1B2C3", build("Shelf-Left"));
    TEST_ASSERT_EQUAL_STRING("kitchen-A1B2C3", build("kitchen"));
    TEST_ASSERT_EQUAL_STRING("a1-A1B2C3", build("a1"));
}

void test_empty_and_null_fall_back(void) {
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", build(""));
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", build(nullptr));
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", build("---"));
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", build("   "));
}

void test_cyrillic_is_dropped(void) {
    TEST_ASSERT_EQUAL_STRING("tickr-A1B2C3", build("Полка слева"));
    TEST_ASSERT_EQUAL_STRING("shelf-2-A1B2C3", build("Полка Shelf 2"));   // the dropped word leaves no dash
    TEST_ASSERT_EQUAL_STRING("shelf-A1B2C3", build("Shelf Полка"));
}

void test_spaces_underscores_and_punctuation(void) {
    TEST_ASSERT_EQUAL_STRING("living-room-A1B2C3", build("Living Room"));
    TEST_ASSERT_EQUAL_STRING("living-room-A1B2C3", build("living_room"));
    TEST_ASSERT_EQUAL_STRING("living-room-A1B2C3", build("Living _ - Room"));
    TEST_ASSERT_EQUAL_STRING("btcusd-A1B2C3", build("BTC/USD!"));
    TEST_ASSERT_EQUAL_STRING("ab-A1B2C3", build("a.b"));
}

void test_edge_dashes_collapse(void) {
    TEST_ASSERT_EQUAL_STRING("shelf-left-A1B2C3", build("--Shelf--Left--"));
    TEST_ASSERT_EQUAL_STRING("x-A1B2C3", build(" -x- "));
}

void test_long_name_is_cut_to_fit_the_suffix(void) {
    const char* h = build("Very Long Device Name For The Shelf In The Living Room");
    TEST_ASSERT_EQUAL_UINT32(HOSTNAME_MAX_LEN, strlen(h));
    TEST_ASSERT_EQUAL_STRING("very-long-device-name-for-A1B2C3", h);   // 25 + 7
    // a cut that would end the label in '-' drops the dash instead
    h = build("abcdefghijklmnopqrstuvwx yz");           // 24 chars, then a space at position 25
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwx-A1B2C3", h);
    TEST_ASSERT_TRUE(strlen(h) <= HOSTNAME_MAX_LEN);
}

void test_sanitize_alone(void) {
    char out[8];
    TEST_ASSERT_EQUAL_UINT32(3, hostname_sanitize("A B", out, sizeof(out), 7));
    TEST_ASSERT_EQUAL_STRING("a-b", out);
    TEST_ASSERT_EQUAL_UINT32(4, hostname_sanitize("abcdefgh", out, sizeof(out), 4));   // max_len wins
    TEST_ASSERT_EQUAL_STRING("abcd", out);
    TEST_ASSERT_EQUAL_UINT32(7, hostname_sanitize("abcdefgh", out, sizeof(out), 100)); // buffer wins
    TEST_ASSERT_EQUAL_STRING("abcdefg", out);
    TEST_ASSERT_EQUAL_UINT32(0, hostname_sanitize("_-_", out, sizeof(out), 7));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_already_valid);
    RUN_TEST(test_empty_and_null_fall_back);
    RUN_TEST(test_cyrillic_is_dropped);
    RUN_TEST(test_spaces_underscores_and_punctuation);
    RUN_TEST(test_edge_dashes_collapse);
    RUN_TEST(test_long_name_is_cut_to_fit_the_suffix);
    RUN_TEST(test_sanitize_alone);
    return UNITY_END();
}
