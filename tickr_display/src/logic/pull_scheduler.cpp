#include "pull_scheduler.h"

static uint32_t interval_to_ms(uint32_t interval_min) {
    if (interval_min == 0) interval_min = 60;
    if (interval_min > 1440) interval_min = 1440;
    return interval_min * 60000u;
}

static uint32_t next_rnd(PullScheduler* s) {
    s->rnd = s->rnd * 1664525u + 1013904223u;   // Numerical Recipes LCG
    return s->rnd >> 8;
}

uint32_t pull_backoff_ms(uint32_t interval_ms, uint8_t fails) {
    uint32_t mult = 1;
    for (uint8_t i = 1; i < fails && mult < PULL_BACKOFF_CAP_X; i++) mult <<= 1;
    return interval_ms * mult;
}

uint32_t pull_jitter_ms(uint32_t delay_ms, uint32_t rnd) {
    // offset in -1000..+1000 tenths of a percent, scaled to the jitter width
    int32_t off = (int32_t)(rnd % 2001u) - 1000;
    int64_t d = (int64_t)delay_ms * off * (int64_t)PULL_JITTER_PCT / 100000;
    int64_t out = (int64_t)delay_ms + d;
    return out < 0 ? 0 : (uint32_t)out;
}

void pull_sched_start(PullScheduler* s, uint32_t now_ms, uint32_t interval_min, bool enabled, uint32_t seed) {
    s->enabled = enabled;
    s->interval_ms = interval_to_ms(interval_min);
    s->fails = 0;
    s->rnd = seed ? seed : 1;
    s->next_ms = now_ms + PULL_FIRST_DELAY_MS;
}

bool pull_sched_due(const PullScheduler* s, uint32_t now_ms) {
    return s->enabled && (int32_t)(now_ms - s->next_ms) >= 0;
}

void pull_sched_done(PullScheduler* s, uint32_t now_ms, bool ok) {
    if (ok) s->fails = 0;
    else if (s->fails < 255) s->fails++;
    s->next_ms = now_ms + pull_jitter_ms(pull_backoff_ms(s->interval_ms, s->fails), next_rnd(s));
}

void pull_sched_defer(PullScheduler* s, uint32_t now_ms, uint32_t ms) {
    s->next_ms = now_ms + ms;
}

void pull_sched_config(PullScheduler* s, uint32_t now_ms, uint32_t interval_min, bool enabled) {
    s->enabled = enabled;
    s->interval_ms = interval_to_ms(interval_min);
    s->fails = 0;
    s->next_ms = now_ms;
}
