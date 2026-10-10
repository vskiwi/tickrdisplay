#pragma once
// Ticker look (docs/TICKERS.md "What the screen shows") - the pure parts, unit-tested on the
// host (test/test_ticker): direction from a formatted change string, the
// age line, the sparkline scaling and the LED rule. No Arduino, no drawing:
// hal_display draws the frame from a TickerFields, renderer.cpp fills it
// from a ScreenPayload and applies the LED rule.
#include <stdint.h>
#include <stddef.h>

#define TICKER_CHANGE_MAX   16     // "+1234.56%" and the like
#define TICKER_TIME_MAX     16     // pass-through time string (MOEX "19:00")
#define TICKER_SPARK_MAX    48     // points drawn; extra points are ignored
#define TICKER_SPARK_H      24     // sparkline box height in px (200 x 24)
#define TICKER_SHORT_MAX    8      // short name on the badge: up to 7 chars ("BTC", "BITCOIN")
#define TICKER_AGE_UNKNOWN  0xFFFFFFFFu

// Direction of the change: -1 down, 0 flat, +1 up (payload "dir").
typedef int8_t TickerDir;

// The optional ticker fields of a payload after parsing / scaling. `ticker`
// selects the ticker layout: set when the payload carried `change`
// or `spark[]`; a plain title/value payload keeps the Text look.
struct TickerFields {
    bool     ticker;
    char     change[TICKER_CHANGE_MAX];   // "" = no change line
    TickerDir dir;
    uint32_t age_s;                       // age of the quote when it was received; TICKER_AGE_UNKNOWN = no age line
    char     time[TICKER_TIME_MAX];       // "" = relative age instead
    uint8_t  spark[TICKER_SPARK_MAX];     // rows 0 (bottom) .. TICKER_SPARK_H-1 (top)
    uint8_t  spark_n;                     // 0 or 1 = nothing to draw
    char     short_label[TICKER_SHORT_MAX]; // "" = no badge, the title row as for a text frame
};

// Sign of a formatted change string: leading '+' / '-' / U+2212 (after
// blanks), else the sign of the number it starts with; unparsable or zero -> 0.
TickerDir ticker_dir_from_change(const char* change);

// Scales `n` values to 0..height-1 by the min-max of the window.
// Non-finite values are skipped; a flat series (or all skipped) is drawn as a
// mid line. Returns the number of points written (<= TICKER_SPARK_MAX).
uint8_t ticker_spark_scale(const float* in, size_t n, uint8_t* out, uint8_t height);

// Age line for the ticker frame: "just now" (under a minute) / "5 min ago" /
// "2 h ago" / "3 d ago"; with `stale` the line reads "stale 40 min".
// `age_s` = TICKER_AGE_UNKNOWN -> "" (no line).
void ticker_age_line(uint32_t age_s, bool stale, char* buf, size_t len);

// Stale crossing: the one refresh the marker is allowed when
// the age of the shown ticker reaches T_stale, so "stale N min" appears on
// its own instead of waiting for an unrelated refresh (a dead proxy on USB
// never brings one). `fired` is the caller's per-content flag - cleared when
// new content arrives; `shows_age` = the frame draws a relative age line (a
// ticker without a pass-through `time`). Returns true exactly once per
// crossing; the caller folds it into the badge policy (<= 1 badge-only
// refresh per minute, docs/DEVICE_UI.md "E-ink refresh rules").
bool ticker_stale_crossing(bool* fired, bool shows_age, uint32_t age_s, uint32_t stale_s);

// The price without its fraction (docs/TICKERS.md "What the screen shows"):
// hal_display asks for it when the whole string misses the largest size of
// the price cascade. Accepts only `[-]digits[.digits]` with an optional
// thousands separator (space or comma, groups of three) and an integer part
// of at least four digits (>= 1 000); rounds half-up on the first fraction
// digit, the carry re-grouped with the same separator ("9 999.99" -> "10 000").
// Anything else - a value under 1 000, no fraction, a currency sign, a proxy's
// free text - returns false and leaves `out` empty. `cap` is the size of `out`.
bool ticker_price_trim_round(const char* in, char* out, size_t cap);

// --- 2x2 grid (docs/TICKERS.md "Several tickers on one panel: the 2x2 grid") ---
// Up to GRID_MAX sources drawn side by side in cells of 148 x 64 px; one
// frame per fetch cycle. hal_display draws a GridFrame, connectivity_manager
// fills it (a source that failed this cycle keeps its last price with
// ok = false, drawn as "?" in place of the change).
#define GRID_MAX           4
#define GRID_PRICE_MAX     24     // = SRC_PRICE_MAX (source.h is not included here)
#define GRID_SYMBOL_MAX    32     // = SRC_SYMBOL_MAX
#define GRID_BATT_MIN_MIN  15     // floor of the on-battery interval with a grid (minutes)
#define GRID_CELL_W        148
#define GRID_CELL_H        64

struct GridCell {
    char      short_label[TICKER_SHORT_MAX]; // badge text; "" = the first 7 chars of the label
    char      symbol[GRID_SYMBOL_MAX];       // the configured symbol (/api/screen/state symbols[])
    char      price[GRID_PRICE_MAX];         // "" = never fetched
    char      change[TICKER_CHANGE_MAX];     // "" = no change line
    TickerDir dir;
    bool      ok;                            // fetched in this cycle; false = "?" instead of the change
    bool      has;                           // a price is known (this cycle or an earlier one)
};
struct GridFrame {
    GridCell cells[GRID_MAX];
    uint8_t  n;                              // 1..GRID_MAX tickers in cells 0..n-1
    uint32_t age_s;                          // age of the oldest quote fetched in this boot; TICKER_AGE_UNKNOWN = none
};

// Top-left corner of cell `idx` (0..3: Q1 top-left, Q2 top-right, Q3
// bottom-left, Q4 bottom-right); the 1 px separators run at x = 148 and
// y = 64, so the right / lower cells start one pixel later.
void grid_cell_origin(uint8_t idx, int16_t* x, int16_t* y);
// Cells drawn as tickers for `n` sources: n (1..4); the Q4 cell carries the
// age line and the badges, and shows a ticker only with four sources.
uint8_t grid_ticker_cells(uint8_t n);
// A key of the set: FNV-1a over n and the short names in order - another
// set or order is a layout change (full refresh), as another title is for
// the single view (docs/DEVICE_UI.md "E-ink refresh rules").
uint32_t grid_set_key(const GridFrame* g);
// The on-battery interval with a grid is at least GRID_BATT_MIN_MIN
// minutes (up to four TLS fetches per wake); `grid` = the grid view with
// two or more sources. Anything else passes through.
uint32_t grid_battery_interval_min(bool grid, uint32_t interval_min);
// Price fit in a cell (docs/TICKERS.md "Several tickers on one panel: the 2x2 grid"):
// step 0 = 18 pt whole, 1 = 18 pt without the fraction (ticker_price_trim_round,
// so only from 1 000), 2 = 12 pt whole, 3 = 9 pt whole - the first whose
// width (`measure(text, font_step)`, font_step 0 = 18 pt, 1 = 12 pt, 2 = 9 pt)
// is <= max_w; step 3 when nothing fits. Returns the step; *out points at
// `price` or at `buf` (the trimmed text). `ctx` is handed to `measure`.
typedef int16_t (*GridMeasureFn)(const char* text, uint8_t font_step, void* ctx);
uint8_t grid_fit_price(const char* price, int16_t max_w, GridMeasureFn measure, void* ctx,
                       char* buf, size_t cap, const char** out);

// LED rule (docs/TICKERS.md "What the screen shows"): the device setting.
enum LedRule : uint8_t {
    LED_RULE_OFF  = 0,   // the payload's alert.led as today (default)
    LED_RULE_SIGN = 1,   // red on down, green on up, unchanged on flat
};
// "off" | "sign" <-> value (unknown strings -> off).
const char* led_rule_str(uint8_t v);
uint8_t     led_rule_parse(const char* s);
// Decides the LED base colour for one payload. An explicit alert.led wins;
// otherwise `sign` colours by `dir` when the payload carries a direction
// (has_dir). Returns true when rgb should be written.
bool ticker_led_decide(uint8_t rule, bool has_led, uint8_t pr, uint8_t pg, uint8_t pb,
                       bool has_dir, TickerDir dir, uint8_t* r, uint8_t* g, uint8_t* b);
