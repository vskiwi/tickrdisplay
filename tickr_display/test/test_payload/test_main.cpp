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
        "\"alert\":{\"led\":\"00FF80\",\"brightness\":64,\"sound\":\"beep\",\"volume\":128}}"));
    TEST_ASSERT_TRUE(P.has_text);
    TEST_ASSERT_EQUAL_STRING("Bitcoin", P.title);
    TEST_ASSERT_EQUAL_STRING("$95,240", P.value);
    TEST_ASSERT_TRUE(P.has_led);
    TEST_ASSERT_EQUAL_UINT8(0x00, P.r);
    TEST_ASSERT_EQUAL_UINT8(0xFF, P.g);
    TEST_ASSERT_EQUAL_UINT8(0x80, P.b);
    TEST_ASSERT_TRUE(P.has_brightness);
    TEST_ASSERT_EQUAL_UINT8(64, P.brightness);
    TEST_ASSERT_TRUE(P.has_volume);
    TEST_ASSERT_EQUAL_UINT8(128, P.volume);
    TEST_ASSERT_EQUAL(SOUND_BEEP, P.sound);
    TEST_ASSERT_EQUAL_STRING("", ERR);
}

void test_brightness(void) {
    // Brightness alone is a recognised field (sticky, like volume): no colour needed.
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"brightness\":0}}"));
    TEST_ASSERT_TRUE(P.has_brightness);
    TEST_ASSERT_EQUAL_UINT8(0, P.brightness);
    TEST_ASSERT_FALSE(P.has_led);
    TEST_ASSERT_EQUAL_STRING("", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"brightness\":255}}"));
    TEST_ASSERT_EQUAL_UINT8(255, P.brightness);
    // Absent -> not set (the device keeps its current brightness).
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"alert\":{\"led\":\"FF0000\"}}"));
    TEST_ASSERT_FALSE(P.has_brightness);
    // Rejected like volume: integer 0..255 only, and the whole payload fails.
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"brightness\":300}}"));
    TEST_ASSERT_EQUAL_STRING("alert.brightness: out of range 0..255", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"alert\":{\"brightness\":-1}}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"title\":\"x\",\"alert\":{\"brightness\":\"dim\"}}"));
    TEST_ASSERT_EQUAL_STRING("alert.brightness: must be an integer 0..255", ERR);
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

void test_short_field(void) {
    // the badge name travels with the ticker fields; absent = ""
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"value\":\"84 015\",\"change\":\"+0.1%\",\"short\":\"BTC\"}"));
    TEST_ASSERT_EQUAL_STRING("BTC", P.short_label);
    TEST_ASSERT_TRUE(payload_is_ticker(P));
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"value\":\"84 015\",\"change\":\"+0.1%\"}"));
    TEST_ASSERT_EQUAL_STRING("", P.short_label);
    // 7 chars fit, 8 do not; a number is text like the other fields
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"short\":\"BITCOIN\"}"));
    TEST_ASSERT_EQUAL_STRING("BITCOIN", P.short_label);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"short\":\"ETHEREUM\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"short\":42}"));
    TEST_ASSERT_EQUAL_STRING("42", P.short_label);
    // printable ASCII only (the badge font is Latin-1, the field is tiny)
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"short\":\"\\u20ac5\"}"));
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"short\":\"a\\tb\"}"));
    // `short` alone is a recognised field but not a ticker layout by itself
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"short\":\"X\"}"));
    TEST_ASSERT_FALSE(payload_is_ticker(P));
}

// --- tile layouts (docs/API.md "Tile layouts") ---------------------------------

void test_tiles_three_of_four_with_null(void) {
    // the pump-tiles case: three coins, the fourth cell empty, a style on one tile
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(
        "{\"layout\":\"2x2\",\"tiles\":["
        "{\"title\":\"BTC\",\"value\":\"80000\",\"change\":\"+15%\",\"style\":{\"invert\":true,\"size\":\"l\"}},"
        "{\"title\":\"ETH\",\"value\":\"4100\",\"change\":\"+12.3%\",\"dir\":1},"
        "{\"title\":\"SOL\",\"value\":\"185.20\",\"change\":\"+10%\",\"style\":{\"look\":\"row\"}},"
        "null],\"age_s\":20,\"alert\":{\"led\":\"00FF00\"}}"));
    TEST_ASSERT_TRUE(payload_is_tiles(P));
    TEST_ASSERT_FALSE(payload_is_ticker(P));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, P.layout);
    TEST_ASSERT_EQUAL_UINT8(4, P.tiles_n);
    TEST_ASSERT_TRUE(P.status);
    TEST_ASSERT_TRUE(P.tiles[0].has);
    TEST_ASSERT_EQUAL_STRING("BTC", P.tiles[0].title);
    TEST_ASSERT_EQUAL_STRING("80000", P.tiles[0].value);
    TEST_ASSERT_EQUAL_STRING("+15%", P.tiles[0].change);
    TEST_ASSERT_TRUE(P.tiles[0].has_dir);                   // derived from the sign
    TEST_ASSERT_EQUAL_INT(1, P.tiles[0].dir);
    TEST_ASSERT_TRUE(P.tiles[0].style.invert);
    TEST_ASSERT_EQUAL_UINT8(TILE_SIZE_L, P.tiles[0].style.size);
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_STACK, P.tiles[0].style.look);
    TEST_ASSERT_FALSE(P.tiles[1].style.invert);
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_ROW, P.tiles[2].style.look);
    TEST_ASSERT_FALSE(P.tiles[3].has);
    TEST_ASSERT_TRUE(P.has_age);
    TEST_ASSERT_EQUAL_UINT32(20, P.age_s);
    TEST_ASSERT_TRUE(P.has_led);
    TEST_ASSERT_EQUAL_STRING("", ERR);
    // the frame hal_display draws
    TilesFrame f;
    payload_tiles_frame(P, &f);
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, f.layout);
    TEST_ASSERT_EQUAL_UINT8(4, f.n);
    TEST_ASSERT_TRUE(f.status);
    TEST_ASSERT_EQUAL_UINT32(20, f.age_s);
    TEST_ASSERT_EQUAL_STRING("SOL", f.t[2].title);
    TEST_ASSERT_FALSE(f.t[3].has);
}

void test_tiles_empty_forms_and_defaults(void) {
    // {} and a short array are empty cells; the layout defaults by the count
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"title\":\"BTC\",\"value\":\"1\"},{}]}"));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_1X2, P.layout);
    TEST_ASSERT_EQUAL_UINT8(2, P.tiles_n);
    TEST_ASSERT_TRUE(P.tiles[0].has);
    TEST_ASSERT_FALSE(P.tiles[1].has);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"value\":\"42\"}]}"));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_1X1, P.layout);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"title\":\"A\"},{\"title\":\"B\"},{\"title\":\"C\"}]}"));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, P.layout);
    TEST_ASSERT_EQUAL_UINT8(3, P.tiles_n);
    TEST_ASSERT_FALSE(P.tiles[3].has);
    // all empty: accepted, a blank grid
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[null,null,null,null]}"));
    TEST_ASSERT_TRUE(payload_is_tiles(P));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, P.layout);
    for (int i = 0; i < 4; i++) TEST_ASSERT_FALSE(P.tiles[i].has);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[]}"));
    TEST_ASSERT_TRUE(payload_is_tiles(P));
    TEST_ASSERT_EQUAL_UINT8(0, P.tiles_n);
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, P.layout);
    // a tile with unknown keys only is empty too; a change alone fills a tile
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"text\":\"x\"},{\"change\":\"-2%\"}]}"));
    TEST_ASSERT_FALSE(P.tiles[0].has);
    TEST_ASSERT_TRUE(P.tiles[1].has);
    TEST_ASSERT_EQUAL_INT(-1, P.tiles[1].dir);
    // a status flag and a smaller preset
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"layout\":\"2x1\",\"status\":false,\"tiles\":[{\"title\":\"A\",\"value\":\"1\"}]}"));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X1, P.layout);
    TEST_ASSERT_FALSE(P.status);
    TEST_ASSERT_EQUAL_UINT8(1, P.tiles_n);
}

void test_tiles_root_style_and_overrides(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(
        "{\"style\":{\"look\":\"ticker\",\"invert\":true,\"size\":\"m\",\"colour\":\"red\"},"
        "\"tiles\":[{\"title\":\"A\",\"value\":\"1\"},{\"title\":\"B\",\"value\":\"2\",\"style\":{\"invert\":false}},null]}"));
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_TICKER, P.tiles[0].style.look);
    TEST_ASSERT_TRUE(P.tiles[0].style.invert);
    TEST_ASSERT_EQUAL_UINT8(TILE_SIZE_M, P.tiles[0].style.size);
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_TICKER, P.tiles[1].style.look);
    TEST_ASSERT_FALSE(P.tiles[1].style.invert);                       // the tile's key wins
    TEST_ASSERT_EQUAL_UINT8(TILE_SIZE_M, P.tiles[1].style.size);
    TEST_ASSERT_TRUE(P.tiles[2].style.invert);                        // the empty cell carries the root style (unused)
    TEST_ASSERT_TRUE(P.tiles[3].style.invert);                        // the omitted cell too
    TEST_ASSERT_EQUAL_STRING("", ERR);
    // a root style alone is a recognised field (no tiles: nothing drawn, no error)
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"style\":{\"invert\":true}}"));
    TEST_ASSERT_FALSE(payload_is_tiles(P));
}

void test_tiles_rejections(void) {
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":{\"title\":\"BTC\"}}"));
    TEST_ASSERT_EQUAL_STRING("tiles: must be an array of up to 4 tiles", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{},{},{},{},{}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles: more than 4", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"layout\":\"1x2\",\"tiles\":[{},{},{}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles: more than 2 for layout 1x2", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"title\":\"A\"},{\"title\":\"B\"},\"C\"]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[2]: must be an object or null", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{},{},{\"title\":\"ABCDEFGHIJKLMNOP\"}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[2].title: too long", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"title\":\"ABCDEFGHIJKLMNO\"}]}"));   // 15 chars fit
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"title\":\"\\u20ac\"}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[0].title: printable ASCII only", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"value\":\"123456789012345678901234\"}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[0].value: too long", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"change\":\"+12345678901.23%\"}]}"));   // 16 chars
    TEST_ASSERT_EQUAL_STRING("tiles[0].change: too long", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"title\":true}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[0].title: must be a string", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{},{\"title\":\"A\",\"dir\":\"rising\"}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[1].dir: expected -1|0|1 or up|down|flat", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"title\":\"A\",\"dir\":1.5}]}"));
    // layout
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"layout\":\"grid\",\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("layout: expected single|1x1|1x2|2x1|2x2", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"layout\":2,\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("layout: expected single|1x1|1x2|2x1|2x2", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"layout\":\"2x2\",\"title\":\"x\"}"));
    TEST_ASSERT_EQUAL_STRING("layout: 2x2 needs tiles", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"layout\":\"single\",\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("layout: single cannot carry tiles", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"layout\":\"single\",\"title\":\"plain\"}"));   // declarative, harmless
    TEST_ASSERT_FALSE(payload_is_tiles(P));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_SINGLE, P.layout);
    // style
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"style\":\"bold\",\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("style: must be an object", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"style\":{\"look\":\"big\"},\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("style.look: expected stack|row|ticker", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"style\":{\"size\":\"xl\"},\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("style.size: expected auto|l|m|s", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"style\":{\"invert\":\"yes\"},\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("style.invert: must be true or false", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"title\":\"A\",\"style\":{\"look\":1}}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[0].style.look: expected stack|row|ticker", ERR);
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"tiles\":[{\"title\":\"A\",\"style\":[]}]}"));
    TEST_ASSERT_EQUAL_STRING("tiles[0].style: must be an object", ERR);
    // status
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_FIELD, parse("{\"status\":\"off\",\"tiles\":[{}]}"));
    TEST_ASSERT_EQUAL_STRING("status: must be true or false", ERR);
    // the recognised list names the new keys
    TEST_ASSERT_EQUAL(PAYLOAD_ERR_NOTHING_TO_DO, parse("{\"cells\":[]}"));
    TEST_ASSERT_NOT_NULL(strstr(ERR, "tiles"));
}

void test_tiles_ignore_root_fields_with_warning(void) {
    // the root value / ticker fields are ignored (a warning), the title is kept, age_s counts
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse(
        "{\"title\":\"Movers\",\"value\":\"BTC +15% | ETH +12%\",\"change\":\"+15%\",\"dir\":1,\"short\":\"BTC\","
        "\"spark\":[1,2,3],\"time\":\"19:00\",\"age_s\":5,\"tiles\":[{\"title\":\"BTC\",\"value\":\"80000\"}]}"));
    TEST_ASSERT_TRUE(payload_is_tiles(P));
    TEST_ASSERT_EQUAL_STRING("value ignored: tiles present", ERR);
    TEST_ASSERT_TRUE(P.has_text);
    TEST_ASSERT_EQUAL_STRING("Movers", P.title);
    TEST_ASSERT_EQUAL_STRING("", P.value);
    TEST_ASSERT_FALSE(P.has_change);
    TEST_ASSERT_FALSE(P.has_dir);
    TEST_ASSERT_EQUAL_UINT8(0, P.spark_n);
    TEST_ASSERT_EQUAL_STRING("", P.short_label);
    TEST_ASSERT_EQUAL_STRING("", P.time);
    TEST_ASSERT_TRUE(P.has_age);
    TEST_ASSERT_EQUAL_UINT32(5, P.age_s);
    // the warning names the first ignored key; an invalid ignored field is not even looked at
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"title\":\"A\"}],\"dir\":\"sideways\"}"));
    TEST_ASSERT_EQUAL_STRING("dir ignored: tiles present", ERR);
    // an alert.sound warning is not displaced
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"tiles\":[{\"title\":\"A\"}],\"value\":\"x\",\"alert\":{\"sound\":\"ding\"}}"));
    TEST_ASSERT_EQUAL_STRING("alert.sound ignored: unknown preset or invalid RTTTL", ERR);
    TEST_ASSERT_EQUAL(SOUND_UNKNOWN, P.sound);
}

void test_tiles_absent_keeps_single_payload_identical(void) {
    // a payload without tiles parses to the same struct as before the tiles existed:
    // every tile field is zero / default, nothing else changes
    ScreenPayload ref;
    memset(&ref, 0, sizeof(ref));
    TEST_ASSERT_EQUAL(PAYLOAD_OK, parse("{\"title\":\"Bitcoin\",\"value\":\"$95,240\",\"change\":\"+0.07%\",\"dir\":1,\"age_s\":120,"
                                        "\"spark\":[83950,83990,84001],\"short\":\"BTC\",\"time\":\"19:00\","
                                        "\"alert\":{\"led\":\"00FF00\",\"brightness\":128,\"sound\":\"double_beep\",\"volume\":128}}"));
    TEST_ASSERT_FALSE(payload_is_tiles(P));
    TEST_ASSERT_TRUE(payload_is_ticker(P));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_SINGLE, P.layout);
    TEST_ASSERT_EQUAL_UINT8(0, P.tiles_n);
    TEST_ASSERT_TRUE(P.status);                                        // the default, unused without tiles
    TEST_ASSERT_FALSE(P.style.has_look || P.style.has_size || P.style.has_invert);
    for (int i = 0; i < TILES_MAX; i++) {
        TEST_ASSERT_EQUAL_MEMORY(&ref.tiles[i], &P.tiles[i], sizeof(Tile));
    }
    TEST_ASSERT_EQUAL_STRING("Bitcoin", P.title);
    TEST_ASSERT_EQUAL_STRING("$95,240", P.value);
    TEST_ASSERT_EQUAL_STRING("+0.07%", P.change);
    TEST_ASSERT_EQUAL_STRING("BTC", P.short_label);
    TEST_ASSERT_EQUAL_STRING("19:00", P.time);
    TEST_ASSERT_EQUAL_UINT8(3, P.spark_n);
    TEST_ASSERT_EQUAL_STRING("", ERR);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tiles_three_of_four_with_null);
    RUN_TEST(test_tiles_empty_forms_and_defaults);
    RUN_TEST(test_tiles_root_style_and_overrides);
    RUN_TEST(test_tiles_rejections);
    RUN_TEST(test_tiles_ignore_root_fields_with_warning);
    RUN_TEST(test_tiles_absent_keeps_single_payload_identical);
    RUN_TEST(test_short_field);
    RUN_TEST(test_ticker_fields_full);
    RUN_TEST(test_plain_payload_is_not_a_ticker);
    RUN_TEST(test_dir_derived_and_string_forms);
    RUN_TEST(test_change_edge_cases);
    RUN_TEST(test_age_edge_cases);
    RUN_TEST(test_spark_edge_cases);
    RUN_TEST(test_nodered_appendix_a_shape);
    RUN_TEST(test_full_payload);
    RUN_TEST(test_brightness);
    RUN_TEST(test_numbers_as_text_and_hash_prefix);
    RUN_TEST(test_presets_and_rtttl);
    RUN_TEST(test_unknown_sound_is_ignored_not_rejected);
    RUN_TEST(test_rejects_bad_json);
    RUN_TEST(test_rejects_bad_fields);
    RUN_TEST(test_length_limits);
    return UNITY_END();
}
