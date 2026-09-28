// Host tests for the fleet layout validator (src/logic/layout.cpp, docs/MULTI_DEVICE.md "The shelf layout").
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "logic/layout.h"

void setUp() {}
void tearDown() {}

static bool ok(const char* json, size_t* n = nullptr) {
    char err[48] = "";
    return layout_validate(json, strlen(json), n, err, sizeof(err));
}

static const char* why(const char* json) {
    static char err[48];
    err[0] = '\0';
    layout_validate(json, strlen(json), nullptr, err, sizeof(err));
    return err;
}

static void test_minimal_and_full_documents() {
    size_t n = 99;
    TEST_ASSERT_TRUE(ok("{\"v\":1,\"updated_at\":0,\"devices\":{}}", &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_TRUE(ok("{\"v\":1,\"updated_at\":1727366400000,\"by\":\"tickr-A1B2C3\","
                        "\"devices\":{\"tickr-A1B2C3\":{\"x\":0,\"y\":0,\"name\":\"Shelf-Left\"},"
                        "\"tickr-D4E5F6\":{\"x\":1,\"y\":0}},\"future\":[1,2,3]}", &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    // updated_at may exceed 32 bits (ms since epoch) - parsed as float without ARDUINOJSON_USE_LONG_LONG
    TEST_ASSERT_TRUE(ok("{\"v\":1,\"updated_at\":1.7273664e12,\"devices\":{}}"));
}

static void test_rejects_shape_errors() {
    TEST_ASSERT_FALSE(ok(""));
    TEST_ASSERT_FALSE(ok("[]"));
    TEST_ASSERT_FALSE(ok("{\"v\":1,\"updated_at\":0,\"devices\":{"));
    TEST_ASSERT_EQUAL_STRING("v must be 1", why("{\"v\":2,\"updated_at\":0,\"devices\":{}}"));
    TEST_ASSERT_EQUAL_STRING("v must be 1", why("{\"updated_at\":0,\"devices\":{}}"));
    TEST_ASSERT_EQUAL_STRING("updated_at must be a number", why("{\"v\":1,\"updated_at\":\"now\",\"devices\":{}}"));
    TEST_ASSERT_EQUAL_STRING("updated_at must be a number", why("{\"v\":1,\"updated_at\":-5,\"devices\":{}}"));
    TEST_ASSERT_EQUAL_STRING("devices must be an object", why("{\"v\":1,\"updated_at\":0,\"devices\":[]}"));
    TEST_ASSERT_EQUAL_STRING("devices must be an object", why("{\"v\":1,\"updated_at\":0}"));
    TEST_ASSERT_EQUAL_STRING("by must be a string", why("{\"v\":1,\"updated_at\":0,\"by\":7,\"devices\":{}}"));
}

static void test_rejects_bad_entries() {
    TEST_ASSERT_EQUAL_STRING("device id must be tickr-XXXXXX", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"kitchen\":{\"x\":0,\"y\":0}}}"));
    TEST_ASSERT_EQUAL_STRING("device entry must be an object", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":[0,0]}}"));
    TEST_ASSERT_EQUAL_STRING("x/y must be 0..15", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":16,\"y\":0}}}"));
    TEST_ASSERT_EQUAL_STRING("x/y must be 0..15", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":0,\"y\":-1}}}"));
    TEST_ASSERT_EQUAL_STRING("x/y must be 0..15", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":1.5,\"y\":0}}}"));
    TEST_ASSERT_EQUAL_STRING("x/y must be 0..15", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":\"0\",\"y\":0}}}"));
    TEST_ASSERT_EQUAL_STRING("x/y must be 0..15", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"y\":0}}}"));
    TEST_ASSERT_EQUAL_STRING("name must be a string", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":0,\"y\":0,\"name\":5}}}"));
    TEST_ASSERT_EQUAL_STRING("name too long", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":0,\"y\":0,"
                                                  "\"name\":\"0123456789012345678901234567890X\"}}}"));
    TEST_ASSERT_EQUAL_STRING("name must be printable ASCII", why("{\"v\":1,\"updated_at\":0,\"devices\":{\"tickr-A1B2C3\":{\"x\":0,\"y\":0,\"name\":\"K\\u00fcche\"}}}"));
}

static void test_limits() {
    // 65 devices -> too many
    char doc[LAYOUT_MAX_LEN + 64];
    strcpy(doc, "{\"v\":1,\"updated_at\":0,\"devices\":{");
    for (int i = 0; i <= LAYOUT_MAX_DEVICES; i++) {
        char e[48];
        snprintf(e, sizeof(e), "%s\"tickr-%06X\":{\"x\":%d,\"y\":%d}", i ? "," : "", i, i % 16, (i / 16) % 16);
        strcat(doc, e);
    }
    strcat(doc, "}}");
    TEST_ASSERT_TRUE(strlen(doc) < LAYOUT_MAX_LEN);
    TEST_ASSERT_EQUAL_STRING("too many devices", why(doc));
    // Exactly the maximum is fine
    strcpy(doc, "{\"v\":1,\"updated_at\":0,\"devices\":{");
    for (int i = 0; i < LAYOUT_MAX_DEVICES; i++) {
        char e[48];
        snprintf(e, sizeof(e), "%s\"tickr-%06X\":{\"x\":%d,\"y\":%d}", i ? "," : "", i, i % 16, (i / 16) % 16);
        strcat(doc, e);
    }
    strcat(doc, "}}");
    size_t n = 0;
    TEST_ASSERT_TRUE(ok(doc, &n));
    TEST_ASSERT_EQUAL_UINT(LAYOUT_MAX_DEVICES, n);
    // Over the byte limit -> rejected before parsing
    char big[LAYOUT_MAX_LEN + 2];
    memset(big, ' ', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char err[48] = "";
    TEST_ASSERT_FALSE(layout_validate(big, sizeof(big) - 1, nullptr, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("layout too large", err);
    // Length is honoured: trailing garbage beyond len is not read
    const char* s = "{\"v\":1,\"updated_at\":0,\"devices\":{}}GARBAGE";
    TEST_ASSERT_TRUE(layout_validate(s, strlen(s) - 7, nullptr, err, sizeof(err)));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_minimal_and_full_documents);
    RUN_TEST(test_rejects_shape_errors);
    RUN_TEST(test_rejects_bad_entries);
    RUN_TEST(test_limits);
    return UNITY_END();
}
