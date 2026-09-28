// Host tests for the 1-bit BMP view of the shadow framebuffer (src/logic/screen_bmp.cpp).
#include <unity.h>
#include <string.h>
#include "logic/screen_bmp.h"

void setUp() {}
void tearDown() {}

static uint8_t frame[SCREEN_FRAME_LEN];

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_constants() {
    TEST_ASSERT_EQUAL_UINT(37, SCREEN_ROW_BYTES);
    TEST_ASSERT_EQUAL_UINT(4736, SCREEN_FRAME_LEN);
    TEST_ASSERT_EQUAL_UINT(40, BMP_ROW_BYTES);
    TEST_ASSERT_EQUAL_UINT(5182, BMP_FILE_LEN);
}

static void test_header() {
    uint8_t h[BMP_HEADER_LEN];
    screen_bmp_header(h);
    TEST_ASSERT_EQUAL_UINT8('B', h[0]);
    TEST_ASSERT_EQUAL_UINT8('M', h[1]);
    TEST_ASSERT_EQUAL_UINT32(BMP_FILE_LEN, rd32(h + 2));
    TEST_ASSERT_EQUAL_UINT32(BMP_HEADER_LEN, rd32(h + 10));
    TEST_ASSERT_EQUAL_UINT32(40, rd32(h + 14));
    TEST_ASSERT_EQUAL_UINT32(SCREEN_W, rd32(h + 18));
    TEST_ASSERT_EQUAL_UINT32(SCREEN_H, rd32(h + 22));
    TEST_ASSERT_EQUAL_UINT16(1, h[28] | (h[29] << 8));   // 1 bpp
    TEST_ASSERT_EQUAL_UINT32(0, rd32(h + 30));           // BI_RGB
    TEST_ASSERT_EQUAL_UINT32(BMP_ROW_BYTES * SCREEN_H, rd32(h + 34));
    TEST_ASSERT_EQUAL_UINT32(0x00FFFFFF, rd32(h + 54));  // index 0 = white
    TEST_ASSERT_EQUAL_UINT32(0x00000000, rd32(h + 58));  // index 1 = black
}

static void test_row_reversal_and_padding() {
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x80;                                  // top-left pixel black
    frame[(SCREEN_H - 1) * SCREEN_ROW_BYTES + 36] = 0x01;   // bottom row, last byte (pixel 295)

    uint8_t file[BMP_FILE_LEN];
    size_t n = screen_bmp_read(frame, 0, file, sizeof(file));
    TEST_ASSERT_EQUAL_UINT(BMP_FILE_LEN, n);

    // BMP row 0 (first stored) = bottom frame row
    const uint8_t* row0 = file + BMP_HEADER_LEN;
    TEST_ASSERT_EQUAL_UINT8(0x01, row0[36]);
    TEST_ASSERT_EQUAL_UINT8(0x00, row0[37]);          // padding bytes are zero
    TEST_ASSERT_EQUAL_UINT8(0x00, row0[39]);
    // Last stored row = top frame row
    const uint8_t* rowN = file + BMP_HEADER_LEN + (SCREEN_H - 1) * BMP_ROW_BYTES;
    TEST_ASSERT_EQUAL_UINT8(0x80, rowN[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, rowN[36]);
}

static void test_chunked_reads_match_whole() {
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)(i * 7 + (i >> 5));
    uint8_t whole[BMP_FILE_LEN];
    TEST_ASSERT_EQUAL_UINT(BMP_FILE_LEN, screen_bmp_read(frame, 0, whole, sizeof(whole)));

    // Odd chunk sizes crossing the header/row boundaries
    const size_t chunks[] = {1, 7, 61, 62, 63, 1436, 5000};
    for (size_t c : chunks) {
        uint8_t out[BMP_FILE_LEN];
        size_t pos = 0;
        while (pos < BMP_FILE_LEN) {
            size_t n = screen_bmp_read(frame, pos, out + pos, c);
            TEST_ASSERT_TRUE(n > 0);
            pos += n;
        }
        TEST_ASSERT_EQUAL_UINT(BMP_FILE_LEN, pos);
        TEST_ASSERT_EQUAL_MEMORY(whole, out, BMP_FILE_LEN);
    }
    uint8_t dummy[8];
    TEST_ASSERT_EQUAL_UINT(0, screen_bmp_read(frame, BMP_FILE_LEN, dummy, sizeof(dummy)));
    TEST_ASSERT_EQUAL_UINT(2, screen_bmp_read(frame, BMP_FILE_LEN - 2, dummy, sizeof(dummy)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_constants);
    RUN_TEST(test_header);
    RUN_TEST(test_row_reversal_and_padding);
    RUN_TEST(test_chunked_reads_match_whole);
    return UNITY_END();
}
