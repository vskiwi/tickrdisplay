#include "recovery_counter.h"

bool recovery_reset_is_cold(RecoveryResetKind kind) {
    return kind == RECOVERY_RST_POWERON || kind == RECOVERY_RST_EXT || kind == RECOVERY_RST_BROWNOUT;
}

bool recovery_counter_boot(RecoveryCounter* c, const RecoveryStore* store, bool cold) {
    c->store = *store;
    c->position = 0;
    c->recovery = false;
    uint8_t n = c->store.load(c->store.ctx);
    c->armed = false;
    if (!cold) {
        // Not the switch. A deep-sleep wake-up comes >= 1 min after the
        // previous start and a software/crash reset is never part of a
        // series, so a leftover count is cleared at once - on battery the
        // device sleeps before the 20 s window could do it.
        if (n != 0) c->store.save(c->store.ctx, 0);
        return false;
    }
    if (n < 255) n++;
    c->position = n;
    if (n >= RECOVERY_THRESHOLD) {
        c->recovery = true;
        c->armed = false;
        c->store.save(c->store.ctx, 0);
        return true;
    }
    c->store.save(c->store.ctx, n);
    c->armed = true;
    return false;
}

bool recovery_counter_tick(RecoveryCounter* c, uint32_t uptime_ms) {
    if (!c->armed || uptime_ms < RECOVERY_WINDOW_MS) return false;
    c->armed = false;
    c->store.save(c->store.ctx, 0);
    return true;
}
