// Sanity tests for the host (native) test environment.
//
// They prove that the Unity runner works and pin down how ArduinoJson (with the
// same configuration as the firmware) treats the payload documented in README.md.
// Real module tests live next to this one: test/test_<module>/test_main.cpp.

#include <unity.h>
#include <ArduinoJson.h>

#include <cstring>

// Same capacity as renderer_process_payload() uses on the device.
static constexpr size_t kPayloadDocCapacity = 1024;

// README "Screen Update" example, as it must be sent on the wire (strict JSON).
static const char kReadmePayload[] =
    "{"
    "  \"title\": \"Bitcoin\","
    "  \"value\": \"$95,240\","
    "  \"alert\": {"
    "    \"led\": \"00FF00\","
    "    \"sound\": \"Mario:d=4,o=5,b=100:32p,16e6,16e6,16p,16e6\","
    "    \"volume\": 128"
    "  }"
    "}";

// README example verbatim, including the explanatory `//` comments.
static const char kReadmePayloadWithComments[] =
    "{\n"
    "  \"title\": \"Bitcoin\",\n"
    "  \"value\": \"$95,240\",\n"
    "  \"alert\": {\n"
    "    \"led\": \"00FF00\",       // Hex color (RRGGBB)\n"
    "    \"sound\": \"Mario:...\",  // RTTTL string or preset name\n"
    "    \"volume\": 128          // 0-255\n"
    "  }\n"
    "}\n";

void setUp() {}
void tearDown() {}

static void test_unity_runner_works() {
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
    TEST_ASSERT_EQUAL_STRING("tickr", "tickr");
}

static void test_readme_payload_parses() {
    StaticJsonDocument<kPayloadDocCapacity> doc;
    DeserializationError err = deserializeJson(doc, kReadmePayload);
    TEST_ASSERT_FALSE_MESSAGE(err, err.c_str());

    TEST_ASSERT_TRUE(doc.containsKey("title"));
    TEST_ASSERT_TRUE(doc.containsKey("value"));
    TEST_ASSERT_EQUAL_STRING("Bitcoin", doc["title"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("$95,240", doc["value"].as<const char*>());

    TEST_ASSERT_TRUE(doc.containsKey("alert"));
    JsonObject alert = doc["alert"];
    TEST_ASSERT_EQUAL_STRING("00FF00", alert["led"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(128, alert["volume"].as<int>());
    TEST_ASSERT_TRUE(strlen(alert["sound"].as<const char*>()) > 10);  // treated as RTTTL

    // The LED colour is parsed with strtol(hex, NULL, 16) in the renderer.
    long rgb = strtol(alert["led"].as<const char*>(), nullptr, 16);
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, rgb);
}

// Documented behaviour: ArduinoJson is built with comments disabled
// (ARDUINOJSON_ENABLE_COMMENTS defaults to 0), so the README example copied
// verbatim with its `//` annotations is rejected by the device.
static void test_readme_payload_with_comments_is_rejected() {
    StaticJsonDocument<kPayloadDocCapacity> doc;
    DeserializationError err = deserializeJson(doc, kReadmePayloadWithComments);
    TEST_ASSERT_TRUE_MESSAGE(err, "payload with // comments unexpectedly parsed");
    TEST_ASSERT_EQUAL(DeserializationError::InvalidInput, err.code());
}

static void test_payload_without_alert_has_no_alert_key() {
    StaticJsonDocument<kPayloadDocCapacity> doc;
    DeserializationError err = deserializeJson(doc, "{\"title\":\"CPU\",\"value\":\"42 %\"}");
    TEST_ASSERT_FALSE_MESSAGE(err, err.c_str());
    TEST_ASSERT_FALSE(doc.containsKey("alert"));
    TEST_ASSERT_TRUE(doc.containsKey("title") && doc.containsKey("value"));
}

static void test_payload_missing_value_is_incomplete() {
    StaticJsonDocument<kPayloadDocCapacity> doc;
    DeserializationError err = deserializeJson(doc, "{\"title\":\"only title\"}");
    TEST_ASSERT_FALSE_MESSAGE(err, err.c_str());
    // renderer_process_payload() only redraws when both keys are present.
    TEST_ASSERT_FALSE(doc.containsKey("title") && doc.containsKey("value"));
}

static void test_invalid_json_is_rejected() {
    StaticJsonDocument<kPayloadDocCapacity> doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, "{\"title\": \"unterminated"));
    TEST_ASSERT_TRUE(deserializeJson(doc, ""));
    TEST_ASSERT_TRUE(deserializeJson(doc, "not json at all"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unity_runner_works);
    RUN_TEST(test_readme_payload_parses);
    RUN_TEST(test_readme_payload_with_comments_is_rejected);
    RUN_TEST(test_payload_without_alert_has_no_alert_key);
    RUN_TEST(test_payload_missing_value_is_incomplete);
    RUN_TEST(test_invalid_json_is_rejected);
    return UNITY_END();
}
