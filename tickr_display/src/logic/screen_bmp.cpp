#include "screen_bmp.h"
#include <string.h>

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

void screen_bmp_header(uint8_t h[BMP_HEADER_LEN]) {
    memset(h, 0, BMP_HEADER_LEN);
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, BMP_FILE_LEN);
    put32(h + 10, BMP_HEADER_LEN);            // pixel data offset
    put32(h + 14, 40);                        // BITMAPINFOHEADER
    put32(h + 18, SCREEN_W);
    put32(h + 22, SCREEN_H);                  // positive = bottom-up rows
    put16(h + 26, 1);                         // planes
    put16(h + 28, 1);                         // bits per pixel
    put32(h + 30, 0);                         // BI_RGB
    put32(h + 34, BMP_ROW_BYTES * SCREEN_H);  // image size
    put32(h + 38, 2835);                      // 72 dpi
    put32(h + 42, 2835);
    put32(h + 46, 2);                         // colours used
    put32(h + 50, 2);                         // important colours
    // Palette: index 0 = white, index 1 = black (B,G,R,0)
    h[54] = 0xFF; h[55] = 0xFF; h[56] = 0xFF; h[57] = 0x00;
    h[58] = 0x00; h[59] = 0x00; h[60] = 0x00; h[61] = 0x00;
}

size_t screen_bmp_read(const uint8_t* frame, size_t pos, uint8_t* out, size_t len) {
    if (pos >= BMP_FILE_LEN) return 0;
    if (pos + len > BMP_FILE_LEN) len = BMP_FILE_LEN - pos;
    size_t n = 0;
    if (pos < BMP_HEADER_LEN) {
        uint8_t h[BMP_HEADER_LEN];
        screen_bmp_header(h);
        size_t take = BMP_HEADER_LEN - pos;
        if (take > len) take = len;
        memcpy(out, h + pos, take);
        n += take;
        pos += take;
    }
    while (n < len) {
        size_t rel = pos - BMP_HEADER_LEN;
        size_t bmp_row = rel / BMP_ROW_BYTES;
        size_t col = rel % BMP_ROW_BYTES;
        size_t src_row = SCREEN_H - 1 - bmp_row;
        size_t take = BMP_ROW_BYTES - col;
        if (take > len - n) take = len - n;
        for (size_t i = 0; i < take; i++) {
            size_t c = col + i;
            out[n + i] = c < SCREEN_ROW_BYTES ? frame[src_row * SCREEN_ROW_BYTES + c] : 0;
        }
        n += take;
        pos += take;
    }
    return n;
}
