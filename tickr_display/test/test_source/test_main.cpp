// Unity tests for the ticker source (src/logic/source.cpp) - docs/TICKERS.md
// "Custom JSON" (mini-JSONPath + extraction), number formatting, presets.
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "logic/source.h"
#include "logic/payload.h"   // PAYLOAD_MAX_LEN
#include "fixtures.h"

void setUp(void) {}
void tearDown(void) {}

static SourceSpec spec(uint8_t preset, const char* sym, const char* mkt) {
    SourceSpec s;
    source_spec_defaults(&s);
    s.preset = preset;
    snprintf(s.symbol, sizeof(s.symbol), "%s", sym);
    snprintf(s.market, sizeof(s.market), "%s", mkt);
    return s;
}

static SourceSpec custom_paths(const char* price, const char* change) {
    SourceSpec s = spec(SRC_PRESET_CUSTOM, "", "");
    snprintf(s.url, sizeof(s.url), "%s", "http://p/q");
    snprintf(s.path_price, sizeof(s.path_price), "%s", price);
    snprintf(s.path_change, sizeof(s.path_change), "%s", change ? change : "");
    return s;
}

static SourceError run(const SourceSpec& s, const char* json, SourceResult* r, SourcePlan* plan = nullptr) {
    SourcePlan p;
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    if (plan) *plan = p;
    return source_extract(json, strlen(json), p, r);
}

// --- enums ------------------------------------------------------------------

void test_enum_strings_round_trip(void) {
    TEST_ASSERT_EQUAL_STRING("ticker", source_kind_str(SRC_KIND_TICKER));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_URL, source_kind_parse("url"));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_NONE, source_kind_parse("bogus"));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_NONE, source_kind_parse(NULL));
    TEST_ASSERT_EQUAL_STRING("kraken", source_preset_str(SRC_PRESET_KRAKEN));
    TEST_ASSERT_EQUAL_UINT8(SRC_PRESET_BINANCE, source_preset_parse("binance"));
    TEST_ASSERT_EQUAL_UINT8(SRC_PRESET_BINANCE_USDM, source_preset_parse("binance_usdm"));
    TEST_ASSERT_EQUAL_UINT8(SRC_PRESET_BINANCE_COINM, source_preset_parse("binance_coinm"));
    TEST_ASSERT_EQUAL_STRING("binance_coinm", source_preset_str(SRC_PRESET_BINANCE_COINM));
    TEST_ASSERT_EQUAL_UINT8(SRC_PRESET_CUSTOM, source_preset_parse("yahoo"));
    TEST_ASSERT_EQUAL_STRING("custom", source_preset_str(99));
    TEST_ASSERT_EQUAL_UINT8(SRC_SEP_COMMA, source_sep_parse("comma"));
    TEST_ASSERT_EQUAL_STRING("space", source_sep_str(77));
    TEST_ASSERT_EQUAL_UINT8(SRC_CHG_OPEN, source_change_mode_parse("open"));
    TEST_ASSERT_EQUAL_STRING("pct", source_change_mode_str(SRC_CHG_PCT));
}

void test_pull_kind_and_schema7_migration(void) {
    // ticker wins; else the pull_url decides (= the migration of a schema < 8 file)
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_TICKER, source_pull_kind(SRC_KIND_TICKER, false));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_URL, source_pull_kind(SRC_KIND_NONE, true));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_URL, source_pull_kind(SRC_KIND_TEXT, true));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_NONE, source_pull_kind(SRC_KIND_NONE, false));
    TEST_ASSERT_EQUAL_UINT8(SRC_KIND_NONE, source_pull_kind(SRC_KIND_MQTT, false));
}

// --- number parsing ---------------------------------------------------------

void test_parse_num_accepts_plain_decimals(void) {
    float v = 0;
    TEST_ASSERT_TRUE(source_parse_num("84000.06", &v));   TEST_ASSERT_FLOAT_WITHIN(0.01f, 84000.06f, v);
    TEST_ASSERT_TRUE(source_parse_num("-1.5", &v));       TEST_ASSERT_FLOAT_WITHIN(0.0001f, -1.5f, v);
    TEST_ASSERT_TRUE(source_parse_num(" 12 ", &v));       TEST_ASSERT_FLOAT_WITHIN(0.0001f, 12.0f, v);
    TEST_ASSERT_TRUE(source_parse_num("+0.070", &v));     TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.07f, v);
    TEST_ASSERT_TRUE(source_parse_num(".5", &v));         TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, v);
}

void test_parse_num_rejects_garbage(void) {
    float v = 0;
    TEST_ASSERT_FALSE(source_parse_num("1e3", &v));
    TEST_ASSERT_FALSE(source_parse_num("0x10", &v));
    TEST_ASSERT_FALSE(source_parse_num("", &v));
    TEST_ASSERT_FALSE(source_parse_num("-", &v));
    TEST_ASSERT_FALSE(source_parse_num("12abc", &v));
    TEST_ASSERT_FALSE(source_parse_num("n/a", &v));
    TEST_ASSERT_FALSE(source_parse_num(NULL, &v));
}

// --- price formatting ---------------------------------------------------------

void test_format_auto_decimals_by_magnitude(void) {
    char b[SRC_PRICE_MAX];
    TEST_ASSERT_TRUE(source_format_price("84000.06000000", SRC_DECIMALS_AUTO, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("84 000.06", b);                                          // >= 1: 2 decimals, thin space
    TEST_ASSERT_TRUE(source_format_price("12700000", SRC_DECIMALS_AUTO, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("12 700 000", b);                                         // >= 1 000 000: none
    TEST_ASSERT_TRUE(source_format_price("0.5", SRC_DECIMALS_AUTO, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("0.5000", b);                                             // >= 0.01: 4
    TEST_ASSERT_TRUE(source_format_price("0.012345", SRC_DECIMALS_AUTO, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("0.0123", b);
    TEST_ASSERT_TRUE(source_format_price("0.00001234", SRC_DECIMALS_AUTO, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("0.000012", b);                                           // < 0.01: 6
    TEST_ASSERT_TRUE(source_format_price("1.5", SRC_DECIMALS_AUTO, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1.50", b);
}

void test_format_fixed_decimals_and_separators(void) {
    char b[SRC_PRICE_MAX];
    TEST_ASSERT_TRUE(source_format_price("1234567.891", 1, SRC_SEP_COMMA, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1,234,567.9", b);
    TEST_ASSERT_TRUE(source_format_price("1234567.891", 0, SRC_SEP_NONE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1234568", b);
    TEST_ASSERT_TRUE(source_format_price("999.5", 0, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1 000", b);                                              // carry into a new digit
    TEST_ASSERT_TRUE(source_format_price("999.996", 2, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1 000.00", b);
    TEST_ASSERT_TRUE(source_format_price("0.6", 0, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1", b);
    TEST_ASSERT_TRUE(source_format_price("5", 3, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("5.000", b);                                              // padded
    TEST_ASSERT_TRUE(source_format_price("000123.450", 2, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("123.45", b);                                             // leading zeros dropped
    TEST_ASSERT_TRUE(source_format_price("-0.001", 2, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("0.00", b);                                               // no "-0.00"
    TEST_ASSERT_TRUE(source_format_price("-12.5", 1, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("-12.5", b);
    TEST_ASSERT_TRUE(source_format_price("84000.0625", 9, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("84 000.062500", b);                                      // decimals clamped to 6
}

void test_format_rejects_non_numbers_and_small_buffers(void) {
    char b[SRC_PRICE_MAX], tiny[6];
    TEST_ASSERT_FALSE(source_format_price("abc", 2, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_FALSE(source_format_price("", 2, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_FALSE(source_format_price("1e5", 2, SRC_SEP_SPACE, b, sizeof(b)));
    TEST_ASSERT_FALSE(source_format_price("1234567890123456", 0, SRC_SEP_SPACE, b, sizeof(b)));   // 16 integer digits
    TEST_ASSERT_FALSE(source_format_price("84000.06", 2, SRC_SEP_SPACE, tiny, sizeof(tiny)));
    TEST_ASSERT_FALSE(source_format_price(NULL, 2, SRC_SEP_SPACE, b, sizeof(b)));
}

void test_format_pct_sign_and_direction(void) {
    char b[TICKER_CHANGE_MAX];
    TEST_ASSERT_EQUAL_INT(1,  source_format_pct(0.07f, b, sizeof(b)));    TEST_ASSERT_EQUAL_STRING("+0.07%", b);
    TEST_ASSERT_EQUAL_INT(-1, source_format_pct(-1.2345f, b, sizeof(b))); TEST_ASSERT_EQUAL_STRING("-1.23%", b);
    TEST_ASSERT_EQUAL_INT(0,  source_format_pct(0.004f, b, sizeof(b)));   TEST_ASSERT_EQUAL_STRING("0.00%", b);   // rounds to zero: flat, no sign
    TEST_ASSERT_EQUAL_INT(1,  source_format_pct(0.005f, b, sizeof(b)));   TEST_ASSERT_EQUAL_STRING("+0.01%", b);
    TEST_ASSERT_EQUAL_INT(-1, source_format_pct(-12.5f, b, sizeof(b)));   TEST_ASSERT_EQUAL_STRING("-12.50%", b);
    TEST_ASSERT_EQUAL_INT(1,  source_format_pct(1e9f, b, sizeof(b)));     TEST_ASSERT_EQUAL_STRING("+99999.00%", b);    // clamped
    // the direction agrees with what the ticker code derives from the string
    TEST_ASSERT_EQUAL_INT(-1, source_format_pct(-1.2345f, b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(-1, ticker_dir_from_change(b));
    TEST_ASSERT_EQUAL_INT(0,  source_format_pct(0.0f, b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(0,  ticker_dir_from_change(b));
}

// --- presets: resolve -------------------------------------------------------------

void test_resolve_binance_upper_cases_symbol(void) {
    SourcePlan p;
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE, "btc", "usdt"), &p));
    TEST_ASSERT_EQUAL_STRING("https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT", p.url);
    TEST_ASSERT_EQUAL_STRING("$.lastPrice", p.path_price);
    TEST_ASSERT_EQUAL_STRING("$.priceChangePercent", p.path_change);
    TEST_ASSERT_EQUAL_UINT8(SRC_CHG_PCT, p.change_mode);
    TEST_ASSERT_EQUAL_STRING("BTC/USDT - Binance", p.label);
    TEST_ASSERT_TRUE(p.https);
    TEST_ASSERT_EQUAL_UINT8(SRC_DECIMALS_AUTO, p.decimals);
}

void test_resolve_binance_futures_urls_labels_and_funding(void) {
    SourcePlan p;
    // USDS-M perpetual: fapi, spot-like paths, "BTC/USDT PERP - Binance", a funding request
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE_USDM, "btc", "usdt"), &p));
    TEST_ASSERT_EQUAL_STRING("https://fapi.binance.com/fapi/v1/ticker/24hr?symbol=BTCUSDT", p.url);
    TEST_ASSERT_EQUAL_STRING("$.lastPrice", p.path_price);
    TEST_ASSERT_EQUAL_STRING("$.priceChangePercent", p.path_change);
    TEST_ASSERT_EQUAL_UINT8(SRC_CHG_PCT, p.change_mode);
    TEST_ASSERT_EQUAL_STRING("BTC/USDT PERP - Binance", p.label);
    TEST_ASSERT_EQUAL_STRING("https://fapi.binance.com/fapi/v1/premiumIndex?symbol=BTCUSDT", p.url_funding);
    TEST_ASSERT_EQUAL_STRING("$.lastFundingRate", p.path_funding);
    TEST_ASSERT_TRUE(p.https);
    // USDS-M quarterly: the market carries the expiry, the label shows it instead of
    // PERP, and there is no funding on a delivery contract (the age line stays)
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE_USDM, "BTC", "usdt_261225"), &p));
    TEST_ASSERT_EQUAL_STRING("https://fapi.binance.com/fapi/v1/ticker/24hr?symbol=BTCUSDT_261225", p.url);
    TEST_ASSERT_EQUAL_STRING("BTC/USDT 261225 - Binance", p.label);
    TEST_ASSERT_EQUAL_STRING("", p.url_funding);
    TEST_ASSERT_EQUAL_STRING("", p.path_funding);
    // COIN-M: dapi, array paths, the pair as one word
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE_COINM, "btc", "usd_perp"), &p));
    TEST_ASSERT_EQUAL_STRING("https://dapi.binance.com/dapi/v1/ticker/24hr?symbol=BTCUSD_PERP", p.url);
    TEST_ASSERT_EQUAL_STRING("$[0].lastPrice", p.path_price);
    TEST_ASSERT_EQUAL_STRING("$[0].priceChangePercent", p.path_change);
    TEST_ASSERT_EQUAL_STRING("BTCUSD PERP - Binance", p.label);
    TEST_ASSERT_EQUAL_STRING("https://dapi.binance.com/dapi/v1/premiumIndex?symbol=BTCUSD_PERP", p.url_funding);
    TEST_ASSERT_EQUAL_STRING("$[0].lastFundingRate", p.path_funding);
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE_COINM, "ETH", "USD_261225"), &p));
    TEST_ASSERT_EQUAL_STRING("ETHUSD 261225 - Binance", p.label);
    TEST_ASSERT_EQUAL_STRING("", p.url_funding);
    // "_PERP" is not a date: the perpetual keeps its funding request
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE_COINM, "ETH", "USD_PERP"), &p));
    TEST_ASSERT_EQUAL_STRING("https://dapi.binance.com/dapi/v1/premiumIndex?symbol=ETHUSD_PERP", p.url_funding);
    // a user label wins; spot and custom have no funding request
    SourceSpec s = spec(SRC_PRESET_BINANCE_USDM, "BTC", "USDT");
    snprintf(s.label, sizeof(s.label), "%s", "BTC perp");
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    TEST_ASSERT_EQUAL_STRING("BTC perp", p.label);
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE, "BTC", "USDT"), &p));
    TEST_ASSERT_EQUAL_STRING("", p.url_funding);
    TEST_ASSERT_TRUE(source_resolve(custom_paths("$.a", nullptr), &p));
    TEST_ASSERT_EQUAL_STRING("", p.url_funding);
}

void test_resolve_coingecko_lower_cases_and_paths_carry_market(void) {
    SourcePlan p;
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_COINGECKO, "Bitcoin", "USD"), &p));
    TEST_ASSERT_EQUAL_STRING("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true", p.url);
    TEST_ASSERT_EQUAL_STRING("$.*.usd", p.path_price);
    TEST_ASSERT_EQUAL_STRING("$.*.usd_24h_change", p.path_change);
    TEST_ASSERT_EQUAL_STRING("bitcoin/usd - CoinGecko", p.label);
}

void test_resolve_kraken_open_mode_and_custom_label(void) {
    SourceSpec s = spec(SRC_PRESET_KRAKEN, "XBT", "USD");
    snprintf(s.label, sizeof(s.label), "%s", "Bitcoin");
    SourcePlan p;
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    TEST_ASSERT_EQUAL_STRING("https://api.kraken.com/0/public/Ticker?pair=XBTUSD", p.url);
    TEST_ASSERT_EQUAL_STRING("$.result.*.c[0]", p.path_price);
    TEST_ASSERT_EQUAL_STRING("$.result.*.o", p.path_change);
    TEST_ASSERT_EQUAL_UINT8(SRC_CHG_OPEN, p.change_mode);
    TEST_ASSERT_EQUAL_STRING("Bitcoin", p.label);
}

void test_resolve_custom_expands_placeholders_and_validates(void) {
    SourceSpec s = spec(SRC_PRESET_CUSTOM, "SBER", "");
    snprintf(s.url, sizeof(s.url), "%s", "http://proxy.lan/q/{s}");
    SourcePlan p;
    TEST_ASSERT_FALSE(source_resolve(s, &p));                       // no price path
    snprintf(s.path_price, sizeof(s.path_price), "%s", "quote.last");
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    TEST_ASSERT_EQUAL_STRING("http://proxy.lan/q/SBER", p.url);
    TEST_ASSERT_FALSE(p.https);
    TEST_ASSERT_EQUAL_STRING("SBER", p.label);
    TEST_ASSERT_EQUAL_STRING("", p.path_change);
    s.url[0] = '\0';
    TEST_ASSERT_FALSE(source_resolve(s, &p));                       // no URL
    TEST_ASSERT_FALSE(source_resolve(spec(SRC_PRESET_BINANCE, "", "USDT"), &p));   // preset without a symbol
}

void test_resolve_rejects_overlong_expansion(void) {
    SourceSpec s = spec(SRC_PRESET_CUSTOM, "0123456789012345678901234567890", "");
    memset(s.url, 'a', sizeof(s.url) - 1);
    s.url[sizeof(s.url) - 1] = '\0';
    memcpy(s.url + sizeof(s.url) - 10, "{s}{s}{s}", 10);            // 118 a's + 3 x 31-char symbol = 211 > SRC_URL_MAX
    snprintf(s.path_price, sizeof(s.path_price), "%s", "x");
    SourcePlan p;
    TEST_ASSERT_FALSE(source_resolve(s, &p));
}

// --- presets: extract from the real shapes --------------------------------------

void test_extract_binance(void) {
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_BINANCE, "BTC", "USDT"), FIX_BINANCE, &r));
    TEST_ASSERT_EQUAL_STRING("84 000.06", r.price);
    TEST_ASSERT_EQUAL_STRING("+0.07%", r.change);
    TEST_ASSERT_TRUE(r.has_change);
    TEST_ASSERT_EQUAL_INT(1, r.dir);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 84000.06f, r.price_f);
    TEST_ASSERT_EQUAL_UINT8(0, r.spark_n);
}

void test_extract_binance_futures(void) {
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_BINANCE_USDM, "BTC", "USDT"), FIX_BINANCE_USDM, &r));
    TEST_ASSERT_EQUAL_STRING("83 597.90", r.price);
    TEST_ASSERT_EQUAL_STRING("-0.45%", r.change);
    TEST_ASSERT_EQUAL_INT(-1, r.dir);
    TEST_ASSERT_EQUAL_STRING("", r.funding);          // extract() never fills it: the second request does
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_BINANCE_COINM, "BTC", "USD_PERP"), FIX_BINANCE_COINM, &r));
    TEST_ASSERT_EQUAL_STRING("83 561.60", r.price);
    TEST_ASSERT_EQUAL_STRING("-0.47%", r.change);
    TEST_ASSERT_EQUAL_INT(-1, r.dir);
    // Binance's answers to an unknown contract: "{}" and the error object are price-path errors
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(spec(SRC_PRESET_BINANCE_USDM, "BTC", "USDT_991231"), FIX_BINANCE_EMPTY, &r));
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(spec(SRC_PRESET_BINANCE_USDM, "FOO", "BAR"), FIX_BINANCE_ERR, &r));
    // the COIN-M paths on an object body (a fapi answer fed to the dapi preset) miss as well
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(spec(SRC_PRESET_BINANCE_COINM, "BTC", "USD_PERP"), FIX_BINANCE_USDM, &r));
}

void test_format_funding(void) {
    char out[TICKER_TIME_MAX];
    TEST_ASSERT_TRUE(source_format_funding(0.0001f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +0.0100%", out);
    TEST_ASSERT_TRUE(source_format_funding(-0.00012345f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR -0.0123%", out);      // rounded half away from zero on the 4th digit
    TEST_ASSERT_TRUE(source_format_funding(0.0f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +0.0000%", out);      // the sign is always there
    TEST_ASSERT_TRUE(source_format_funding(0.00004183f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +0.0042%", out);
    TEST_ASSERT_TRUE(source_format_funding(0.0075f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +0.7500%", out);      // Binance's cap
    TEST_ASSERT_TRUE(source_format_funding(5.0f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +9.9999%", out);      // clamped, still 11 characters
    TEST_ASSERT_TRUE(source_format_funding(-5.0f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR -9.9999%", out);
    TEST_ASSERT_TRUE(strlen(out) < TICKER_TIME_MAX);
    TEST_ASSERT_FALSE(source_format_funding(0.0001f, out, 8));   // too small a buffer
}

void test_extract_funding_object_array_and_errors(void) {
    char out[TICKER_TIME_MAX];
    TEST_ASSERT_EQUAL(SRC_OK, source_extract_funding(FIX_BINANCE_USDM_PREMIUM, strlen(FIX_BINANCE_USDM_PREMIUM), "$.lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +0.0042%", out);
    TEST_ASSERT_EQUAL(SRC_OK, source_extract_funding(FIX_BINANCE_COINM_PREMIUM, strlen(FIX_BINANCE_COINM_PREMIUM), "$[0].lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR +0.0083%", out);
    // a JSON number instead of a string works too
    static const char num[] = "{\"lastFundingRate\":-0.0005}";
    TEST_ASSERT_EQUAL(SRC_OK, source_extract_funding(num, strlen(num), "$.lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("FR -0.0500%", out);
    // errors leave "" behind: missing field, not a number, not JSON, empty, array path on an object
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, source_extract_funding(FIX_BINANCE_ERR, strlen(FIX_BINANCE_ERR), "$.lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    static const char bad[] = "{\"lastFundingRate\":\"n/a\"}";
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_NUM, source_extract_funding(bad, strlen(bad), "$.lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL(SRC_ERR_JSON, source_extract_funding("<html>", 6, "$.lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL(SRC_ERR_EMPTY, source_extract_funding("", 0, "$.lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, source_extract_funding(FIX_BINANCE_USDM_PREMIUM, strlen(FIX_BINANCE_USDM_PREMIUM), "$[0].lastFundingRate", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_extract_coingecko_numbers_and_first_key(void) {
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_COINGECKO, "bitcoin", "usd"), FIX_COINGECKO, &r));
    // ArduinoJson (no double) keeps ~6 significant digits of a JSON number:
    // 84000.06 arrives as 84000.0 - the body's digits are not recovered any
    // more; strings (Binance, Kraken) keep every digit
    TEST_ASSERT_EQUAL_STRING("84 000.00", r.price);
    TEST_ASSERT_EQUAL_STRING("-1.23%", r.change);
    TEST_ASSERT_EQUAL_INT(-1, r.dir);
}

void test_extract_json_number_is_the_parsed_float(void) {
    SourceResult r;
    static const char J[] = "{\"tether\":{\"usd\":1.0004},\"bitcoin\":{ \"usd\" :\t123456.78 ,\"usd_24h_change\":0.5}}";
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("$.bitcoin.usd", NULL), J, &r));
    TEST_ASSERT_EQUAL_STRING("123 456.70", r.price);   // 6 significant digits of the JSON number
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 123456.78f, r.price_f);
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("tether.usd", NULL), J, &r));
    TEST_ASSERT_EQUAL_STRING("1.00", r.price);
    // an exponent in the body: the parsed float
    static const char E[] = "{\"v\":1.5e2}";
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("v", NULL), E, &r));
    TEST_ASSERT_EQUAL_STRING("150.00", r.price);
    // integers are exact
    static const char I[] = "{\"v\":84000}";
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("v", NULL), I, &r));
    TEST_ASSERT_EQUAL_STRING("84 000.00", r.price);
}

void test_extract_kraken_change_from_open(void) {
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_KRAKEN, "XBT", "USD"), FIX_KRAKEN, &r));
    TEST_ASSERT_EQUAL_STRING("84 000.10", r.price);
    TEST_ASSERT_EQUAL_STRING("+0.12%", r.change);        // (84000.1 - 83900) / 83900 = 0.1193 %
    TEST_ASSERT_EQUAL_INT(1, r.dir);
}

void test_extract_kraken_error_body_is_a_price_path_error(void) {
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(spec(SRC_PRESET_KRAKEN, "XBT", "XYZ"), FIX_KRAKEN_ERR, &r));
    TEST_ASSERT_EQUAL_STRING("price path", source_error_str(SRC_ERR_PRICE_PATH));
}

void test_extract_custom_with_spark_and_pct(void) {
    SourceSpec s = spec(SRC_PRESET_CUSTOM, "", "");
    snprintf(s.url, sizeof(s.url), "%s", "http://proxy.lan/q");
    snprintf(s.path_price, sizeof(s.path_price), "%s", "quote.last");
    snprintf(s.path_change, sizeof(s.path_change), "%s", "$.quote.pct");
    snprintf(s.path_spark, sizeof(s.path_spark), "%s", "quote.hist");
    s.decimals = 5;
    s.sep = SRC_SEP_NONE;
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(s, FIX_CUSTOM, &r));
    TEST_ASSERT_EQUAL_STRING("0.01235", r.price);         // 0.012345 -> 5 decimals, half up
    TEST_ASSERT_EQUAL_STRING("+2.50%", r.change);
    TEST_ASSERT_EQUAL_UINT8(4, r.spark_n);                 // "x" and null skipped
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0121f, r.spark[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.012345f, r.spark[3]);
}

void test_extract_custom_without_change_path(void) {
    SourceSpec s = spec(SRC_PRESET_CUSTOM, "", "");
    snprintf(s.url, sizeof(s.url), "%s", "http://p/q");
    snprintf(s.path_price, sizeof(s.path_price), "%s", "quote.last");
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(s, FIX_CUSTOM, &r));
    TEST_ASSERT_FALSE(r.has_change);
    TEST_ASSERT_EQUAL_STRING("", r.change);
    TEST_ASSERT_EQUAL_INT(0, r.dir);
    TEST_ASSERT_EQUAL_STRING("0.0123", r.price);           // auto: >= 0.01 -> 4 decimals
}

// --- mini-JSONPath grammar and malformed input --------------------------------------

void test_jpath_index_wildcard_and_optional_dollar(void) {
    SourceResult r;
    static const char J[] = "{\"a\":{\"b\":[{\"c\":\"7\"},{\"c\":\"8\"}]},\"arr\":[[\"1.5\"]]}";
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("$.a.b[1].c", NULL), J, &r));  TEST_ASSERT_EQUAL_STRING("8.00", r.price);
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("a.b[0].c", NULL), J, &r));    TEST_ASSERT_EQUAL_STRING("7.00", r.price);
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("*.*[1].c", NULL), J, &r));    TEST_ASSERT_EQUAL_STRING("8.00", r.price);   // * on an object, * on an array
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("arr[0][0]", NULL), J, &r));   TEST_ASSERT_EQUAL_STRING("1.50", r.price);
    TEST_ASSERT_EQUAL(SRC_OK, run(custom_paths("$.arr.*.*", NULL), J, &r));   TEST_ASSERT_EQUAL_STRING("1.50", r.price);
}

void test_jpath_misses_and_bad_syntax(void) {
    SourceResult r;
    static const char J[] = "{\"a\":{\"b\":[{\"c\":\"7\"}]},\"s\":\"text\",\"o\":{}}";
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("a.x", NULL), J, &r));         // missing key
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("a.b[5].c", NULL), J, &r));    // index out of range
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("a.b[x]", NULL), J, &r));      // bad index
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("a.b[0", NULL), J, &r));       // unclosed
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("a]b", NULL), J, &r));         // stray ]
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("o.*", NULL), J, &r));         // * on an empty object
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("s.x", NULL), J, &r));         // key on a string
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_PATH, run(custom_paths("averyveryveryverylongkeyname", NULL), J, &r));   // key > 23 chars
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_NUM, run(custom_paths("s", NULL), J, &r));            // a string that is not a number
    TEST_ASSERT_EQUAL(SRC_ERR_PRICE_NUM, run(custom_paths("a", NULL), J, &r));            // an object
    TEST_ASSERT_EQUAL(SRC_ERR_CHANGE_PATH, run(custom_paths("a.b[0].c", "a.b[0].d"), J, &r));
    TEST_ASSERT_EQUAL(SRC_ERR_CHANGE_NUM, run(custom_paths("a.b[0].c", "s"), J, &r));
}

void test_extract_malformed_bodies(void) {
    SourceResult r;
    SourcePlan p;
    TEST_ASSERT_TRUE(source_resolve(spec(SRC_PRESET_BINANCE, "BTC", "USDT"), &p));
    #define X(body, expect) TEST_ASSERT_EQUAL(expect, source_extract(body, strlen(body), p, &r))
    TEST_ASSERT_EQUAL(SRC_ERR_EMPTY, source_extract("", 0, p, &r));
    TEST_ASSERT_EQUAL(SRC_ERR_EMPTY, source_extract(NULL, 5, p, &r));
    X("<html>451</html>", SRC_ERR_JSON);                                          // geo-block page instead of JSON
    X("{\"lastPrice\":", SRC_ERR_JSON);                                           // truncated
    X("[1,2,3]", SRC_ERR_PRICE_PATH);
    X("{\"code\":-1121,\"msg\":\"Invalid symbol.\"}", SRC_ERR_PRICE_PATH);      // Binance's error body
    X("{\"lastPrice\":\"n/a\"}", SRC_ERR_PRICE_NUM);
    X("{\"lastPrice\":true}", SRC_ERR_PRICE_NUM);
    X("{\"lastPrice\":\"1\",\"priceChangePercent\":null}", SRC_ERR_CHANGE_PATH);   // null = absent
    X("{\"lastPrice\":\"1\",\"priceChangePercent\":\"--\"}", SRC_ERR_CHANGE_NUM);
    #undef X
    // a body over the 4 KB limit is refused before parsing
    static char big[PAYLOAD_MAX_LEN + 8];
    memset(big, ' ', sizeof(big));
    TEST_ASSERT_EQUAL(SRC_ERR_JSON, source_extract(big, sizeof(big), p, &r));
}

void test_extract_kraken_open_zero_is_an_error(void) {
    SourceResult r;
    static const char J[] = "{\"result\":{\"X\":{\"c\":[\"5\"],\"o\":\"0\"}}}";
    TEST_ASSERT_EQUAL(SRC_ERR_CHANGE_NUM, run(spec(SRC_PRESET_KRAKEN, "X", ""), J, &r));
}

void test_result_change_feeds_t1_direction(void) {
    // the extractor's dir matches what a proxy-sent payload would derive (ticker_dir_from_change)
    SourceResult r;
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_COINGECKO, "bitcoin", "usd"), FIX_COINGECKO, &r));
    TEST_ASSERT_EQUAL_INT(ticker_dir_from_change(r.change), r.dir);
    TEST_ASSERT_EQUAL(SRC_OK, run(spec(SRC_PRESET_BINANCE, "BTC", "USDT"), FIX_BINANCE, &r));
    TEST_ASSERT_EQUAL_INT(ticker_dir_from_change(r.change), r.dir);
}

// --- short name for the badge (docs/TICKERS.md "What the screen shows") -------

static const char* shrt(const SourceSpec& s) {
    static char out[TICKER_SHORT_MAX];
    memset(out, 'X', sizeof(out));
    source_short_default(s, out, sizeof(out));
    return out;
}

void test_short_default_exchange_presets(void) {
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_BINANCE, "btc", "USDT")));
    TEST_ASSERT_EQUAL_STRING("XBT", shrt(spec(SRC_PRESET_KRAKEN, "XBT", "USD")));      // Kraken's own name stays
    TEST_ASSERT_EQUAL_STRING("ETH", shrt(spec(SRC_PRESET_BINANCE_USDM, "eth", "USDT_261225")));
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_BINANCE_COINM, "BTC", "USD_PERP")));
    TEST_ASSERT_EQUAL_STRING("DOGE", shrt(spec(SRC_PRESET_CUSTOM, "doge", "")));
    // a pair typed as one word with an empty market: the base only
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_BINANCE, "BTCUSDT", "")));
    TEST_ASSERT_EQUAL_STRING("BNB", shrt(spec(SRC_PRESET_BINANCE, "bnbusdt", "")));     // USDT before USD
    TEST_ASSERT_EQUAL_STRING("XBT", shrt(spec(SRC_PRESET_KRAKEN, "XBTUSD", "")));
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_BINANCE_COINM, "BTCUSD_PERP", "")));
    TEST_ASSERT_EQUAL_STRING("ETH", shrt(spec(SRC_PRESET_BINANCE, "ETHBTC", "")));
    // a symbol that is itself a quote currency, or too short to be a pair, is kept
    TEST_ASSERT_EQUAL_STRING("USDT", shrt(spec(SRC_PRESET_BINANCE, "USDT", "")));
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_BINANCE, "BTC", "")));
    TEST_ASSERT_EQUAL_STRING("BUSD", shrt(spec(SRC_PRESET_BINANCE, "BUSD", "")));
    // with a market the symbol is taken as the base, whatever it ends with
    TEST_ASSERT_EQUAL_STRING("BTCUSDT", shrt(spec(SRC_PRESET_BINANCE, "BTCUSDT", "USDT")));
    // cut to 7
    TEST_ASSERT_EQUAL_STRING("LONGSYM", shrt(spec(SRC_PRESET_BINANCE, "longsymbol", "USDT")));
    TEST_ASSERT_EQUAL_STRING("", shrt(spec(SRC_PRESET_CUSTOM, "", "")));
}

void test_short_default_coingecko_table_and_fallback(void) {
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_COINGECKO, "bitcoin", "usd")));
    TEST_ASSERT_EQUAL_STRING("BTC", shrt(spec(SRC_PRESET_COINGECKO, "Bitcoin", "usd")));   // ids are folded like the URL
    TEST_ASSERT_EQUAL_STRING("ETH", shrt(spec(SRC_PRESET_COINGECKO, "ethereum", "eur")));
    TEST_ASSERT_EQUAL_STRING("AVAX", shrt(spec(SRC_PRESET_COINGECKO, "avalanche-2", "usd")));
    TEST_ASSERT_EQUAL_STRING("TON", shrt(spec(SRC_PRESET_COINGECKO, "the-open-network", "usd")));
    TEST_ASSERT_EQUAL_STRING("UNI", shrt(spec(SRC_PRESET_COINGECKO, "uniswap", "usd")));   // last entry
    // unknown id: upper-cased, cut to 7
    TEST_ASSERT_EQUAL_STRING("WRAPPED", shrt(spec(SRC_PRESET_COINGECKO, "wrapped-bitcoin", "usd")));
    TEST_ASSERT_EQUAL_STRING("PEPE", shrt(spec(SRC_PRESET_COINGECKO, "pepe", "usd")));
    // "bitcoin-cash" is not "bitcoin": whole ids only
    TEST_ASSERT_EQUAL_STRING("BCH", shrt(spec(SRC_PRESET_COINGECKO, "bitcoin-cash", "usd")));
}

void test_resolve_carries_short_label(void) {
    SourcePlan p;
    SourceSpec s = spec(SRC_PRESET_BINANCE, "BTC", "USDT");
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    TEST_ASSERT_EQUAL_STRING("BTC", p.short_label);                 // derived
    snprintf(s.short_label, sizeof(s.short_label), "%s", "Bitcoin");
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    TEST_ASSERT_EQUAL_STRING("Bitcoin", p.short_label);             // the user's text wins, case kept
    s = spec(SRC_PRESET_CUSTOM, "", "");
    snprintf(s.url, sizeof(s.url), "%s", "http://p/q");
    snprintf(s.path_price, sizeof(s.path_price), "%s", "$.p");
    TEST_ASSERT_TRUE(source_resolve(s, &p));
    TEST_ASSERT_EQUAL_STRING("", p.short_label);                    // no symbol: no badge
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_short_default_exchange_presets);
    RUN_TEST(test_short_default_coingecko_table_and_fallback);
    RUN_TEST(test_resolve_carries_short_label);
    RUN_TEST(test_enum_strings_round_trip);
    RUN_TEST(test_pull_kind_and_schema7_migration);
    RUN_TEST(test_parse_num_accepts_plain_decimals);
    RUN_TEST(test_parse_num_rejects_garbage);
    RUN_TEST(test_format_auto_decimals_by_magnitude);
    RUN_TEST(test_format_fixed_decimals_and_separators);
    RUN_TEST(test_format_rejects_non_numbers_and_small_buffers);
    RUN_TEST(test_format_pct_sign_and_direction);
    RUN_TEST(test_resolve_binance_upper_cases_symbol);
    RUN_TEST(test_resolve_binance_futures_urls_labels_and_funding);
    RUN_TEST(test_resolve_coingecko_lower_cases_and_paths_carry_market);
    RUN_TEST(test_resolve_kraken_open_mode_and_custom_label);
    RUN_TEST(test_resolve_custom_expands_placeholders_and_validates);
    RUN_TEST(test_resolve_rejects_overlong_expansion);
    RUN_TEST(test_extract_binance);
    RUN_TEST(test_extract_binance_futures);
    RUN_TEST(test_format_funding);
    RUN_TEST(test_extract_funding_object_array_and_errors);
    RUN_TEST(test_extract_coingecko_numbers_and_first_key);
    RUN_TEST(test_extract_json_number_is_the_parsed_float);
    RUN_TEST(test_extract_kraken_change_from_open);
    RUN_TEST(test_extract_kraken_error_body_is_a_price_path_error);
    RUN_TEST(test_extract_custom_with_spark_and_pct);
    RUN_TEST(test_extract_custom_without_change_path);
    RUN_TEST(test_jpath_index_wildcard_and_optional_dollar);
    RUN_TEST(test_jpath_misses_and_bad_syntax);
    RUN_TEST(test_extract_malformed_bodies);
    RUN_TEST(test_extract_kraken_open_zero_is_an_error);
    RUN_TEST(test_result_change_feeds_t1_direction);
    return UNITY_END();
}
