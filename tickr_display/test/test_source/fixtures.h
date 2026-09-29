#pragma once
// Real response shapes of the three presets (docs/TICKERS.md "Presets", as probed;
// values edited to round figures). Binance and Kraken deliver
// numbers as strings, CoinGecko as JSON numbers; Binance carries 64-bit
// integers (openTime / closeTime) that overflow `long` in this build.

// GET api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT (552 B measured)
static const char FIX_BINANCE[] =
    "{\"symbol\":\"BTCUSDT\",\"priceChange\":\"58.06000000\",\"priceChangePercent\":\"0.070\","
    "\"weightedAvgPrice\":\"83950.12345678\",\"prevClosePrice\":\"83942.00000000\",\"lastPrice\":\"84000.06000000\","
    "\"lastQty\":\"0.00500000\",\"bidPrice\":\"84000.05000000\",\"bidQty\":\"1.23400000\",\"askPrice\":\"84000.06000000\","
    "\"askQty\":\"0.98700000\",\"openPrice\":\"83942.00000000\",\"highPrice\":\"84500.00000000\",\"lowPrice\":\"83100.00000000\","
    "\"volume\":\"12345.67890000\",\"quoteVolume\":\"1036543210.12345678\",\"openTime\":1758800000000,\"closeTime\":1758886399999,"
    "\"firstId\":4000000000,\"lastId\":4001234567,\"count\":1234568}";

// GET api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true (62 B measured)
static const char FIX_COINGECKO[] = "{\"bitcoin\":{\"usd\":84000.06,\"usd_24h_change\":-1.2345678901234}}";

// GET api.kraken.com/0/public/Ticker?pair=XBTUSD (308 B measured) - the result key is XXBTZUSD, not the pair name
static const char FIX_KRAKEN[] =
    "{\"error\":[],\"result\":{\"XXBTZUSD\":{\"a\":[\"84000.10000\",\"1\",\"1.000\"],\"b\":[\"84000.00000\",\"2\",\"2.000\"],"
    "\"c\":[\"84000.10000\",\"0.01000000\"],\"v\":[\"1234.12345678\",\"5678.12345678\"],\"p\":[\"83950.12345\",\"83900.54321\"],"
    "\"t\":[12345,45678],\"l\":[\"83100.00000\",\"83000.00000\"],\"h\":[\"84500.00000\",\"84600.00000\"],\"o\":\"83900.00000\"}}}";

// GET fapi.binance.com/fapi/v1/ticker/24hr?symbol=BTCUSDT (374 B measured) - USDS-M futures, an object like spot
static const char FIX_BINANCE_USDM[] =
    "{\"closeTime\":1790708197169,\"count\":3206269,\"firstId\":8127761756,\"highPrice\":\"84554.90\",\"lastId\":8130979236,"
    "\"lastPrice\":\"83597.90\",\"lastQty\":\"0.002\",\"lowPrice\":\"82724.30\",\"openPrice\":\"83977.90\",\"openTime\":1790621760000,"
    "\"priceChange\":\"-380.00\",\"priceChangePercent\":\"-0.453\",\"quoteVolume\":\"10455015751.44\",\"symbol\":\"BTCUSDT\","
    "\"volume\":\"125113.994\",\"weightedAvgPrice\":\"83563.92\"}";

// GET fapi.binance.com/fapi/v1/premiumIndex?symbol=BTCUSDT (231 B measured) - the funding rate is a fraction
static const char FIX_BINANCE_USDM_PREMIUM[] =
    "{\"symbol\":\"BTCUSDT\",\"markPrice\":\"83597.90000000\",\"indexPrice\":\"83628.30000000\",\"estimatedSettlePrice\":\"83580.60469244\","
    "\"lastFundingRate\":\"0.00004183\",\"interestRate\":\"0.00010000\",\"nextFundingTime\":1790726400000,\"time\":1790708199000}";

// GET dapi.binance.com/dapi/v1/ticker/24hr?symbol=BTCUSD_PERP (380 B measured) - COIN-M futures, a one-element array
static const char FIX_BINANCE_COINM[] =
    "[{\"baseVolume\":\"8384.1\",\"closeTime\":1790708193711,\"count\":166017,\"firstId\":1156202966,\"highPrice\":\"84500.0\","
    "\"lastId\":1156368982,\"lastPrice\":\"83561.6\",\"lastQty\":\"4\",\"lowPrice\":\"82680.0\",\"openPrice\":\"83957.3\","
    "\"openTime\":1790621760000,\"pair\":\"BTCUSD\",\"priceChange\":\"-395.7\",\"priceChangePercent\":\"-0.471\","
    "\"symbol\":\"BTCUSD_PERP\",\"volume\":\"7006537\",\"weightedAvgPrice\":\"83568.95105357\"}]";

// GET dapi.binance.com/dapi/v1/premiumIndex?symbol=BTCUSD_PERP (253 B measured)
static const char FIX_BINANCE_COINM_PREMIUM[] =
    "[{\"symbol\":\"BTCUSD_PERP\",\"pair\":\"BTCUSD\",\"markPrice\":\"83561.69631148\",\"indexPrice\":\"83598.11214507\","
    "\"estimatedSettlePrice\":\"83554.83112754\",\"lastFundingRate\":\"0.00008311\",\"interestRate\":\"0.00010000\","
    "\"nextFundingTime\":1790726400000,\"time\":1790708202001}]";

// Binance's answers to a symbol it does not know: an empty object (fapi 24hr) or an error object (HTTP 400)
static const char FIX_BINANCE_EMPTY[] = "{}";
static const char FIX_BINANCE_ERR[] = "{\"code\":-1121,\"msg\":\"Invalid symbol.\"}";

// Kraken's answer to an unknown pair
static const char FIX_KRAKEN_ERR[] = "{\"error\":[\"EQuery:Unknown asset pair\"]}";

// A self-hosted proxy: price + percentage as numbers, a history array
static const char FIX_CUSTOM[] =
    "{\"quote\":{\"last\":0.012345,\"pct\":2.5,\"hist\":[0.0121,0.0122,\"x\",null,0.0123,0.012345]}}";
