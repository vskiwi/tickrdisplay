// Host-side unit tests for the pure screen-payload validator (PlatformIO Unity format).
// Run with a native environment, e.g.:  pio test -e native -f test_payload
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "logic/payload.h"

static ScreenPayload P;
static char ERR[128];

static PayloadResult parse(const char* json) {
    return payload_parse(json, strlen(json), &P, ERR, sizeof(ERR));
}

void setUp(void) { memset(&P, 0, sizeof(P)); ERR[0] = 0; }
void tearDown(void) {}

void test_full_payload(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(
        "{\"title\":\"Bitcoin\",\"value\":\"$95,240\","
        "\"alert\":{\"led\":\"00FF80\",\"sound\":\"beep\",\"volume\":128}}"));
    TEST_ASSERT_TRUE(P.has_text);
    TEST_ASSERT_EQUAL_STRING("Bitcoin", P.title);
    TEST_ASSERT_EQUAL_STRING("$95,240", P.value);
    TEST_ASSERT_TRUE(P.has_led);
    TEST_ASSERT_EQUAL_UINT8(0x00, P.r);
    TEST_ASSERT_EQUAL_UINT8(0xFF, P.g);
    TEST_ASSERT_EQUAL_UINT8(0x80, P.b);
    TEST_ASSERT_TRUE(P.has_volume);
    TEST_ASSERT_EQUAL_UINT8(128, P.volume);
    TEST_ASSERT_EQUAL(SOUND_BEEP, P.sound);
    TEST_ASSERT_EQUAL_STRING("", ERR);
}

void test_numbers_as_text_and_hash_prefix(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"title\":\"CPU\",\"value\":42,\"alert\":{\"led\":\"#ff0000\"}}"));
    TEST_ASSERT_EQUAL_STRING("42", P.value);
    TEST_ASSERT_EQUAL_UINT8(255, P.r);
    TEST_ASSERT_EQUAL_UINT8(0, P.g);
}

void test_presets_and_rtttl(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"sound\":\"double_beep\"}}"));
    TEST_ASSERT_EQUAL(SOUND_DOUBLE_BEEP, P.sound);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"sound\":\"long_beep\"}}"));
    TEST_ASSERT_EQUAL(SOUND_LONG_BEEP, P.sound);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"sound\":\"none\"}}"));
    TEST_ASSERT_EQUAL(SOUND_NONE, P.sound);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"sound\":\"Nokia:d=8,o=5,b=225:e6,d6,f#,g#,c#6,b,d,e,b,a,c#,e,2a\"}}"));
    TEST_ASSERT_EQUAL(SOUND_RTTTL, P.sound);
    TEST_ASSERT_EQUAL_STRING("Nokia:d=8,o=5,b=225:e6,d6,f#,g#,c#6,b,d,e,b,a,c#,e,2a", P.rtttl);
}

void test_unknown_sound_is_ignored_not_rejected(void) {
    // The old heuristic treated any string > 10 chars as RTTTL; now it must be a valid melody.
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"sound\":\"hello world!\"}}"));
    TEST_ASSERT_EQUAL(SOUND_UNKNOWN, P.sound);
    TEST_ASSERT_TRUE(strlen(ERR) > 0);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"sound\":\"x:d=4,o=9,b=100:c\"}}"));  // scale 9
    TEST_ASSERT_EQUAL(SOUND_UNKNOWN, P.sound);
}

void test_rejects_bad_json(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_EMPTY, parse(""));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_JSON, parse("{\"title\":"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_JSON, parse("hello world!"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_NOT_OBJECT, parse("[1,2,3]"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_NOT_OBJECT, parse("\"str\""));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_NOTHING_TO_DO, parse("{\"foo\":1}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_NOTHING_TO_DO, parse("{}"));
}

void test_rejects_bad_fields(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"title\":{\"x\":1}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":\"beep\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"led\":\"GGGGGG\"}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"led\":\"FFF\"}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"led\":123}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"volume\":300}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"volume\":-1}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"volume\":\"loud\"}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"sound\":5}}"));
    // null values are treated as absent -> nothing to do
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_NOTHING_TO_DO, parse("{\"title\":null,\"value\":null}"));
}

void test_length_limits(void) {
    char json[512];
    char longtitle[PAYLOAD_TITLE_MAX + 8];
    memset(longtitle, 'A', sizeof(longtitle) - 1);
    longtitle[sizeof(longtitle) - 1] = 0;
    snprintf(json, sizeof(json), "{\"title\":\"%s\"}", longtitle);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse(json));

    longtitle[PAYLOAD_TITLE_MAX - 1] = 0;   // exactly max-1 chars fits
    snprintf(json, sizeof(json), "{\"title\":\"%s\"}", longtitle);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(json));

    static char huge[PAYLOAD_MAX_LEN + 16];
    memset(huge, ' ', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_TOO_LARGE, payload_parse(huge, sizeof(huge) - 1, &P, ERR, sizeof(ERR)));
}

// --- ticker fields (docs/API.md "Payload format") --------------------------------

void test_ticker_fields_full(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(
        "{\"title\":\"BTC/USDT\",\"value\":\"84 000.06\",\"change\":\"+0.07%\",\"dir\":1,"
        "\"age_s\":120,\"time\":\"19:00\",\"spark\":[83950,83990.5,84001],\"alert\":{\"led\":\"00FF00\"}}"));
    TEST_ASSERT_TRUE(P.has_text);
    TEST_ASSERT_TRUE(P.has_change);
    TEST_ASSERT_EQUAL_STRING("+0.07%", P.change);
    TEST_ASSERT_TRUE(P.has_dir);
    TEST_ASSERT_EQUAL_INT(1, P.dir);
    TEST_ASSERT_TRUE(P.has_age);
    TEST_ASSERT_EQUAL_UINT32(120, P.age_s);
    TEST_ASSERT_EQUAL_STRING("19:00", P.time);
    TEST_ASSERT_EQUAL_UINT8(3, P.spark_n);
    TEST_ASSERT_EQUAL_FLOAT(83950.0f, P.spark[0]);
    TEST_ASSERT_EQUAL_FLOAT(83990.5f, P.spark[1]);
    TEST_ASSERT_TRUE(payload_is_ticker(P));
    TEST_ASSERT_TRUE(P.has_led);
    TEST_ASSERT_EQUAL_STRING("", ERR);
}

void test_plain_payload_is_not_a_ticker(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"title\":\"Temperature\",\"value\":\"21.5 C\"}"));
    TEST_ASSERT_FALSE(payload_is_ticker(P));
    TEST_ASSERT_FALSE(P.has_change);
    TEST_ASSERT_FALSE(P.has_dir);
    TEST_ASSERT_FALSE(P.has_age);
    TEST_ASSERT_EQUAL_STRING("", P.time);
    TEST_ASSERT_EQUAL_UINT8(0, P.spark_n);
    // dir / age_s / time alone are recognised but do not select the ticker layout
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"dir\":-1}"));
    TEST_ASSERT_FALSE(payload_is_ticker(P));
    TEST_ASSERT_TRUE(P.has_dir);
    TEST_ASSERT_EQUAL_INT(-1, P.dir);
    TEST_ASSERT_FALSE(P.has_text);
}

void test_dir_derived_and_string_forms(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"value\":\"1\",\"change\":\"-1.23%\"}"));
    TEST_ASSERT_TRUE(P.has_dir);
    TEST_ASSERT_EQUAL_INT(-1, P.dir);
    TEST_ASSERT_TRUE(payload_is_ticker(P));
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"change\":\"0.00%\"}"));
    TEST_ASSERT_EQUAL_INT(0, P.dir);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"change\":\"+5\",\"dir\":\"down\"}"));   // explicit dir wins
    TEST_ASSERT_EQUAL_INT(-1, P.dir);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"change\":\"-5\",\"dir\":\"up\"}"));
    TEST_ASSERT_EQUAL_INT(1, P.dir);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"dir\":\"flat\"}"));
    TEST_ASSERT_EQUAL_INT(0, P.dir);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"dir\":-7}"));    // any negative / positive integer
    TEST_ASSERT_EQUAL_INT(-1, P.dir);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"dir\":3}"));
    TEST_ASSERT_EQUAL_INT(1, P.dir);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"dir\":\"sideways\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"dir\":true}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"dir\":1.5}"));
}

void test_change_edge_cases(void) {
    // empty string: recognised, but no change line and no direction, not a ticker
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"value\":\"x\",\"change\":\"\"}"));
    TEST_ASSERT_FALSE(P.has_change);
    TEST_ASSERT_FALSE(P.has_dir);
    TEST_ASSERT_FALSE(payload_is_ticker(P));
    // a number is rendered as text and gives a direction
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"change\":-2.5}"));
    TEST_ASSERT_EQUAL_STRING("-2.5", P.change);
    TEST_ASSERT_EQUAL_INT(-1, P.dir);
    // wrong type / too long
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"change\":{\"pct\":1}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"change\":\"+123456789.123456%\"}"));   // 17 chars > 15
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"change\":\"+12345678.1234%\"}"));            // 15 chars fits
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"time\":\"a much too long time\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"time\":[1]}"));
}

void test_age_edge_cases(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"age_s\":0}"));
    TEST_ASSERT_TRUE(P.has_age);
    TEST_ASSERT_EQUAL_UINT32(0, P.age_s);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"age_s\":12.9}"));      // truncated
    TEST_ASSERT_EQUAL_UINT32(12, P.age_s);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"age_s\":1e12}"));      // huge: clamped below TICKER_AGE_UNKNOWN
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEu, P.age_s);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"age_s\":-5}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"age_s\":\"120\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"value\":\"x\",\"age_s\":null}"));   // null = absent
    TEST_ASSERT_FALSE(P.has_age);
}

void test_spark_edge_cases(void) {
    // more than 48 points: the first 48 are kept, the rest ignored
    char json[1024];
    int n = snprintf(json, sizeof(json), "{\"spark\":[");
    for (int i = 0; i < 60; i++) n += snprintf(json + n, sizeof(json) - n, "%s%d", i ? "," : "", i);
    snprintf(json + n, sizeof(json) - n, "]}");
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(json));
    TEST_ASSERT_EQUAL_UINT8(TICKER_SPARK_MAX, P.spark_n);
    TEST_ASSERT_EQUAL_FLOAT(47.0f, P.spark[TICKER_SPARK_MAX - 1]);
    TEST_ASSERT_TRUE(payload_is_ticker(P));
    // non-numbers are skipped, an empty array is nothing to draw (and not a ticker)
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"spark\":[1,\"x\",null,2,true,3.5]}"));
    TEST_ASSERT_EQUAL_UINT8(3, P.spark_n);
    TEST_ASSERT_EQUAL_FLOAT(3.5f, P.spark[2]);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"value\":\"x\",\"spark\":[]}"));
    TEST_ASSERT_EQUAL_UINT8(0, P.spark_n);
    TEST_ASSERT_FALSE(payload_is_ticker(P));
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"spark\":[42]}"));      // a single point: kept, drawn as nothing
    TEST_ASSERT_EQUAL_UINT8(1, P.spark_n);
    // huge numbers survive as float
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"spark\":[1e30,-1e30,123456789012]}"));
    TEST_ASSERT_EQUAL_UINT8(3, P.spark_n);
    TEST_ASSERT_TRUE(P.spark[0] > 1e29f && P.spark[1] < -1e29f);
    // wrong type
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"spark\":\"1,2,3\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"spark\":{\"a\":1}}"));
}

void test_nodered_appendix_a_shape(void) {
    // docs/TICKERS.md Appendix A: what the Node-RED flow sends (dir as an integer, age_s 0)
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(
        "{\"title\":\"BTC/USDT  +0.07%\",\"value\":\"84\\u2009000.06\",\"change\":\"+0.07%\",\"dir\":1,\"age_s\":0,"
        "\"alert\":{\"led\":\"00FF00\",\"sound\":\"none\"}}"));
    TEST_ASSERT_TRUE(payload_is_ticker(P));
    TEST_ASSERT_EQUAL_INT(1, P.dir);
    TEST_ASSERT_EQUAL_UINT32(0, P.age_s);
    TEST_ASSERT_EQUAL(SOUND_NONE, P.sound);
    TEST_ASSERT_TRUE(P.has_led);
    // the failure branch of the same flow
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"title\":\"BTCUSDT\",\"value\":\"no data\",\"alert\":{\"led\":\"000000\"}}"));
    TEST_ASSERT_FALSE(payload_is_ticker(P));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_ticker_fields_full);
    RUN_TEST(test_plain_payload_is_not_a_ticker);
    RUN_TEST(test_dir_derived_and_string_forms);
    RUN_TEST(test_change_edge_cases);
    RUN_TEST(test_age_edge_cases);
    RUN_TEST(test_spark_edge_cases);
    RUN_TEST(test_nodered_appendix_a_shape);
    RUN_TEST(test_full_payload);
    RUN_TEST(test_numbers_as_text_and_hash_prefix);
    RUN_TEST(test_presets_and_rtttl);
    RUN_TEST(test_unknown_sound_is_ignored_not_rejected);
    RUN_TEST(test_rejects_bad_json);
    RUN_TEST(test_rejects_bad_fields);
    RUN_TEST(test_length_limits);
    return UNITY_END();
}
