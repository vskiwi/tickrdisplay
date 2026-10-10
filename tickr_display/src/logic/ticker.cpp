#include "ticker.h"
#include "ui_strings.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

TickerDir ticker_dir_from_change(const char* change) {
    if (!change) return 0;
    while (*change == ' ' || *change == '\t') change++;
    if (*change == '+') return 1;
    if (*change == '-') return -1;
    if ((unsigned char)change[0] == 0xE2 && (unsigned char)change[1] == 0x88 && (unsigned char)change[2] == 0x92) return -1;   // U+2212
    // Unsigned number: positive unless every digit before the first other
    // character is 0 ("0.00%" -> flat). No strtod: it would link 6 KB of libc.
    for (; *change; change++) {
        if (*change >= '1' && *change <= '9') return 1;
        if (*change != '0' && *change != '.' && *change != ',' && *change != ' ') break;
    }
    return 0;
}

uint8_t ticker_spark_scale(const float* in, size_t n, uint8_t* out, uint8_t height) {
    if (!in || !out || height == 0) return 0;
    if (n > TICKER_SPARK_MAX) n = TICKER_SPARK_MAX;
    float lo = 0, hi = 0;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(in[i])) continue;
        if (!any || in[i] < lo) lo = in[i];
        if (!any || in[i] > hi) hi = in[i];
        any = true;
    }
    uint8_t k = 0;
    const uint8_t mid = (uint8_t)((height - 1) / 2);
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(in[i])) continue;
        if (hi <= lo) { out[k++] = mid; continue; }               // flat series: mid line
        float f = (in[i] - lo) / (hi - lo) * (float)(height - 1);
        int v = (int)(f + 0.5f);
        if (v < 0) v = 0;
        if (v > height - 1) v = height - 1;
        out[k++] = (uint8_t)v;
    }
    return k;
}

void ticker_age_line(uint32_t age_s, bool stale, char* buf, size_t len) {
    if (!len) return;
    buf[0] = '\0';
    if (age_s == TICKER_AGE_UNKNOWN) return;
    uint32_t m = age_s / 60u, h = m / 60u, d = h / 24u;
    if (stale) {
        if (d >= 2)       snprintf(buf, len, UI_STALE_D_FMT, (unsigned)d);
        else if (h >= 1)  snprintf(buf, len, UI_STALE_H_FMT, (unsigned)h);
        else              snprintf(buf, len, UI_STALE_MIN_FMT, (unsigned)m);
        return;
    }
    if (d >= 2)           snprintf(buf, len, UI_AGE_D_FMT, (unsigned)d);
    else if (h >= 1)      snprintf(buf, len, UI_AGE_H_FMT, (unsigned)h);
    else if (m >= 1)      snprintf(buf, len, UI_AGE_MIN_FMT, (unsigned)m);
    else                  snprintf(buf, len, "%s", UI_AGE_JUST_NOW);
}

bool ticker_stale_crossing(bool* fired, bool shows_age, uint32_t age_s, uint32_t stale_s) {
    if (!fired) return false;
    if (!shows_age || age_s == TICKER_AGE_UNKNOWN) return false;   // nothing to mark; the flag waits for content that has an age line
    if (age_s < stale_s) return false;
    if (*fired) return false;
    *fired = true;
    return true;
}

bool ticker_price_trim_round(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!in) return false;
    const char* p = in;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    // integer part: digits with one kind of separator between groups of three
    char digits[20];
    size_t nd = 0, group = 0;
    char sep = 0;
    bool grouped = false;
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            if (nd >= sizeof(digits) - 1) return false;
            digits[nd++] = *p;
            group++;
            if (grouped && group > 3) return false;         // "1 2345" - not a thousands grouping
            continue;
        }
        if ((*p == ' ' || *p == ',') && group >= 1 && group <= 3 && (sep == 0 || *p == sep)
            && p[1] >= '0' && p[1] <= '9') {
            if (grouped && group != 3) return false;        // "12 34 567" - groups after the first are three digits
            sep = *p;
            grouped = true;
            group = 0;
            continue;
        }
        break;
    }
    if (nd < 4 || digits[0] == '0') return false;           // under 1 000 (or a leading zero): keep the cents
    if (grouped && group != 3) return false;                // "1 23.45"
    // fraction: at least one digit, then the end of the string
    if (*p != '.') return false;
    p++;
    if (*p < '0' || *p > '9') return false;
    const bool up = *p >= '5';
    for (; *p >= '0' && *p <= '9'; p++) {}
    if (*p) return false;                                   // "%", a currency, blanks: not a bare number
    // half-up: carry from the right; an overflow adds a leading '1'
    if (up) {
        size_t i = nd;
        while (i > 0) {
            i--;
            if (digits[i] == '9') { digits[i] = '0'; continue; }
            digits[i]++;
            break;
        }
        if (i == 0 && digits[0] == '0') {                   // all nines rolled over
            memmove(digits + 1, digits, nd);
            digits[0] = '1';
            nd++;
        }
    }
    // emit: sign, groups of three from the right with the same separator
    size_t o = 0;
    if (neg) { if (o + 1 >= cap) goto tight; out[o++] = '-'; }
    for (size_t i = 0; i < nd; i++) {
        if (sep && i > 0 && (nd - i) % 3 == 0) { if (o + 1 >= cap) goto tight; out[o++] = sep; }
        if (o + 1 >= cap) goto tight;
        out[o++] = digits[i];
    }
    out[o] = '\0';
    return true;
tight:
    out[0] = '\0';
    return false;
}

// --- 2x2 grid ----------------------------------------------------------------

void grid_cell_origin(uint8_t idx, int16_t* x, int16_t* y) {
    *x = (idx & 1) ? GRID_CELL_W + 1 : 0;
    *y = (idx & 2) ? GRID_CELL_H + 1 : 0;
}

uint8_t grid_ticker_cells(uint8_t n) {
    if (n > GRID_MAX) n = GRID_MAX;
    return n == GRID_MAX ? GRID_MAX : (n < GRID_MAX - 1 ? n : GRID_MAX - 1);
}

static uint32_t fnv1a(uint32_t h, const char* s) {
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    h ^= 0xFF;                       // terminator: "AB","C" differs from "A","BC"
    h *= 16777619u;
    return h;
}

uint32_t grid_set_key(const GridFrame* g) {
    uint32_t h = 2166136261u ^ g->n;
    for (uint8_t i = 0; i < g->n && i < GRID_MAX; i++) h = fnv1a(h, g->cells[i].short_label);
    return h;
}

uint32_t grid_battery_interval_min(bool grid, uint32_t interval_min) {
    return grid && interval_min < GRID_BATT_MIN_MIN ? GRID_BATT_MIN_MIN : interval_min;
}

uint8_t grid_fit_price(const char* price, int16_t max_w, GridMeasureFn measure, void* ctx,
                       char* buf, size_t cap, const char** out) {
    *out = price;
    if (measure(price, 0, ctx) <= max_w) return 0;
    if (ticker_price_trim_round(price, buf, cap) && measure(buf, 0, ctx) <= max_w) { *out = buf; return 1; }
    if (measure(price, 1, ctx) <= max_w) return 2;
    return 3;
}

const char* led_rule_str(uint8_t v) {
    return v == LED_RULE_SIGN ? "sign" : "off";
}

uint8_t led_rule_parse(const char* s) {
    return (s && strcmp(s, "sign") == 0) ? LED_RULE_SIGN : LED_RULE_OFF;
}

bool ticker_led_decide(uint8_t rule, bool has_led, uint8_t pr, uint8_t pg, uint8_t pb,
                       bool has_dir, TickerDir dir, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (has_led) { *r = pr; *g = pg; *b = pb; return true; }      // an explicit colour wins
    if (rule != LED_RULE_SIGN || !has_dir || dir == 0) return false;
    *r = dir < 0 ? 255 : 0;
    *g = dir > 0 ? 255 : 0;
    *b = 0;
    return true;
}
