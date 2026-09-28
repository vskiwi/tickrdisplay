#pragma once
// USB-mode pull scheduler (docs/TICKERS.md "Fetch schedule and errors"): when does the
// awake device GET its Pull URL next. Pure logic, unit-tested on the host
// (test/test_pull_scheduler); main.cpp runs it around fetch_pull_data().
//
//   * the first fetch PULL_FIRST_DELAY_MS after the link is up;
//   * then every refresh interval with a +-PULL_JITTER_PCT jitter so a shelf
//     of devices does not hit one proxy in the same second;
//   * after a failure the wait grows 1x, 2x, 4x, 8x the interval (capped) and
//     resets on the next success - the panel keeps the last frame meanwhile
//     (no card, never a red LED: red means "down");
//   * a changed Pull URL or interval (POST /config) fetches at once.
// The battery flow does not use it (one fetch per wake, backoff_minutes()).
#include <stdint.h>

#define PULL_FIRST_DELAY_MS   3000u
#define PULL_JITTER_PCT       10u
#define PULL_BACKOFF_CAP_X    8u

struct PullScheduler {
    bool     enabled = false;     // a Pull URL is configured
    uint32_t interval_ms = 0;
    uint32_t next_ms = 0;         // when the next fetch is due (millis)
    uint8_t  fails = 0;           // consecutive failures
    uint32_t rnd = 1;             // jitter generator state
};

// Arms the scheduler: first fetch at now + PULL_FIRST_DELAY_MS when enabled.
void pull_sched_start(PullScheduler* s, uint32_t now_ms, uint32_t interval_min, bool enabled, uint32_t seed);
// True when a fetch should run now.
bool pull_sched_due(const PullScheduler* s, uint32_t now_ms);
// Records the outcome of a fetch and schedules the next one.
void pull_sched_done(PullScheduler* s, uint32_t now_ms, bool ok);
// Pull URL / interval changed: fetch at once, failures forgotten.
void pull_sched_config(PullScheduler* s, uint32_t now_ms, uint32_t interval_min, bool enabled);
// A due fetch could not run (no link, a pairing screen owns the panel):
// try again in `ms` without counting a failure.
void pull_sched_defer(PullScheduler* s, uint32_t now_ms, uint32_t ms);
#define PULL_DEFER_NO_LINK_MS  30000u
#define PULL_DEFER_SERVICE_MS  10000u

// Pure pieces:
// interval x min(2^(fails-1), PULL_BACKOFF_CAP_X); fails = 0 or 1 -> interval.
uint32_t pull_backoff_ms(uint32_t interval_ms, uint8_t fails);
// delay +- PULL_JITTER_PCT %, the offset taken from `rnd` (any 32-bit value).
uint32_t pull_jitter_ms(uint32_t delay_ms, uint32_t rnd);
