#include "wifi_supervisor.h"

uint32_t wifi_sup_backoff_ms(uint8_t attempts) {
    if (attempts == 0) return 0;
    uint32_t ms = WIFI_SUP_BACKOFF_MIN_MS;
    for (uint8_t i = 1; i < attempts && ms < WIFI_SUP_BACKOFF_MAX_MS; i++) ms <<= 1;
    return ms > WIFI_SUP_BACKOFF_MAX_MS ? WIFI_SUP_BACKOFF_MAX_MS : ms;
}

uint32_t wifi_sup_down_s(const WifiSupervisor* s, uint32_t now_ms) {
    return s->down ? (now_ms - s->down_at) / 1000u : 0;
}

WifiSupervisorAction wifi_sup_poll(WifiSupervisor* s, uint32_t now_ms, WifiLinkState link, bool hold, bool* recovered) {
    if (recovered) *recovered = false;

    if (link == WIFI_LINK_UP) {
        if (s->down && s->attempts > 0 && recovered) *recovered = true;
        s->down = false;
        s->assoc = false;
        s->attempts = 0;
        s->restarted = false;
        return WIFI_SUP_NONE;
    }

    if (!s->down) {                      // outage begins
        s->down = true;
        s->down_at = now_ms;
        s->attempts = 0;
        s->restarted = false;
        s->assoc = false;
    }
    // A fresh association (after the core's or our own attempt) gets the
    // DHCP grace before it is torn down again.
    if (link == WIFI_LINK_ASSOCIATED) {
        if (!s->assoc) { s->assoc = true; s->assoc_at = now_ms; }
    } else {
        s->assoc = false;
    }

    if (hold) return WIFI_SUP_NONE;

    uint32_t down_ms = now_ms - s->down_at;
    if (!s->restarted && down_ms >= WIFI_SUP_RESTART_AFTER_MS) {
        s->restarted = true;
        return WIFI_SUP_RESTART;
    }

    // First attempt: after the grace of the state; later ones: after the back-off.
    bool due;
    if (s->attempts == 0) {
        uint32_t grace = link == WIFI_LINK_GAVE_UP ? WIFI_SUP_GAVE_UP_GRACE_MS : WIFI_SUP_DOWN_GRACE_MS;
        due = down_ms >= grace;
    } else {
        due = now_ms - s->last_attempt_ms >= wifi_sup_backoff_ms(s->attempts);
    }
    if (link == WIFI_LINK_ASSOCIATED && now_ms - s->assoc_at < WIFI_SUP_NO_IP_GRACE_MS) due = false;
    if (!due) return WIFI_SUP_NONE;

    if (s->attempts < 255) s->attempts++;
    s->last_attempt_ms = now_ms;
    return WIFI_SUP_RECONNECT;
}
