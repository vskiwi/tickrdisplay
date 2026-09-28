#include "status_policy.h"
#include <string.h>

uint8_t badge_change(const DisplayStatus& shown, const DisplayStatus& cur) {
    uint8_t c = BADGE_SAME;
    if (shown.wifi_connected != cur.wifi_connected) c |= BADGE_WIFI;
    if (shown.power != cur.power || shown.usb != cur.usb || shown.batt_known != cur.batt_known) c |= BADGE_POWER;
    if (shown.batt_pct != cur.batt_pct || strcmp(shown.ip, cur.ip) != 0) c |= BADGE_COSMETIC;
    return c;
}

BadgeAction badge_poll(BadgePolicy* p, uint8_t change, uint32_t since_refresh_ms, bool refreshed) {
    if (refreshed) p->due = false;   // whatever refreshed the panel drew the stored snapshot
    uint8_t state = change & (BADGE_WIFI | BADGE_POWER);
    bool adopt = false;
    if (change & BADGE_STALE) p->due = true;   // the stale marker: owed one refresh (the snapshot itself is unchanged - nothing to adopt)
    if (state) {
        if (state != p->pending) { p->pending = state; p->count = 0; }
        if (p->count < 255) p->count++;
        uint8_t need = (state & BADGE_WIFI) ? BADGE_WIFI_DEBOUNCE_POLLS : BADGE_POWER_DEBOUNCE_POLLS;
        if (p->count >= need) {
            p->pending = 0;
            p->count = 0;
            p->due = true;
            adopt = true;
        }
    } else {
        p->pending = 0;
        p->count = 0;
        if (change & BADGE_COSMETIC) adopt = true;    // cosmetic: folded into the next refresh
    }
    // Not while a state change is still being debounced: the refresh would
    // adopt the caller's raw snapshot and bypass the debounce.
    if (p->due && !p->pending && since_refresh_ms >= BADGE_REFRESH_MIN_MS) {
        p->due = false;
        return BADGE_REFRESH;
    }
    return adopt ? BADGE_ADOPT : BADGE_KEEP;
}
