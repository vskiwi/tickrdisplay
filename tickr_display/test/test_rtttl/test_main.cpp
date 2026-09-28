// Host-side unit tests for the pure RTTTL parser (PlatformIO Unity format).
// Run with a native environment, e.g.:  pio test -e native -f test_rtttl
#include <unity.h>
#include <string.h>
#include "logic/rtttl.h"

static const char* MARIO =
    "Mario:d=4,o=5,b=100:32p,16e6,16e6,16p,16e6,16p,16c6,16e6,16p,16g6,8p,16g";
static const char* STARWARS =
    "StarWars:d=4,o=5,b=45:32p,32f#,32f#,32f#,8b.,8f#.6,32e6,32d#6,32c#6,8b.6,16f#.6,32e6,32d#6,32c#6,8b.6,16f#.6,32e6,32d#6,32e6,8c#6";
static const char* NOKIA =
    "Nokia:d=8,o=5,b=225:e6,d6,f#,g#,c#6,b,d,e,b,a,c#,e,2a";

static uint32_t play_all(const RtttlSong* song, uint16_t* count_out) {
    RtttlIter it;
    RtttlNote n;
    uint32_t total = 0;
    uint16_t count = 0;
    rtttl_iter_init(&it, song);
    while (rtttl_iter_next(&it, &n)) {
        total += n.duration_ms;
        count++;
    }
    if (count_out) *count_out = count;
    return total;
}

void setUp(void) {}
void tearDown(void) {}

void test_valid_mario(void) {
    RtttlSong s;
    TEST_ASSERT_TRUE(rtttl_looks_like(MARIO));
    TEST_ASSERT_TRUE(rtttl_parse(MARIO, &s));
    TEST_ASSERT_EQUAL_STRING("Mario", s.name);
    TEST_ASSERT_EQUAL_UINT8(4, s.default_dur);
    TEST_ASSERT_EQUAL_UINT8(5, s.default_oct);
    TEST_ASSERT_EQUAL_UINT16(100, s.bpm);
    TEST_ASSERT_EQUAL_UINT16(12, s.note_count);
    uint16_t count;
    uint32_t total = play_all(&s, &count);
    TEST_ASSERT_EQUAL_UINT16(12, count);
    TEST_ASSERT_EQUAL_UINT32(s.total_ms, total);
    TEST_ASSERT_TRUE(total > 0 && total <= RTTTL_MAX_TOTAL_MS);
}

void test_valid_starwars_dotted_and_sharps(void) {
    RtttlSong s;
    TEST_ASSERT_TRUE(rtttl_parse(STARWARS, &s));
    TEST_ASSERT_EQUAL_UINT16(20, s.note_count);

    RtttlIter it; RtttlNote n;
    rtttl_iter_init(&it, &s);
    // 32p -> pause
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));
    TEST_ASSERT_EQUAL_UINT16(0, n.freq_hz);
    // 32f# octave 5 -> 740 Hz
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));
    TEST_ASSERT_EQUAL_UINT16(740, n.freq_hz);
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));
    // 8b. dotted eighth at 45 bpm: whole = 5333 ms, 1/8 = 666, dotted = 999
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));
    TEST_ASSERT_EQUAL_UINT16(988, n.freq_hz);   // b5
    TEST_ASSERT_EQUAL_UINT16(999, n.duration_ms);
}

void test_valid_nokia_default_octave(void) {
    RtttlSong s;
    TEST_ASSERT_TRUE(rtttl_parse(NOKIA, &s));
    RtttlIter it; RtttlNote n;
    rtttl_iter_init(&it, &s);
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));   // e6
    TEST_ASSERT_EQUAL_UINT16(1319, n.freq_hz);
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));   // d6
    TEST_ASSERT_TRUE(rtttl_iter_next(&it, &n));   // f# (octave 5)
    TEST_ASSERT_EQUAL_UINT16(740, n.freq_hz);
}

void test_defaults_any_order_and_spaces(void) {
    RtttlSong s;
    TEST_ASSERT_TRUE(rtttl_parse("X: b=120 , o=4, d=8 : c, d , e", &s));
    TEST_ASSERT_EQUAL_UINT8(8, s.default_dur);
    TEST_ASSERT_EQUAL_UINT8(4, s.default_oct);
    TEST_ASSERT_EQUAL_UINT16(120, s.bpm);
    TEST_ASSERT_EQUAL_UINT16(3, s.note_count);
    // empty defaults section is allowed
    TEST_ASSERT_TRUE(rtttl_parse("::c,d,e", &s));
    TEST_ASSERT_EQUAL_UINT8(4, s.default_dur);
    TEST_ASSERT_EQUAL_UINT8(6, s.default_oct);
    TEST_ASSERT_EQUAL_UINT16(63, s.bpm);
}

void test_rejects_garbage(void) {
    RtttlSong s;
    TEST_ASSERT_FALSE(rtttl_parse("", &s));
    TEST_ASSERT_FALSE(rtttl_looks_like(""));
    TEST_ASSERT_FALSE(rtttl_parse("hello world!", &s));
    TEST_ASSERT_FALSE(rtttl_looks_like("hello world!"));
    TEST_ASSERT_FALSE(rtttl_parse("beep", &s));
    TEST_ASSERT_FALSE(rtttl_parse("no colons here at all", &s));
    TEST_ASSERT_FALSE(rtttl_parse("onlyone:d=4", &s));           // missing second ':'
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=100:", &s));       // no notes
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=100:c,x,e", &s));  // unknown note
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=100:c;d", &s));    // bad separator
    TEST_ASSERT_FALSE(rtttl_parse("x:z=4:c", &s));                // unknown default key
    TEST_ASSERT_FALSE(rtttl_parse("x:d=:c", &s));                 // missing value
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4:p#", &s));               // sharp pause
    TEST_ASSERT_FALSE(rtttl_parse(nullptr, &s));
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4:c\x01d", &s));           // control char
}

void test_rejects_out_of_range(void) {
    RtttlSong s;
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=9,b=100:c", &s));      // scale 9 default
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=100:c9", &s));     // scale 9 on note
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=3,b=100:c", &s));      // scale 3
    TEST_ASSERT_TRUE (rtttl_parse("x:d=4,o=7,b=100:c7,b7,b#7", &s)); // top of table clamps safely
    TEST_ASSERT_FALSE(rtttl_parse("x:d=3,o=5,b=100:c", &s));      // duration 3
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=100:64c", &s));    // duration 64
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=0:c", &s));        // bpm 0 (division by zero guard)
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=5000:c", &s));     // bpm too high
    TEST_ASSERT_FALSE(rtttl_parse("x:d=4,o=5,b=100:c..", &s));    // double dot
}

void test_rejects_too_long_melody(void) {
    RtttlSong s;
    // 1-note whole notes at 25 bpm = 9600 ms each; two of them exceed 15 s
    TEST_ASSERT_TRUE (rtttl_parse("x:d=1,o=5,b=25:c", &s));
    TEST_ASSERT_FALSE(rtttl_parse("x:d=1,o=5,b=25:c,c", &s));

    // Overlong string (> RTTTL_MAX_LEN)
    static char big[RTTTL_MAX_LEN + 64];
    memset(big, 0, sizeof(big));
    strcpy(big, "x:d=32,o=5,b=900:");
    size_t pos = strlen(big);
    while (pos + 2 < sizeof(big) - 1) { big[pos++] = 'c'; big[pos++] = ','; }
    big[pos] = '\0';
    TEST_ASSERT_FALSE(rtttl_parse(big, &s));
    TEST_ASSERT_FALSE(rtttl_looks_like(big));
}

void test_name_length_limit(void) {
    RtttlSong s;
    char buf[128];
    memset(buf, 'n', RTTTL_MAX_NAME_LEN);
    buf[RTTTL_MAX_NAME_LEN] = '\0';
    strcat(buf, ":d=4:c");
    TEST_ASSERT_TRUE(rtttl_parse(buf, &s));
    memset(buf, 'n', RTTTL_MAX_NAME_LEN + 1);
    buf[RTTTL_MAX_NAME_LEN + 1] = '\0';
    strcat(buf, ":d=4:c");
    TEST_ASSERT_FALSE(rtttl_parse(buf, &s));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_mario);
    RUN_TEST(test_valid_starwars_dotted_and_sharps);
    RUN_TEST(test_valid_nokia_default_octave);
    RUN_TEST(test_defaults_any_order_and_spaces);
    RUN_TEST(test_rejects_garbage);
    RUN_TEST(test_rejects_out_of_range);
    RUN_TEST(test_rejects_too_long_melody);
    RUN_TEST(test_name_length_limit);
    return UNITY_END();
}
