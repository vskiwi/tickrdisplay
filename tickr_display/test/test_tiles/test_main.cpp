// Host-side unit tests for the tile layouts (logic/tiles.h): presets and
// cell rectangles, the status band, the row geometry and font ladders, the
// row-sharing rule (with the xAdvance tables of FreeSansBold 9 / 12 / 18 pt
// as the width measure), the style layering and the frame key.
// Run with:  pio test -e native -f test_tiles
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "logic/tiles.h"

// xAdvance of the printable ASCII glyphs 0x20..0x7E (Adafruit GFX FreeSansBold 9 / 12 / 18 pt).
static const uint8_t kAdv9[95]  = {5,6,9,10,10,16,13,5,6,6,7,11,4,6,4,5,10,10,10,10,10,10,10,10,10,10,4,4,11,11,11,11,18,13,13,13,13,12,11,14,13,6,10,13,11,16,14,14,12,14,13,12,12,13,12,17,12,12,11,6,5,6,11,10,5,10,11,10,11,10,6,11,11,5,5,10,5,16,11,11,11,11,7,10,6,11,10,14,10,10,9,7,5,7,9};
static const uint8_t kAdv12[95] = {7,8,11,13,13,21,17,6,8,8,9,14,6,8,6,7,13,14,13,13,13,13,13,13,13,13,6,6,14,14,14,15,23,17,17,17,17,16,15,18,18,7,14,17,15,21,18,19,16,19,17,16,15,18,16,23,16,15,15,8,7,8,14,13,6,14,15,13,15,14,8,15,14,7,7,14,6,21,15,15,15,15,9,13,8,15,13,19,13,13,12,9,7,9,12};
static const uint8_t kAdv18[95] = {10,12,17,19,19,31,25,9,12,12,14,20,9,12,9,10,19,19,19,19,19,19,19,19,19,19,9,9,20,20,20,21,34,24,25,25,25,23,22,27,26,11,20,25,22,30,26,27,24,27,25,24,23,26,23,34,24,22,21,12,10,12,20,19,9,20,22,20,22,20,12,21,21,10,10,20,9,31,21,21,22,22,14,19,12,21,19,27,19,19,18,14,10,14,18};

// Width of `text` at a step: the sum of the advances, doubled for the x2 steps.
static int16_t measure(const char* text, uint8_t step, void*) {
    const uint8_t* adv = (step == TILE_F18X2 || step == TILE_F18) ? kAdv18 : (step == TILE_F12X2 || step == TILE_F12) ? kAdv12 : kAdv9;
    const int mul = (step == TILE_F18X2 || step == TILE_F12X2) ? 2 : 1;
    int w = 0;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        w += (c >= 0x20 && c <= 0x7E) ? adv[c - 0x20] : 0;
    }
    return (int16_t)(w * mul);
}

void setUp(void) {}
void tearDown(void) {}

void test_names_round_trip(void) {
    uint8_t v = 99;
    TEST_ASSERT_TRUE(tile_layout_parse("2x2", &v));    TEST_ASSERT_EQUAL(TILE_LAYOUT_2X2, v);
    TEST_ASSERT_TRUE(tile_layout_parse("single", &v)); TEST_ASSERT_EQUAL(TILE_LAYOUT_SINGLE, v);
    TEST_ASSERT_TRUE(tile_layout_parse("1x2", &v));    TEST_ASSERT_EQUAL_STRING("1x2", tile_layout_str(v));
    TEST_ASSERT_FALSE(tile_layout_parse("grid", &v));  TEST_ASSERT_EQUAL(TILE_LAYOUT_1X2, v);   // untouched
    TEST_ASSERT_FALSE(tile_layout_parse("2X2", &v));
    TEST_ASSERT_FALSE(tile_layout_parse(NULL, &v));
    TEST_ASSERT_TRUE(tile_look_parse("ticker", &v));   TEST_ASSERT_EQUAL(TILE_LOOK_TICKER, v);
    TEST_ASSERT_FALSE(tile_look_parse("big", &v));
    TEST_ASSERT_TRUE(tile_size_parse("s", &v));        TEST_ASSERT_EQUAL(TILE_SIZE_S, v);
    TEST_ASSERT_TRUE(tile_size_parse("auto", &v));     TEST_ASSERT_EQUAL(TILE_SIZE_AUTO, v);
    TEST_ASSERT_FALSE(tile_size_parse("xl", &v));
    TEST_ASSERT_EQUAL_STRING("single", tile_layout_str(200));   // out of range = the default name
}

void test_layout_cells_and_default(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tiles_layout_cells(TILE_LAYOUT_SINGLE));
    TEST_ASSERT_EQUAL_UINT8(1, tiles_layout_cells(TILE_LAYOUT_1X1));
    TEST_ASSERT_EQUAL_UINT8(2, tiles_layout_cells(TILE_LAYOUT_1X2));
    TEST_ASSERT_EQUAL_UINT8(2, tiles_layout_cells(TILE_LAYOUT_2X1));
    TEST_ASSERT_EQUAL_UINT8(4, tiles_layout_cells(TILE_LAYOUT_2X2));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, tiles_layout_default(0));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_1X1, tiles_layout_default(1));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_1X2, tiles_layout_default(2));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, tiles_layout_default(3));
    TEST_ASSERT_EQUAL_UINT8(TILE_LAYOUT_2X2, tiles_layout_default(4));
}

static void expect_rect(uint8_t layout, uint8_t idx, int16_t x, int16_t y, int16_t w, int16_t h) {
    TileRect r;
    TEST_ASSERT_TRUE(tiles_cell_rect(layout, idx, &r));
    TEST_ASSERT_EQUAL_INT16(x, r.x); TEST_ASSERT_EQUAL_INT16(y, r.y);
    TEST_ASSERT_EQUAL_INT16(w, r.w); TEST_ASSERT_EQUAL_INT16(h, r.h);
}

void test_cell_rects_per_preset(void) {
    TileRect r;
    expect_rect(TILE_LAYOUT_1X1, 0, 0, 0, 296, 128);
    expect_rect(TILE_LAYOUT_1X2, 0, 0, 0, 148, 128);      // separator at x = 148
    expect_rect(TILE_LAYOUT_1X2, 1, 149, 0, 147, 128);
    expect_rect(TILE_LAYOUT_2X1, 0, 0, 0, 296, 64);       // separator at y = 64
    expect_rect(TILE_LAYOUT_2X1, 1, 0, 65, 296, 63);
    expect_rect(TILE_LAYOUT_2X2, 0, 0, 0, 148, 64);       // the ticker grid's cells
    expect_rect(TILE_LAYOUT_2X2, 1, 149, 0, 147, 64);
    expect_rect(TILE_LAYOUT_2X2, 2, 0, 65, 148, 63);
    expect_rect(TILE_LAYOUT_2X2, 3, 149, 65, 147, 63);
    TEST_ASSERT_FALSE(tiles_cell_rect(TILE_LAYOUT_1X1, 1, &r));
    TEST_ASSERT_FALSE(tiles_cell_rect(TILE_LAYOUT_2X2, 4, &r));
    TEST_ASSERT_FALSE(tiles_cell_rect(TILE_LAYOUT_SINGLE, 0, &r));
    // the cells tile the panel: every pixel row / column is covered once
    for (uint8_t l = TILE_LAYOUT_1X1; l < TILE_LAYOUT_COUNT; l++) {
        long area = 0;
        for (uint8_t i = 0; tiles_cell_rect(l, i, &r); i++) {
            area += (long)r.w * r.h;
            TEST_ASSERT_TRUE(r.x + r.w <= 296 && r.y + r.h <= 128);
        }
        long seps = (l == TILE_LAYOUT_1X2 ? 128 : 0) + (l == TILE_LAYOUT_2X1 ? 296 : 0) + (l == TILE_LAYOUT_2X2 ? 128 + 296 - 1 : 0);
        TEST_ASSERT_EQUAL_INT32(296L * 128 - seps, area);
    }
}

void test_status_band_only_in_the_bottom_right_cell(void) {
    TileRect u;
    TEST_ASSERT_EQUAL_UINT8(3, tiles_status_cell(TILE_LAYOUT_2X2));
    TEST_ASSERT_EQUAL_UINT8(1, tiles_status_cell(TILE_LAYOUT_1X2));
    TEST_ASSERT_EQUAL_UINT8(0, tiles_status_cell(TILE_LAYOUT_1X1));
    tiles_usable(TILE_LAYOUT_2X2, 3, true, &u);  TEST_ASSERT_EQUAL_INT16(63 - 16, u.h); TEST_ASSERT_EQUAL_INT16(65, u.y);
    tiles_usable(TILE_LAYOUT_2X2, 2, true, &u);  TEST_ASSERT_EQUAL_INT16(63, u.h);        // Q3 touches the bottom but keeps its height
    tiles_usable(TILE_LAYOUT_2X2, 0, true, &u);  TEST_ASSERT_EQUAL_INT16(64, u.h);
    tiles_usable(TILE_LAYOUT_2X2, 3, false, &u); TEST_ASSERT_EQUAL_INT16(63, u.h);        // status off: the whole cell
    tiles_usable(TILE_LAYOUT_1X2, 1, true, &u);  TEST_ASSERT_EQUAL_INT16(112, u.h);
    tiles_usable(TILE_LAYOUT_1X2, 0, true, &u);  TEST_ASSERT_EQUAL_INT16(128, u.h);
    tiles_usable(TILE_LAYOUT_2X1, 1, true, &u);  TEST_ASSERT_EQUAL_INT16(47, u.h);
    tiles_usable(TILE_LAYOUT_1X1, 0, true, &u);  TEST_ASSERT_EQUAL_INT16(112, u.h);
    tiles_usable(TILE_LAYOUT_2X2, 7, true, &u);  TEST_ASSERT_EQUAL_INT16(0, u.h);         // outside: empty
}

void test_cramped_cells(void) {
    TileRect u;
    tiles_usable(TILE_LAYOUT_2X2, 3, true, &u);  TEST_ASSERT_TRUE(tiles_cramped(&u));     // Q4 at 47 px
    tiles_usable(TILE_LAYOUT_2X1, 1, true, &u);  TEST_ASSERT_TRUE(tiles_cramped(&u));     // the bottom row at 47 px
    tiles_usable(TILE_LAYOUT_2X2, 3, false, &u); TEST_ASSERT_FALSE(tiles_cramped(&u));    // 63 px
    tiles_usable(TILE_LAYOUT_2X2, 0, true, &u);  TEST_ASSERT_FALSE(tiles_cramped(&u));
    tiles_usable(TILE_LAYOUT_1X2, 1, true, &u);  TEST_ASSERT_FALSE(tiles_cramped(&u));
}

void test_geometry_stack_two_rows(void) {
    TileRect u; TileGeom g;
    tiles_usable(TILE_LAYOUT_2X2, 0, true, &u);              // 148 x 64
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    TEST_ASSERT_EQUAL_UINT8(2, g.rows);
    TEST_ASSERT_FALSE(g.cramped);
    TEST_ASSERT_TRUE(g.title_h >= 36);                       // 12 pt x2 fits the title band
    TEST_ASSERT_EQUAL_INT16(20, g.value_h);                  // the bottom row: 12 pt
    TEST_ASSERT_EQUAL_INT16(g.value_top, g.change_top);
    TEST_ASSERT_TRUE(g.value_top + g.value_h <= u.h);        // inside the cell
    TEST_ASSERT_TRUE(g.title_top + g.title_h <= g.value_top);
    // Q4 with the band: cramped, the bottom row 9 pt, the title band still holds 18 pt
    tiles_usable(TILE_LAYOUT_2X2, 3, true, &u);              // 147 x 47
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    TEST_ASSERT_EQUAL_UINT8(2, g.rows);
    TEST_ASSERT_TRUE(g.cramped);
    TEST_ASSERT_EQUAL_INT16(13, g.value_h);
    TEST_ASSERT_TRUE(g.title_h >= 26 && g.title_h < 36);
    TEST_ASSERT_TRUE(g.value_top + g.value_h <= u.h);
}

void test_geometry_stack_three_rows_and_row_look(void) {
    TileRect u; TileGeom g;
    tiles_usable(TILE_LAYOUT_1X2, 0, true, &u);              // 148 x 128: three bands
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, false, &g);
    TEST_ASSERT_EQUAL_UINT8(3, g.rows);
    TEST_ASSERT_TRUE(g.title_h >= 52);                       // 18 pt x2 allowed by height (the width decides)
    TEST_ASSERT_TRUE(g.value_h >= 26 && g.change_h >= 26);   // 18 pt value and change
    TEST_ASSERT_TRUE(g.change_top + g.change_h <= u.h);
    TEST_ASSERT_TRUE(g.value_top >= g.title_top + g.title_h && g.change_top >= g.value_top + g.value_h);
    tiles_usable(TILE_LAYOUT_1X2, 1, true, &u);              // 147 x 112 with the band: still three bands, no 18 x2
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    TEST_ASSERT_EQUAL_UINT8(3, g.rows);
    TEST_ASSERT_TRUE(g.title_h >= 36 && g.title_h < 52);
    TEST_ASSERT_TRUE(g.value_h >= 26);
    // the frame's smallest cell lays out every tall cell: the left cell of a 1x2 with the
    // band on the right gets the right cell's geometry row for row (its extra rows stay white)
    TEST_ASSERT_EQUAL_INT16(112, tiles_frame_min_h(TILE_LAYOUT_1X2, true));
    TEST_ASSERT_EQUAL_INT16(128, tiles_frame_min_h(TILE_LAYOUT_1X2, false));
    TEST_ASSERT_EQUAL_INT16(47, tiles_frame_min_h(TILE_LAYOUT_2X2, true));
    TileGeom left;
    tiles_usable(TILE_LAYOUT_1X2, 0, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, tiles_frame_min_h(TILE_LAYOUT_1X2, true), true, &left);
    TEST_ASSERT_EQUAL_MEMORY(&g, &left, sizeof(TileGeom));
    tiles_usable(TILE_LAYOUT_2X1, 0, true, &u);              // the row look: one band
    tiles_geometry(TILE_LOOK_ROW, &u, u.h, true, &g);
    TEST_ASSERT_EQUAL_UINT8(1, g.rows);
    TEST_ASSERT_EQUAL_INT16(g.title_top, g.change_top);
    TEST_ASSERT_EQUAL_INT16(g.title_h, g.value_h);
    TEST_ASSERT_TRUE(g.title_h >= 52);
    tiles_geometry(TILE_LOOK_TICKER, &u, u.h, true, &g);                // ticker: one band too (unused by the drawer)
    TEST_ASSERT_EQUAL_UINT8(1, g.rows);
}

// The cells next to the status band keep clear of it: the lowest band ends at
// least a margin above the band's top row (usable.h), the fonts stay 18 pt in a
// tall cell, and the right padding is the standard one in every cell.
static void expect_clear_of_band(uint8_t layout, uint8_t idx, uint8_t look, int16_t margin, const char* what) {
    TileRect u; TileGeom g; TileSteps s;
    tiles_usable(layout, idx, true, &u);
    tiles_geometry(look, &u, tiles_frame_min_h(layout, true), true, &g);
    tiles_steps(look, TILE_SIZE_AUTO, &g, &s);
    int16_t bottom = g.change_top + g.change_h;
    if (g.value_top + g.value_h > bottom) bottom = g.value_top + g.value_h;
    TEST_ASSERT_TRUE_MESSAGE(bottom + margin <= u.h, what);                         // the band begins at u.h
    TEST_ASSERT_TRUE_MESSAGE(tile_step_h[s.change[0]] <= g.change_h, what);         // the first step really fits the band
    TEST_ASSERT_TRUE_MESSAGE(u.x + u.w - 1 - TILES_PAD <= TILES_SCREEN_W - 1 - TILES_PAD, what);   // the drawer's right edge
    TEST_ASSERT_TRUE_MESSAGE(TILES_PAD >= 4, what);
}

void test_cells_next_to_the_status_band_keep_clear_of_it(void) {
    TileRect u; TileGeom g; TileSteps s;
    // 1x2 right column (147 x 112): the third band ends >= 4 + 2 rows above the band and keeps 18 pt
    expect_clear_of_band(TILE_LAYOUT_1X2, 1, TILE_LOOK_STACK, 6, "1x2 right");
    tiles_usable(TILE_LAYOUT_1X2, 1, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, tiles_frame_min_h(TILE_LAYOUT_1X2, true), true, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.change[0]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.value[0]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12X2, s.title[0]);
    TEST_ASSERT_TRUE(g.change_top + g.change_h <= u.h - 6);
    // 1x1 with the band: the same bands
    expect_clear_of_band(TILE_LAYOUT_1X1, 0, TILE_LOOK_STACK, 6, "1x1");
    // 2x1 bottom row and the 2x2 Q4 (147 x 47, cramped): the 9 pt row ends >= 2 + 2 rows above the band, the title keeps 18 pt
    expect_clear_of_band(TILE_LAYOUT_2X1, 1, TILE_LOOK_STACK, 4, "2x1 bottom");
    expect_clear_of_band(TILE_LAYOUT_2X2, 3, TILE_LOOK_STACK, 4, "2x2 Q4");
    tiles_usable(TILE_LAYOUT_2X2, 3, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, tiles_frame_min_h(TILE_LAYOUT_2X2, true), true, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.title[0]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.value[0]);
    // the row look in the cramped row too
    expect_clear_of_band(TILE_LAYOUT_2X1, 1, TILE_LOOK_ROW, 4, "2x1 bottom row look");
    // without the band nothing is reserved: the 1x1 hero keeps 18 pt x2 for its title
    tiles_usable(TILE_LAYOUT_1X1, 0, false, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, tiles_frame_min_h(TILE_LAYOUT_1X1, false), false, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18X2, s.title[0]);
    TEST_ASSERT_TRUE(g.change_top + g.change_h <= u.h);
}

void test_ladders_by_height_and_size(void) {
    TileRect u; TileGeom g; TileSteps s;
    tiles_usable(TILE_LAYOUT_2X2, 0, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    // title: 12x2 -> 18 -> 12 -> 9 (18x2 is 52 px, too tall for 38); value / change: 12 -> 9
    TEST_ASSERT_EQUAL_UINT8(4, s.title_n);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12X2, s.title[0]); TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.title[3]);
    TEST_ASSERT_EQUAL_UINT8(2, s.value_n);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.value[0]);   TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.value[1]);
    TEST_ASSERT_EQUAL_UINT8(2, s.change_n);
    // size m: 18 pt at most, value from 12; size s: 12 at most, value 9 only
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_M, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.title[0]); TEST_ASSERT_EQUAL_UINT8(3, s.title_n);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.value[0]);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_S, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.title[0]); TEST_ASSERT_EQUAL_UINT8(2, s.title_n);
    TEST_ASSERT_EQUAL_UINT8(1, s.value_n);         TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.value[0]);
    // l = auto today
    TileSteps l;
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_L, &g, &l);
    TEST_ASSERT_EQUAL_MEMORY(&s, &s, sizeof(s));
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(l.title_n, s.title_n);
    TEST_ASSERT_EQUAL_UINT8(l.title[0], s.title[0]);
    // cramped Q4: title 18 -> 12 -> 9, value / change 9 only
    tiles_usable(TILE_LAYOUT_2X2, 3, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.title[0]);
    TEST_ASSERT_EQUAL_UINT8(1, s.value_n); TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.value[0]);
    TEST_ASSERT_EQUAL_UINT8(1, s.change_n);
    // a tall cell: the title may try 18x2, value / change 18 -> 12 -> 9
    tiles_usable(TILE_LAYOUT_1X2, 0, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, false, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18X2, s.title[0]); TEST_ASSERT_EQUAL_UINT8(5, s.title_n);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.value[0]);   TEST_ASSERT_EQUAL_UINT8(3, s.value_n);
    // the row look never doubles 18 pt
    tiles_usable(TILE_LAYOUT_2X1, 0, true, &u);
    tiles_geometry(TILE_LOOK_ROW, &u, u.h, true, &g);
    tiles_steps(TILE_LOOK_ROW, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12X2, s.title[0]);
    // the smallest step is always there, even in a band too low for it
    TileGeom tiny = {}; tiny.rows = 1; tiny.title_h = tiny.value_h = tiny.change_h = 5;
    tiles_steps(TILE_LOOK_ROW, TILE_SIZE_AUTO, &tiny, &s);
    TEST_ASSERT_EQUAL_UINT8(1, s.title_n); TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.title[0]);
}

void test_fit_row_shrinks_change_then_value_then_title(void) {
    TileRect u; TileGeom g; TileSteps s;
    uint8_t ti, vi, ci;
    // the stack's bottom row in a 148 px cell: "80000" + "+15%" at 12 pt is 65 + 60 + 15 + 8 = 148 > 140,
    // so the change drops to 9 pt (42): 65 + 42 + 15 + 8 = 130 fits
    tiles_usable(TILE_LAYOUT_2X2, 0, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_TRUE(tiles_fit_row(measure, NULL, NULL, "80000", "+15%", &s, 140, TILES_GAP, TILES_ARROW_W, &ti, &vi, &ci));
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.value[vi]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.change[ci]);
    // a short pair stays 12 / 12: "4100" (52) + "+9%" (47) + 15 + 8 = 122
    TEST_ASSERT_TRUE(tiles_fit_row(measure, NULL, NULL, "4100", "+9%", &s, 140, TILES_GAP, TILES_ARROW_W, &ti, &vi, &ci));
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.value[vi]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.change[ci]);
    // a long value: the change at 9, the value at 9 - "0.00001234" 9 pt = 84 + 42 + 15 + 8 = 149 > 140: false, the drawer truncates
    TEST_ASSERT_FALSE(tiles_fit_row(measure, NULL, NULL, "0.00001234", "+15%", &s, 140, TILES_GAP, TILES_ARROW_W, &ti, &vi, &ci));
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.value[vi]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.change[ci]);
    // no change: the value alone, no arrow / gap: "0.00001234" 12 pt = 110 fits 140
    TEST_ASSERT_TRUE(tiles_fit_row(measure, NULL, NULL, "0.00001234", "", &s, 140, TILES_GAP, TILES_ARROW_W, &ti, &vi, &ci));
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.value[vi]);
    // the row look on 288 px: "BTC" 12x2 (98) + "80000" + "+15%": the change at 18 (94 + 15) and the value
    // at 12x2 (130) are too wide; change 12 (60 + 15) with value 18 (95): 98 + 95 + 75 + 16 = 284 fits
    tiles_usable(TILE_LAYOUT_2X1, 0, true, &u);
    tiles_geometry(TILE_LOOK_ROW, &u, u.h, true, &g);
    tiles_steps(TILE_LOOK_ROW, TILE_SIZE_AUTO, &g, &s);
    TEST_ASSERT_TRUE(tiles_fit_row(measure, NULL, "BTC", "80000", "+15%", &s, 288, TILES_GAP, TILES_ARROW_W, &ti, &vi, &ci));
    TEST_ASSERT_EQUAL_UINT8(TILE_F12X2, s.title[ti]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F18, s.value[vi]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F12, s.change[ci]);
    // the title shrinks last: a long one pushes everything to the smallest and then fails
    TEST_ASSERT_FALSE(tiles_fit_row(measure, NULL, "ABCDEFGHIJKLMNO", "1234567890123", "+1234.56%", &s, 140, TILES_GAP, TILES_ARROW_W, &ti, &vi, &ci));
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.title[ti]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.value[vi]);
    TEST_ASSERT_EQUAL_UINT8(TILE_F9, s.change[ci]);
}

void test_title_ladder_samples(void) {
    // which step a title gets in a 140 px band of a 2x2 cell (the drawer's fit_text uses the same order)
    TileRect u; TileGeom g; TileSteps s;
    tiles_usable(TILE_LAYOUT_2X2, 0, true, &u);
    tiles_geometry(TILE_LOOK_STACK, &u, u.h, true, &g);
    tiles_steps(TILE_LOOK_STACK, TILE_SIZE_AUTO, &g, &s);
    struct { const char* t; uint8_t step; } cases[] = {
        { "BTC",      TILE_F12X2 },   // 98 px
        { "DOGE",     TILE_F12X2 },   // 140 px, exactly
        { "MATIC",    TILE_F18 },     // 154 at 12x2, 113 at 18
        { "PEPEUSDT", TILE_F12 },     // 192 at 18, 130 at 12
        { "ABCDEFGHIJKL", TILE_F9 },  // 12 caps: 12 pt is too wide, 9 pt (~ 150) does not fit either -> smallest, truncated
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t chosen = s.title[s.title_n - 1];
        for (uint8_t k = 0; k < s.title_n; k++) {
            if (measure(cases[i].t, s.title[k], NULL) <= 140) { chosen = s.title[k]; break; }
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(cases[i].step, chosen, cases[i].t);
    }
}

void test_style_resolve_layers(void) {
    TileStyleSpec root = {}, tile = {};
    TileStyle out;
    tiles_style_resolve(NULL, NULL, &out);
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_STACK, out.look);
    TEST_ASSERT_EQUAL_UINT8(TILE_SIZE_AUTO, out.size);
    TEST_ASSERT_FALSE(out.invert);
    root.st.look = TILE_LOOK_ROW; root.has_look = true;
    root.st.invert = true; root.has_invert = true;
    tiles_style_resolve(&root, NULL, &out);
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_ROW, out.look);
    TEST_ASSERT_TRUE(out.invert);
    TEST_ASSERT_EQUAL_UINT8(TILE_SIZE_AUTO, out.size);
    tile.st.invert = false; tile.has_invert = true;        // the tile overrides one key only
    tile.st.size = TILE_SIZE_S; tile.has_size = true;
    tile.st.look = TILE_LOOK_TICKER;                       // not flagged: ignored
    tiles_style_resolve(&root, &tile, &out);
    TEST_ASSERT_EQUAL_UINT8(TILE_LOOK_ROW, out.look);
    TEST_ASSERT_EQUAL_UINT8(TILE_SIZE_S, out.size);
    TEST_ASSERT_FALSE(out.invert);
}

static void fill(TilesFrame* f, uint8_t layout, const char* a, const char* b, const char* c, const char* d) {
    memset(f, 0, sizeof(*f));
    f->layout = layout;
    f->n = tiles_layout_cells(layout);
    f->status = true;
    const char* names[4] = { a, b, c, d };
    for (uint8_t i = 0; i < 4; i++) {
        if (!names[i]) continue;
        f->t[i].has = true;
        strncpy(f->t[i].title, names[i], TILE_TITLE_MAX - 1);
        strcpy(f->t[i].value, "1");
    }
}

void test_frame_key_values_partial_everything_else_full(void) {
    TilesFrame a, b;
    fill(&a, TILE_LAYOUT_2X2, "BTC", "ETH", "SOL", NULL);
    b = a;
    strcpy(b.t[0].value, "99999"); strcpy(b.t[1].change, "+5%"); b.t[1].dir = 1; b.t[1].has_dir = true;
    TEST_ASSERT_EQUAL_UINT32(tiles_frame_key(&a), tiles_frame_key(&b));   // numbers only: partial
    b = a; strcpy(b.t[0].title, "XBT");
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&a), tiles_frame_key(&b));      // another name
    b = a; b.t[3].has = true; strcpy(b.t[3].title, "BNB");
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&a), tiles_frame_key(&b));      // a fourth tile appears
    b = a; b.t[1].style.invert = true;
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&a), tiles_frame_key(&b));      // inversion toggled
    b = a; b.t[1].style.look = TILE_LOOK_ROW;
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&a), tiles_frame_key(&b));
    b = a; b.t[2].style.size = TILE_SIZE_S;
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&a), tiles_frame_key(&b));
    b = a; b.status = false;
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&a), tiles_frame_key(&b));
    fill(&b, TILE_LAYOUT_1X2, "BTC", "ETH", NULL, NULL);
    TilesFrame c; fill(&c, TILE_LAYOUT_2X1, "BTC", "ETH", NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&b), tiles_frame_key(&c));      // another preset, same names
    // the empty cells' styles do not count (an empty cell is white whatever the root style says)
    b = a; b.t[3].style.invert = true;
    TEST_ASSERT_EQUAL_UINT32(tiles_frame_key(&a), tiles_frame_key(&b));
    // names are delimited: "AB","C" differs from "A","BC"
    fill(&b, TILE_LAYOUT_1X2, "AB", "C", NULL, NULL);
    fill(&c, TILE_LAYOUT_1X2, "A", "BC", NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(tiles_frame_key(&b), tiles_frame_key(&c));
}

void test_frame_key_apart_from_the_ticker_grid(void) {
    // a tile frame and a ticker grid with the same four names never share a key
    TilesFrame f; fill(&f, TILE_LAYOUT_2X2, "BTC", "ETH", "SOL", "BNB");
    GridFrame g; memset(&g, 0, sizeof(g)); g.n = 4;
    const char* names[4] = { "BTC", "ETH", "SOL", "BNB" };
    for (uint8_t i = 0; i < 4; i++) strcpy(g.cells[i].short_label, names[i]);
    TEST_ASSERT_NOT_EQUAL(grid_set_key(&g), tiles_frame_key(&f));
}

void test_join_titles(void) {
    TilesFrame f; char buf[64];
    fill(&f, TILE_LAYOUT_2X2, "BTC", NULL, "SOL", NULL);
    tiles_join_titles(&f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("BTC, SOL", buf);
    f.t[1].has = true;                                     // a tile without a title (value only) adds nothing
    tiles_join_titles(&f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("BTC, SOL", buf);
    fill(&f, TILE_LAYOUT_2X2, NULL, NULL, NULL, NULL);
    tiles_join_titles(&f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    fill(&f, TILE_LAYOUT_2X2, "ABCDEFGHIJKLMNO", "ABCDEFGHIJKLMNO", "ABCDEFGHIJKLMNO", "ABCDEFGHIJKLMNO");
    char small[20];
    tiles_join_titles(&f, small, sizeof(small));           // a tight buffer: cut, terminated
    TEST_ASSERT_EQUAL(19, strlen(small));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_names_round_trip);
    RUN_TEST(test_layout_cells_and_default);
    RUN_TEST(test_cell_rects_per_preset);
    RUN_TEST(test_status_band_only_in_the_bottom_right_cell);
    RUN_TEST(test_cramped_cells);
    RUN_TEST(test_geometry_stack_two_rows);
    RUN_TEST(test_geometry_stack_three_rows_and_row_look);
    RUN_TEST(test_cells_next_to_the_status_band_keep_clear_of_it);
    RUN_TEST(test_ladders_by_height_and_size);
    RUN_TEST(test_fit_row_shrinks_change_then_value_then_title);
    RUN_TEST(test_title_ladder_samples);
    RUN_TEST(test_style_resolve_layers);
    RUN_TEST(test_frame_key_values_partial_everything_else_full);
    RUN_TEST(test_frame_key_apart_from_the_ticker_grid);
    RUN_TEST(test_join_titles);
    return UNITY_END();
}
