#include "source.h"
#include "payload.h"   // PAYLOAD_MAX_LEN, PAYLOAD_JSON_DOC: the same bounded document as a payload
#include <ArduinoJson.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Presets (docs/TICKERS.md "Presets"). {s} = symbol, {m} = market; `fold` puts
// both in the case the API wants (CoinGecko ids and currencies are lower-case
// slugs, Binance symbols upper-case; Kraken accepts either).
// ---------------------------------------------------------------------------
enum { FOLD_NONE = 0, FOLD_LOWER, FOLD_UPPER };
struct PresetDef {
    const char* name;
    const char* url;
    const char* p_price;
    const char* p_change;
    uint8_t     change_mode;
    uint8_t     fold;
};
static const PresetDef kPresets[SRC_PRESET_CUSTOM] = {
    {"CoinGecko", "https://api.coingecko.com/api/v3/simple/price?ids={s}&vs_currencies={m}&include_24hr_change=true",
     "$.*.{m}", "$.*.{m}_24h_change", SRC_CHG_PCT, FOLD_LOWER},
    {"Kraken",    "https://api.kraken.com/0/public/Ticker?pair={s}{m}",
     "$.result.*.c[0]", "$.result.*.o", SRC_CHG_OPEN, FOLD_NONE},
    {"Binance",   "https://api.binance.com/api/v3/ticker/24hr?symbol={s}{m}",
     "$.lastPrice", "$.priceChangePercent", SRC_CHG_PCT, FOLD_UPPER},
};

static const char* const kKind[]   = {"none", "text", "url", "mqtt", "ticker"};
static const char* const kPreset[] = {"coingecko", "kraken", "binance", "custom"};
static const char* const kSep[]    = {"space", "comma", "none"};
static const char* const kMode[]   = {"pct", "open"};

static uint8_t parse_enum(const char* s, const char* const* tab, uint8_t n, uint8_t def) {
    if (s) for (uint8_t i = 0; i < n; i++) if (strcmp(s, tab[i]) == 0) return i;
    return def;
}
const char* source_kind_str(uint8_t v)         { return kKind[v < 5 ? v : 0]; }
uint8_t     source_kind_parse(const char* s)   { return parse_enum(s, kKind, 5, SRC_KIND_NONE); }
uint8_t source_pull_kind(uint8_t kind, bool has_pull_url) {
    if (kind == SRC_KIND_TICKER) return SRC_KIND_TICKER;
    return has_pull_url ? (uint8_t)SRC_KIND_URL : (uint8_t)SRC_KIND_NONE;
}
const char* source_preset_str(uint8_t v)       { return kPreset[v < SRC_PRESET_COUNT ? v : (uint8_t)SRC_PRESET_CUSTOM]; }
uint8_t     source_preset_parse(const char* s) { return parse_enum(s, kPreset, SRC_PRESET_COUNT, SRC_PRESET_CUSTOM); }
const char* source_sep_str(uint8_t v)          { return kSep[v < 3 ? v : 0]; }
uint8_t     source_sep_parse(const char* s)    { return parse_enum(s, kSep, 3, SRC_SEP_SPACE); }
const char* source_change_mode_str(uint8_t v)  { return kMode[v < 2 ? v : 0]; }
uint8_t     source_change_mode_parse(const char* s) { return parse_enum(s, kMode, 2, SRC_CHG_PCT); }

// bounded copy / append (strlcpy is not on every host libc)
static void cpy(char* dst, size_t n, const char* src) {
    snprintf(dst, n, "%s", src ? src : "");
}
static void cat(char* dst, size_t n, const char* src) {
    size_t l = strlen(dst);
    if (l < n) cpy(dst + l, n - l, src);
}
static void fold(char* s, uint8_t how) {
    if (how == FOLD_NONE) return;
    for (; *s; s++) *s = (char)(how == FOLD_LOWER ? tolower((unsigned char)*s) : toupper((unsigned char)*s));
}

// Copies `tpl` to `out`, replacing {s} / {m}. False when it does not fit.
static bool expand(const char* tpl, const char* s, const char* m, char* out, size_t n) {
    size_t o = 0;
    for (; *tpl; tpl++) {
        const char* ins = nullptr;
        if (tpl[0] == '{' && tpl[2] == '}' && (tpl[1] == 's' || tpl[1] == 'm')) {
            ins = tpl[1] == 's' ? s : m;
            tpl += 2;
        }
        if (ins) {
            size_t l = strlen(ins);
            if (o + l >= n) return false;
            memcpy(out + o, ins, l);
            o += l;
        } else {
            if (o + 1 >= n) return false;
            out[o++] = *tpl;
        }
    }
    out[o] = '\0';
    return true;
}

void source_spec_defaults(SourceSpec* s) {
    memset(s, 0, sizeof(*s));
    s->preset = SRC_PRESET_COINGECKO;
    s->decimals = SRC_DECIMALS_AUTO;
    s->sep = SRC_SEP_SPACE;
}

bool source_resolve(const SourceSpec& s, SourcePlan* out) {
    memset(out, 0, sizeof(*out));
    out->decimals = s.decimals > SRC_DECIMALS_MAX ? SRC_DECIMALS_AUTO : s.decimals;
    out->sep = s.sep;
    char sym[SRC_SYMBOL_MAX], mkt[SRC_MARKET_MAX];
    cpy(sym, sizeof(sym), s.symbol);
    cpy(mkt, sizeof(mkt), s.market);
    const PresetDef* d = s.preset < SRC_PRESET_CUSTOM ? &kPresets[s.preset] : nullptr;
    if (d) {
        if (!sym[0]) return false;
        fold(sym, d->fold);
        fold(mkt, d->fold);
        if (!expand(d->url, sym, mkt, out->url, sizeof(out->url)) ||
            !expand(d->p_price, sym, mkt, out->path_price, sizeof(out->path_price)) ||
            !expand(d->p_change, sym, mkt, out->path_change, sizeof(out->path_change))) return false;
        out->change_mode = d->change_mode;
    } else {
        if (!s.url[0] || !s.path_price[0]) return false;
        if (!expand(s.url, sym, mkt, out->url, sizeof(out->url))) return false;
        cpy(out->path_price, sizeof(out->path_price), s.path_price);
        cpy(out->path_change, sizeof(out->path_change), s.path_change);
        cpy(out->path_spark, sizeof(out->path_spark), s.path_spark);
        out->change_mode = s.change_mode;
    }
    out->https = strncasecmp(out->url, "https://", 8) == 0;
    if (s.label[0]) {
        cpy(out->label, sizeof(out->label), s.label);
    } else if (d) {
        // "<symbol>/<market> - <Preset>", truncated to the label field
        out->label[0] = '\0';
        cat(out->label, sizeof(out->label), sym);
        if (mkt[0]) cat(out->label, sizeof(out->label), "/");
        cat(out->label, sizeof(out->label), mkt);
        cat(out->label, sizeof(out->label), " - ");
        cat(out->label, sizeof(out->label), d->name);
    } else {
        cpy(out->label, sizeof(out->label), sym[0] ? sym : "Ticker");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Numbers - no strtod (it would link 10 KB of libc)
// ---------------------------------------------------------------------------
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool source_parse_num(const char* s, float* out) {
    if (!s || !out) return false;
    while (*s == ' ') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
    float v = 0;
    int digits = 0;
    for (; is_digit(*s); s++, digits++) v = v * 10.0f + (float)(*s - '0');
    if (*s == '.') {
        s++;
        float scale = 0.1f;
        for (; is_digit(*s); s++, digits++) { v += (float)(*s - '0') * scale; scale *= 0.1f; }
    }
    while (*s == ' ') s++;
    if (*s || digits == 0) return false;
    *out = neg ? -v : v;
    return true;
}

bool source_format_price(const char* in, uint8_t decimals, uint8_t sep, char* out, size_t n) {
    if (!in || !out || n == 0) return false;
    while (*in == ' ') in++;
    bool neg = false;
    if (*in == '-') { neg = true; in++; } else if (*in == '+') in++;
    // digits: integer part without leading zeros, then the fraction
    char id[16], fd[16];
    size_t il = 0, fl = 0;
    bool any = false;
    for (; is_digit(*in); in++) {
        any = true;
        if (il == 0 && *in == '0') continue;          // leading zeros
        if (il >= sizeof(id) - 1) return false;
        id[il++] = *in;
    }
    if (*in == '.') {
        in++;
        for (; is_digit(*in); in++) {
            any = true;
            if (fl < sizeof(fd) - 1) fd[fl++] = *in;   // beyond 15 fraction digits: ignored
        }
    }
    while (*in == ' ') in++;
    if (*in || !any) return false;
    id[il] = '\0';
    fd[fl] = '\0';

    uint8_t d = decimals;
    if (d == SRC_DECIMALS_AUTO) {
        if (il >= 7) d = 0;                                             // >= 1 000 000
        else if (il >= 1) d = 2;                                        // >= 1
        else if (fl >= 1 && (fd[0] != '0' || (fl >= 2 && fd[1] != '0'))) d = 4;   // >= 0.01
        else d = 6;
    }
    if (d > SRC_DECIMALS_MAX) d = SRC_DECIMALS_MAX;

    // one digit string "iiiiffff" (fraction padded to d digits), then round half up on digit d
    char all[32];
    size_t al = 0;
    for (size_t i = 0; i < il; i++) all[al++] = id[i];
    for (size_t i = 0; i < d; i++) all[al++] = i < fl ? fd[i] : '0';
    all[al] = '\0';
    if (fl > d && fd[d] >= '5') {
        size_t i = al;
        bool carry = true;
        while (carry && i > 0) {
            i--;
            if (all[i] == '9') all[i] = '0';
            else { all[i]++; carry = false; }
        }
        if (carry) { memmove(all + 1, all, al + 1); all[0] = '1'; al++; }
    }
    // rounding may have grown the integer part ("999.996" -> "1000.00")
    il = al - d;

    size_t o = 0;
    bool zero = true;
    for (size_t i = 0; i < al; i++) if (all[i] != '0') { zero = false; break; }
    if (neg && !zero) { if (o + 1 >= n) return false; out[o++] = '-'; }
    if (il == 0) { if (o + 1 >= n) return false; out[o++] = '0'; }
    for (size_t i = 0; i < il; i++) {
        if (i > 0 && (il - i) % 3 == 0 && sep != SRC_SEP_NONE) {
            if (o + 1 >= n) return false;
            out[o++] = sep == SRC_SEP_COMMA ? ',' : ' ';
        }
        if (o + 1 >= n) return false;
        out[o++] = all[i];
    }
    if (d > 0) {
        if (o + 1 + d >= n) return false;
        out[o++] = '.';
        memcpy(out + o, all + il, d);
        o += d;
    }
    out[o] = '\0';
    return true;
}

TickerDir source_format_pct(float pct, char* out, size_t n) {
    if (!isfinite(pct)) { snprintf(out, n, "%s", "n/a"); return 0; }
    if (pct > 99999.0f) pct = 99999.0f;      // x 100 stays exact in a float
    if (pct < -99999.0f) pct = -99999.0f;
    long c = (long)(pct * 100.0f + (pct >= 0 ? 0.5f : -0.5f));
    TickerDir d = c > 0 ? 1 : c < 0 ? -1 : 0;
    long a = c < 0 ? -c : c;
    snprintf(out, n, "%s%ld.%02ld%%", d > 0 ? "+" : d < 0 ? "-" : "", a / 100, a % 100);
    return d;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
static JsonVariantConst jpath(JsonVariantConst v, const char* path) {
    if (!path) return JsonVariantConst();
    if (*path == '$') path++;
    while (*path && !v.isNull()) {
        if (*path == '.') { path++; continue; }
        if (*path == '[') {
            path++;
            if (!is_digit(*path)) return JsonVariantConst();
            size_t idx = 0;
            for (; is_digit(*path); path++) idx = idx * 10 + (size_t)(*path - '0');
            if (*path != ']') return JsonVariantConst();
            path++;
            v = v[idx];
            continue;
        }
        char key[SRC_PATH_KEY_MAX];
        size_t k = 0;
        for (; *path && *path != '.' && *path != '['; path++) {
            if (*path == ']' || k >= sizeof(key) - 1) return JsonVariantConst();
            key[k++] = *path;
        }
        key[k] = '\0';
        if (k == 1 && key[0] == '*') {
            if (v.is<JsonObjectConst>()) {
                JsonObjectConst o = v.as<JsonObjectConst>();
                v = o.begin() != o.end() ? o.begin()->value() : JsonVariantConst();
            } else {
                v = v[(size_t)0];
            }
        } else {
            v = v[(const char*)key];
        }
    }
    return v;
}

// The decimal text of a JSON value: strings as they are (Binance, Kraken
// deliver "84000.06000000"), integers, floats with 8 fraction digits
// (the formatter rounds afterwards).
static bool variant_text(JsonVariantConst v, char* out, size_t n) {
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s || strlen(s) >= n) return false;
        memcpy(out, s, strlen(s) + 1);
        return true;
    }
    if (v.is<long>())  { snprintf(out, n, "%ld", v.as<long>()); return true; }
    if (v.is<float>()) { snprintf(out, n, "%.8f", (double)v.as<float>()); return true; }
    return false;
}

// A JSON *number* (CoinGecko) is what ArduinoJson parsed into a float:
// ~7 significant digits; the price text is not recovered from the body any
// more - a quote above 100 000 with cents
// may show a rounded last digit.

SourceError source_extract(const char* json, size_t len, const SourcePlan& p, SourceResult* out) {
    if (!out) return SRC_ERR_NOMEM;
    // cppcheck-suppress memsetClassFloat ; all-zero bytes are 0.0f
    memset(out, 0, sizeof(*out));
    if (!json || len == 0) return SRC_ERR_EMPTY;
    if (len > PAYLOAD_MAX_LEN) return SRC_ERR_JSON;

    DynamicJsonDocument doc(PAYLOAD_JSON_DOC);
    if (doc.capacity() == 0) return SRC_ERR_NOMEM;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return SRC_ERR_JSON;
    JsonVariantConst root = doc.as<JsonVariantConst>();

    JsonVariantConst pv = jpath(root, p.path_price);
    if (pv.isNull()) return SRC_ERR_PRICE_PATH;
    char num[SRC_NUM_MAX];
    if (!variant_text(pv, num, sizeof(num)) || !source_parse_num(num, &out->price_f)) return SRC_ERR_PRICE_NUM;
    if (!source_format_price(num, p.decimals, p.sep, out->price, sizeof(out->price))) return SRC_ERR_PRICE_NUM;

    if (p.path_change[0]) {
        JsonVariantConst cv = jpath(root, p.path_change);
        if (cv.isNull()) return SRC_ERR_CHANGE_PATH;
        float c;
        if (!variant_text(cv, num, sizeof(num)) || !source_parse_num(num, &c)) return SRC_ERR_CHANGE_NUM;
        if (p.change_mode == SRC_CHG_OPEN) {
            if (c == 0.0f) return SRC_ERR_CHANGE_NUM;
            c = (out->price_f - c) / c * 100.0f;
        }
        out->dir = source_format_pct(c, out->change, sizeof(out->change));
        out->has_change = true;
    }

    if (p.path_spark[0]) {
        JsonVariantConst sv = jpath(root, p.path_spark);
        if (sv.is<JsonArrayConst>()) {
            for (JsonVariantConst e : sv.as<JsonArrayConst>()) {
                if (out->spark_n >= TICKER_SPARK_MAX) break;
                if (e.is<float>() || e.is<long>()) out->spark[out->spark_n++] = e.as<float>();
            }
        }
    }
    return SRC_OK;
}

const char* source_error_str(SourceError e) {
    switch (e) {
        case SRC_OK:              return "ok";
        case SRC_ERR_EMPTY:       return "empty body";
        case SRC_ERR_JSON:        return "not json";
        case SRC_ERR_PRICE_PATH:  return "price path";
        case SRC_ERR_PRICE_NUM:   return "price not a number";
        case SRC_ERR_CHANGE_PATH: return "change path";
        case SRC_ERR_CHANGE_NUM:  return "change not a number";
        case SRC_ERR_NOMEM:       return "out of memory";
    }
    return "error";
}
