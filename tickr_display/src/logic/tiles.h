#pragma once
// Tile layouts of a payload (docs/API.md "Tile layouts") - the pure parts,
// unit-tested on the host (test/test_tiles): the layout presets and their
// cell rectangles, the status band, the font ladders per cell height, the
// row-sharing rule, the style layering and the frame key of the refresh
// policy. No Arduino, no drawing: hal_display draws a TilesFrame, payload.cpp
// fills it from the JSON (`tiles[]`), renderer.cpp hands it over.
//
// The sender decides which tiles are filled; the firmware owns the tile
// mode - where the elements go and how large they are drawn.
#include <stdint.h>
#include <stddef.h>
#include "ticker.h"   // TickerDir, TICKER_CHANGE_MAX

#define TILES_MAX         4      // cells of the largest preset (2x2)
#define TILE_TITLE_MAX    16     // the big text: up to 15 printable ASCII chars
#define TILE_VALUE_MAX    24     // = GRID_PRICE_MAX
#define TILE_CHANGE_MAX   TICKER_CHANGE_MAX
#define TILES_SCREEN_W    296
#define TILES_SCREEN_H    128
#define TILES_STATUS_H    16     // the status band: badges + age line in the bottom rows
#define TILES_PAD         4      // padding inside a cell
#define TILES_GAP         8      // gap between elements on one row
#define TILES_ARROW_W     15     // the direction triangle (11 px) and its gap left of the change

// Layout presets (rows x cols). SINGLE = a payload without `tiles` - today's
// text / ticker frame, drawn by other code.
enum TileLayout : uint8_t {
    TILE_LAYOUT_SINGLE = 0,
    TILE_LAYOUT_1X1,       // one cell 296 x 128
    TILE_LAYOUT_1X2,       // two columns 148 x 128
    TILE_LAYOUT_2X1,       // two rows 296 x 64
    TILE_LAYOUT_2X2,       // four cells 148 x 64 (the ticker grid's geometry)
    TILE_LAYOUT_COUNT
};
// Placement templates of a cell.
enum TileLook : uint8_t {
    TILE_LOOK_STACK = 0,   // title big top-left; value bottom-left, change bottom-right (three rows in a tall cell)
    TILE_LOOK_ROW,         // one line: title left, value and change right
    TILE_LOOK_TICKER,      // the ticker grid's cell: name badge + change on top, value right-aligned below
    TILE_LOOK_COUNT
};
// Size cap of the title ladder; value and change start one step smaller.
enum TileSize : uint8_t {
    TILE_SIZE_AUTO = 0,    // the largest step that fits
    TILE_SIZE_L,           // the same as auto today
    TILE_SIZE_M,           // 18 pt at most
    TILE_SIZE_S,           // 12 pt at most
    TILE_SIZE_COUNT
};
// Font steps, largest first: 18 pt x2, 12 pt x2, 18, 12, 9 pt.
enum TileFontStep : uint8_t { TILE_F18X2 = 0, TILE_F12X2, TILE_F18, TILE_F12, TILE_F9, TILE_F_COUNT };
// Height of the capitals / digits of each step in px (18 pt digits are 25-26 px tall).
extern const uint8_t tile_step_h[TILE_F_COUNT];

struct TileStyle {
    uint8_t look;          // TileLook
    uint8_t size;          // TileSize
    bool    invert;        // white on black for the whole cell
};
// A style object as written: which keys were given (root defaults -> tile overrides).
struct TileStyleSpec {
    TileStyle st;
    bool has_look, has_size, has_invert;
};
struct TileRect { int16_t x, y, w, h; };

struct Tile {
    bool      has;                       // a title, value or change is present; false = empty cell (white)
    char      title[TILE_TITLE_MAX];
    char      value[TILE_VALUE_MAX];
    char      change[TILE_CHANGE_MAX];   // "" = no change
    TickerDir dir;
    bool      has_dir;
    TileStyle style;                     // resolved (root defaults applied)
};
struct TilesFrame {
    uint8_t  layout;                     // TileLayout, never SINGLE
    uint8_t  n;                          // cells of the layout (tiles_layout_cells)
    bool     status;                     // the status band (badges + age line) in the bottom-right cell
    Tile     t[TILES_MAX];
    uint32_t age_s;                      // age of the content when received; TICKER_AGE_UNKNOWN = none
};

// Names <-> values ("1x1" | "1x2" | "2x1" | "2x2", "single"; "stack" | "row" |
// "ticker"; "auto" | "l" | "m" | "s"). The parse functions return false on an
// unknown string and leave *out untouched.
const char* tile_layout_str(uint8_t layout);
bool        tile_layout_parse(const char* s, uint8_t* out);
const char* tile_look_str(uint8_t look);
bool        tile_look_parse(const char* s, uint8_t* out);
const char* tile_size_str(uint8_t size);
bool        tile_size_parse(const char* s, uint8_t* out);

// Cells of a preset: 1, 2, 2, 4 (0 for SINGLE / unknown).
uint8_t tiles_layout_cells(uint8_t layout);
// The preset `tiles` defaults to when the payload names none: 1x1 for one
// tile, 1x2 for two, 2x2 otherwise (also for an empty array).
uint8_t tiles_layout_default(uint8_t tiles_n);
// Cell `idx` of a preset (0..cells-1, row-major: top-left, top-right,
// bottom-left, bottom-right) without the 1 px separators, which run at
// x = 148 and / or y = 64. Returns false for an index outside the preset.
bool tiles_cell_rect(uint8_t layout, uint8_t idx, TileRect* out);
// The cell that carries the status band: the bottom-right one.
uint8_t tiles_status_cell(uint8_t layout);
// The drawable part of a cell: the status band (TILES_STATUS_H rows) is
// taken from the bottom-right cell when `status` is on; every other cell
// is returned whole.
void tiles_usable(uint8_t layout, uint8_t idx, bool status, TileRect* out);
// A cell too low for the stack look's normal bottom row (its value 12 pt,
// change 12 pt): under 60 px the row is 9 pt only, same placement.
bool tiles_cramped(const TileRect* usable);

// Geometry of a cell's rows for a look, relative to the usable rect's top
// (y offsets) and inside its padding. rows: 1 = the row look (one band),
// 2 = title band + bottom row (value left, change right), 3 = title, value,
// change bands (a cell of 96 px or more). `frame_h` is the smallest usable
// height among the frame's cells (tiles_frame_min_h): every three-row cell
// is laid out from it, so the rows of a 1x2 align and both cells get the
// same sizes whether or not they carry the status band. `status` = the
// frame's status band is on: the cells next to it keep a white margin above
// it (BAND_MARGIN / BAND_MARGIN_CR rows), the three-row title band is 40 %
// instead of 45 % so the value and the change keep 18 pt.
struct TileGeom {
    uint8_t rows;
    bool    cramped;
    int16_t title_top, title_h;
    int16_t value_top, value_h;
    int16_t change_top, change_h;
};
void tiles_geometry(uint8_t look, const TileRect* usable, int16_t frame_h, bool status, TileGeom* out);
// The smallest usable height of a frame's cells (the bottom-right cell's).
int16_t tiles_frame_min_h(uint8_t layout, bool status);

// Font ladders of the three elements for a look / size / geometry: steps
// largest first, only those whose height fits the band; the title starts
// at the size cap, value and change one step smaller; TILE_F9 is always
// the last entry, so a list is never empty.
struct TileSteps {
    uint8_t title[TILE_F_COUNT],  title_n;
    uint8_t value[TILE_F_COUNT],  value_n;
    uint8_t change[TILE_F_COUNT], change_n;
};
void tiles_steps(uint8_t look, uint8_t size, const TileGeom* g, TileSteps* out);

// Width of `text` drawn with `step` (hal_display measures on the canvas, the
// tests sum xAdvance tables).
typedef int16_t (*TilesMeasureFn)(const char* text, uint8_t step, void* ctx);
// Steps of up to three texts on one row of `max_w`: every element starts at
// the largest step of its list; while the sum of the widths (+ `gap` between
// elements, + `extra` for the triangle when there is a change) exceeds
// max_w, the change is shrunk first, then the value, then the title. NULL or
// "" skips an element (its step index is left 0). Returns false when even
// the smallest steps do not fit - the drawer truncates the title then.
bool tiles_fit_row(TilesMeasureFn measure, void* ctx, const char* title, const char* value,
                   const char* change, const TileSteps* s, int16_t max_w, int16_t gap, int16_t extra,
                   uint8_t* ti, uint8_t* vi, uint8_t* ci);

// The resolved style of a tile: the defaults (stack, auto, not inverted),
// then the root object's keys, then the tile's own.
void tiles_style_resolve(const TileStyleSpec* root, const TileStyleSpec* tile, TileStyle* out);

// The frame key of the refresh policy (docs/DEVICE_UI.md "E-ink refresh rules"): FNV-1a over
// the layout, the status flag and, per cell, has / invert / look / size and
// the title - another value is the same frame (partial), another set of
// names, a toggled inversion or another preset is a layout change (full).
// Seeded apart from grid_set_key(): a ticker grid and a tile frame with the
// same names never share a key.
uint32_t tiles_frame_key(const TilesFrame* f);
// The titles of the filled cells, comma-separated (the shelf's Showing line).
void tiles_join_titles(const TilesFrame* f, char* buf, size_t cap);
