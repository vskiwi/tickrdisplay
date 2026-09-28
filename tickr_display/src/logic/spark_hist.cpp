#include "spark_hist.h"
#include <math.h>
#include <string.h>

void spark_hist_reset(SparkHist* h, uint32_t key) {
    // cppcheck-suppress memsetClassFloat ; all-zero bytes are 0.0f
    memset(h, 0, sizeof(*h));
    h->key = key;
}

static uint16_t quant(float p, float lo, float hi) {
    if (hi <= lo) return 0;
    float f = (p - lo) / (hi - lo) * 65535.0f + 0.5f;
    if (f < 0) f = 0;
    if (f > 65535.0f) f = 65535.0f;
    return (uint16_t)f;
}

static float dequant(uint16_t q, float lo, float hi) {
    return hi <= lo ? lo : lo + (float)q * (hi - lo) / 65535.0f;
}

void spark_hist_push(SparkHist* h, uint32_t key, float price) {
    if (!h || !isfinite(price)) return;
    if (h->key != key || h->n > SPARK_HIST_N || h->head >= SPARK_HIST_N || !isfinite(h->lo) || !isfinite(h->hi)) {
        spark_hist_reset(h, key);
    }
    if (h->n == 0) {
        h->lo = h->hi = price;
    } else if (price < h->lo || price > h->hi) {
        // widen the range and re-quantise what is stored
        float lo = price < h->lo ? price : h->lo, hi = price > h->hi ? price : h->hi;
        for (uint8_t i = 0; i < h->n; i++) h->v[i] = quant(dequant(h->v[i], h->lo, h->hi), lo, hi);
        h->lo = lo;
        h->hi = hi;
    }
    h->v[h->head] = quant(price, h->lo, h->hi);
    h->head = (uint8_t)((h->head + 1) % SPARK_HIST_N);
    if (h->n < SPARK_HIST_N) h->n++;
}

uint8_t spark_hist_values(const SparkHist* h, float* out, size_t cap) {
    if (!h || !out || h->n > SPARK_HIST_N || h->head >= SPARK_HIST_N) return 0;
    uint8_t n = h->n;
    // oldest = head when the ring is full, else slot 0
    uint8_t start = n == SPARK_HIST_N ? h->head : 0;
    uint8_t k = 0;
    for (uint8_t i = 0; i < n && k < cap; i++) {
        out[k++] = dequant(h->v[(start + i) % SPARK_HIST_N], h->lo, h->hi);
    }
    return k;
}

uint32_t spark_hist_key(const char* s) {
    uint32_t hsh = 2166136261u;
    for (; s && *s; s++) { hsh ^= (uint8_t)*s; hsh *= 16777619u; }
    return hsh;
}
