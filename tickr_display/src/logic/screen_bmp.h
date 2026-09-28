#pragma once
// 1-bit BMP view of the shadow framebuffer (docs/API.md "Screen").
//
// Frame format (both the raw endpoint and the canvas): W x H pixels, 1 bpp,
// rows top-down, (W+7)/8 bytes per row, MSB first, 1 = black.
// BMP: 62-byte header (14 file + 40 DIB + 2 palette entries), rows bottom-up,
// padded to 4 bytes, palette index 0 = white, 1 = black - so the pixel bits
// are copied unchanged and only the row order is reversed.
//
// Pure logic, unit-tested on the host.
#include <stdint.h>
#include <stddef.h>

#define SCREEN_W          296
#define SCREEN_H          128
#define SCREEN_ROW_BYTES  ((SCREEN_W + 7) / 8)              // 37
#define SCREEN_FRAME_LEN  (SCREEN_ROW_BYTES * SCREEN_H)     // 4736
#define BMP_HEADER_LEN    62
#define BMP_ROW_BYTES     (((SCREEN_W + 31) / 32) * 4)      // 40
#define BMP_FILE_LEN      (BMP_HEADER_LEN + BMP_ROW_BYTES * SCREEN_H)   // 5182

// Writes the 62-byte header into `out`.
void screen_bmp_header(uint8_t out[BMP_HEADER_LEN]);

// Copies `len` bytes of the BMP file starting at file offset `pos` into
// `out`, reading pixels from `frame` (SCREEN_FRAME_LEN bytes). Returns the
// number of bytes produced (0 at/after the end of the file).
size_t screen_bmp_read(const uint8_t* frame, size_t pos, uint8_t* out, size_t len);
