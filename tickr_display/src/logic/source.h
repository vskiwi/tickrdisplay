#pragma once
// Ticker source (docs/TICKERS.md "Presets", "Custom JSON") - the pure parts,
// unit-tested on the host (test/test_source): the preset table, the resolved
// fetch plan (URL template + paths), the mini-JSONPath walk over an
// ArduinoJson document, number parsing and formatting. No Arduino, no HTTP:
// connectivity_manager fetches the body and feeds it to source_extract(),
// then renders the result through the existing ScreenPayload path.
//
// Mini-JSONPath (a subset, no recursion, ~40 lines):
//   $.a.b        dotted keys (the leading "$" and "." are optional)
//   a[0].c       array index
//   $.result.*   "*" = the first member of an object / the first element of
//                an array (Kraken's result key differs from the pair name)
// Keys are up to SRC_PATH_KEY_MAX-1 bytes and may not contain '.', '[', ']'.
#include <stdint.h>
#include <stddef.h>
#include "ticker.h"

#define SRC_SYMBOL_MAX   32    // CoinGecko ids are slugs ("wrapped-bitcoin")
#define SRC_MARKET_MAX   12
#define SRC_URL_CFG_MAX  128   // custom URL as configured (may contain {s} / {m})
#define SRC_URL_MAX      176   // expanded URL
#define SRC_PATH_MAX     32
#define SRC_PATH_KEY_MAX 24
#define SRC_LABEL_MAX    32
#define SRC_PRICE_MAX    24    // formatted price ("12 345 678.12")
#define SRC_NUM_MAX      24    // raw decimal text of a number
#define SRC_ERR_MAX      24
#define SRC_DECIMALS_AUTO 255
#define SRC_DECIMALS_MAX  6

// What the device shows content from (AppConfig::source_kind, schema 8).
// Only `ticker` changes the firmware's behaviour (the pull target); the
// others record the editor's choice (docs/WEB_UI.md "Change what a device shows").
enum SourceKind : uint8_t { SRC_KIND_NONE = 0, SRC_KIND_TEXT, SRC_KIND_URL, SRC_KIND_MQTT, SRC_KIND_TICKER };
// Preset = URL template + paths + change mode (firmware table, source.cpp).
// The two Binance futures presets (USDS-M on fapi, COIN-M on dapi - docs/TICKERS.md
// "Binance futures") add a second request for the funding rate, shown in place of the age line.
enum SourcePreset : uint8_t {
    SRC_PRESET_COINGECKO = 0, SRC_PRESET_KRAKEN, SRC_PRESET_BINANCE,
    SRC_PRESET_BINANCE_USDM, SRC_PRESET_BINANCE_COINM,
    SRC_PRESET_CUSTOM, SRC_PRESET_COUNT
};
// How `path_change` is read: a percentage, or the open price the change is computed from.
enum SourceChangeMode : uint8_t { SRC_CHG_PCT = 0, SRC_CHG_OPEN = 1 };
// Thousands separator of the price. The e-ink fonts are ASCII only, so the
// "thin space" of the design is a plain space on the device.
enum SourceSep : uint8_t { SRC_SEP_SPACE = 0, SRC_SEP_COMMA, SRC_SEP_NONE };

const char* source_kind_str(uint8_t v);       uint8_t source_kind_parse(const char* s);      // unknown -> none
// The pull target: `ticker` -> ticker; else a non-empty pull_url -> url
// (this is also the schema < 8 migration, where no kind is stored: pull_url
// set -> url, else none); else none. text / mqtt never pull.
uint8_t source_pull_kind(uint8_t kind, bool has_pull_url);
const char* source_preset_str(uint8_t v);     uint8_t source_preset_parse(const char* s);    // unknown -> custom
const char* source_sep_str(uint8_t v);        uint8_t source_sep_parse(const char* s);       // unknown -> space
const char* source_change_mode_str(uint8_t v); uint8_t source_change_mode_parse(const char* s);  // unknown -> pct

// The user's configuration (AppConfig fields, or the body of POST /api/source/test).
struct SourceSpec {
    uint8_t preset;                       // SourcePreset
    char    symbol[SRC_SYMBOL_MAX];       // "BTC", "XBT", "bitcoin"
    char    market[SRC_MARKET_MAX];       // "USDT", "USD", "usd"
    char    url[SRC_URL_CFG_MAX];         // custom only
    char    path_price[SRC_PATH_MAX];     // custom only
    char    path_change[SRC_PATH_MAX];    // custom only, "" = no change line
    char    path_spark[SRC_PATH_MAX];     // custom only, "" = device history
    uint8_t change_mode;                  // custom only (presets know theirs)
    uint8_t decimals;                     // SRC_DECIMALS_AUTO or 0..6
    uint8_t sep;                          // SourceSep
    char    label[SRC_LABEL_MAX];         // "" = "<symbol>/<market> - <Preset>"
    char    short_label[TICKER_SHORT_MAX]; // badge text, "" = source_short_default()
};

// The fetch plan a spec resolves to.
struct SourcePlan {
    char    url[SRC_URL_MAX];
    char    path_price[SRC_PATH_MAX];
    char    path_change[SRC_PATH_MAX];
    char    path_spark[SRC_PATH_MAX];
    uint8_t change_mode;
    uint8_t decimals;
    uint8_t sep;
    char    label[SRC_LABEL_MAX];
    char    short_label[TICKER_SHORT_MAX]; // the spec's, else the default; "" = no badge
    bool    https;
    // Second request of the futures presets: the funding rate ("" = none).
    // Same host and scheme as `url`; fetched after the price, its failure
    // does not fail the fetch (the line keeps its last text).
    char    url_funding[SRC_URL_MAX];
    char    path_funding[SRC_PATH_MAX];
};

// Fills the defaults of a spec (CoinGecko, auto decimals, space separator).
void source_spec_defaults(SourceSpec* s);
// The short name drawn on the badge when the spec has none (docs/TICKERS.md
// "What the screen shows"): exchange presets and Custom - the symbol
// upper-cased ("btc" -> "BTC", Kraken's "XBT" stays), a pair typed into the
// symbol with an empty market loses a known quote currency ("BTCUSDT" ->
// "BTC"); CoinGecko - a table of common ids ("bitcoin" -> "BTC"), an unknown
// id upper-cased. Cut to TICKER_SHORT_MAX-1 chars; "" without a symbol.
void source_short_default(const SourceSpec& s, char* out, size_t n);
// Expands the preset (or the custom URL: "{s}" / "{m}" replaced by symbol /
// market) into a plan. False when the spec cannot be fetched: no symbol for a
// preset, no URL or no price path for custom.
bool source_resolve(const SourceSpec& s, SourcePlan* out);

struct SourceResult {
    char      price[SRC_PRICE_MAX];        // formatted for the screen
    char      change[TICKER_CHANGE_MAX];   // "+0.07%" / "-1.23%" / "0.00%"; "" = no change line
    bool      has_change;
    TickerDir dir;                         // from the change (0 without one)
    float     price_f;                     // for the sparkline history
    uint8_t   spark_n;                     // points from path_spark (custom), else 0
    float     spark[TICKER_SPARK_MAX];
    char      funding[TICKER_TIME_MAX];    // "FR +0.0100%" (futures presets), "" = none / not fetched
};

enum SourceError : uint8_t {
    SRC_OK = 0,
    SRC_ERR_EMPTY,        // no body
    SRC_ERR_JSON,         // the body is not JSON / does not fit the document
    SRC_ERR_PRICE_PATH,   // nothing at path_price
    SRC_ERR_PRICE_NUM,    // the value there is not a number
    SRC_ERR_CHANGE_PATH,  // nothing at path_change
    SRC_ERR_CHANGE_NUM,   // the value there is not a number (or open = 0)
    SRC_ERR_NOMEM,
};
// Parses `json` (<= PAYLOAD_MAX_LEN bytes, need not be NUL-terminated) and
// extracts / formats price and change per the plan. Bounded: one 4 KB
// document, the existing ArduinoJson reader instantiation, no recursion.
SourceError source_extract(const char* json, size_t len, const SourcePlan& p, SourceResult* out);
const char* source_error_str(SourceError e);   // short, for last_error / the Test button

// The funding-rate body (Binance premiumIndex: an object on fapi, a one-element
// array on dapi) -> the ticker's time line, "FR +0.0100%": the rate is a
// fraction (0.0001 = 0.01 %), shown as a percentage with 4 fraction digits
// and an explicit sign (also "+0.0000%"); clamped to +-9.9999 %. `out` gets ""
// and an error when the body is not JSON, `path` finds nothing or not a number.
SourceError source_extract_funding(const char* json, size_t len, const char* path, char* out, size_t n);
// Pure piece of the above: fraction -> "FR +0.0100%" text. False when `n` is too small.
bool source_format_funding(float rate, char* out, size_t n);

// --- pure pieces --------------------------------------------------------
// Decimal text -> float without strtod ("84000.06", "-0.5", " 12"); false on
// anything else (exponents, hex, empty, trailing garbage).
bool source_parse_num(const char* s, float* out);
// Formats a decimal text: rounds (half up) to `decimals` fraction digits -
// SRC_DECIMALS_AUTO: >= 1 000 000 -> 0, >= 1 -> 2, >= 0.01 -> 4, else 6 -
// and inserts the thousands separator. False when `in` is not a plain
// decimal number or `out` is too small.
bool source_format_price(const char* in, uint8_t decimals, uint8_t sep, char* out, size_t n);
// "+0.07%" / "-1.23%" / "0.00%" (no sign when it rounds to zero) and the direction.
TickerDir source_format_pct(float pct, char* out, size_t n);
