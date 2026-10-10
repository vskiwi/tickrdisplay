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
