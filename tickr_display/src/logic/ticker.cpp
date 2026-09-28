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
