#pragma once
// On-device sparkline history (docs/TICKERS.md "Sparkline history"): the last 48
// prices of the ticker source as a 96 B ring of uint16_t, scaled to the
// min-max of the window (`lo`..`hi`); the range widens - and the stored
// points are re-quantised - when a price falls outside it. One point per
// successful fetch: 4 h at 5 min, 2 days at 1 h. Lives in RTC slow memory
// (RTC_DATA_ATTR in connectivity_manager.cpp) so it survives deep sleep on
// battery; a cold boot or a changed source (`key`) starts it empty.
// Pure logic, unit-tested on the host (test/test_spark_hist).
#include <stdint.h>
#include <stddef.h>
#include "ticker.h"

#define SPARK_HIST_N TICKER_SPARK_MAX   // 48 points

struct SparkHist {
    uint32_t key;                 // hash of the source (URL); a different key clears the ring
    float    lo, hi;              // value range of the stored points
    uint16_t v[SPARK_HIST_N];     // quantised: lo + v / 65535 * (hi - lo)
    uint8_t  n;                   // points stored (<= SPARK_HIST_N)
    uint8_t  head;                // next write slot
};

// Empties the ring for `key`.
void spark_hist_reset(SparkHist* h, uint32_t key);
// Appends one price. A different key (or an inconsistent ring - random RTC
// memory after a cold boot) resets first. Non-finite prices are ignored.
void spark_hist_push(SparkHist* h, uint32_t key, float price);
// Copies the stored prices, oldest first; returns how many (<= cap).
uint8_t spark_hist_values(const SparkHist* h, float* out, size_t cap);
// FNV-1a of a string (the source URL) - the ring's key.
uint32_t spark_hist_key(const char* s);
