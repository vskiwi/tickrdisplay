#include "device_state.h"
#include "ui_strings.h"
#include <stdio.h>

const char* screen_card_str(ScreenCard c) {
    static const char* const kNames[CARD_COUNT] = {
        "none", "offline", "power_to_battery", "ota", "battery_empty",
    };
    return c < CARD_COUNT ? kNames[c] : "unknown";
}

const char* power_switch_str(uint8_t s) {
    static const char* const kNames[PSW_COUNT] = { "off", "idle", "grace", "blocked", "restart" };
    return s < PSW_COUNT ? kNames[s] : "unknown";
}

bool device_battery_low(uint8_t power, bool batt_known, uint8_t batt_pct) {
    return power == BADGE_POWER_BATTERY && batt_known && batt_pct <= DS_BATTERY_LOW_PCT;
}

bool device_offline_card_due(uint32_t fail_count) {
    if (fail_count < DS_OFFLINE_FAIL_FIRST) return false;
    return (fail_count - DS_OFFLINE_FAIL_FIRST) % DS_OFFLINE_FAIL_EVERY == 0;
}

void device_offline_age_line(uint32_t age_s, char* buf, size_t len) {
    if (!len) return;
    buf[0] = '\0';
    if (age_s == DS_AGE_UNKNOWN) return;
    uint32_t min = age_s / 60u;
    if (min < 60u) snprintf(buf, len, UI_OFFLINE_AGE_MIN_FMT, (unsigned)min);
    else snprintf(buf, len, UI_OFFLINE_AGE_H_FMT, (unsigned)(min / 60u));
}

uint32_t device_stale_ms(uint32_t interval_min) {
    uint32_t ms = interval_min * 3u * 60000u;
    return ms > DS_T_STALE_MIN_MS ? ms : DS_T_STALE_MIN_MS;
}

// --- the machine --------------------------------------------------------------

static void poll_wifi(DeviceState* s, const DeviceInputs& in, bool content_refreshed) {
    if (!in.wifi_connected) {
        if (!s->wifi_down) {                       // outage begins
            s->wifi_down = true;
            s->wifi_down_at = in.now_ms;
            s->offline_done = false;
        }
        s->wifi_up = false;
    } else if (s->wifi_down) {
        if (!s->wifi_up) {                         // link back: start the hold
            s->wifi_up = true;
            s->wifi_up_at = in.now_ms;
        } else if (in.now_ms - s->wifi_up_at >= DS_WIFI_RESTORE_HOLD_MS) {
            s->wifi_down = false;                  // outage over
            s->wifi_up = false;
            s->offline_up = false;
            s->offline_done = false;
        }
    }
    if (s->offline_up && content_refreshed) {      // "the way back is the next content refresh"
        s->offline_up = false;
        s->offline_done = true;
    }
    // The card is drawn only while the link is actually down: with the link
    // back and its hold running, T_long passing would draw a card that the
    // restore removes 30 s later (two wasted refreshes). If the link drops
    // again inside the hold, the card comes with that poll.
    if (s->wifi_down && !s->wifi_up && !s->offline_done && !s->offline_up &&
        in.now_ms - s->wifi_down_at >= DS_T_LONG_MS) {
        s->offline_up = true;                      // once per outage
    }
}

// A change of the debounced reading: the card and the grace start here.
static void power_flip(DeviceState* s, const DeviceInputs& in, DeviceOutputs* out, bool switch_allowed) {
    uint8_t prev = s->power_last;
    uint32_t held = in.now_ms - s->power_last_at;   // how long the previous reading stood
    s->power_last = in.power;
    s->power_last_at = in.now_ms;
    s->grace_on = false;                           // any change ends a pending restart - silently
    // A flip to or from "unknown" is the detector losing information, not a
    // transition: never a card, never counted, never a restart.
    if (prev == BADGE_POWER_UNKNOWN || in.power == BADGE_POWER_UNKNOWN) return;
    if (!s->power_window_open || in.now_ms - s->power_window_at >= DS_POWER_CARD_WINDOW_MS) {
        s->power_window_open = true;
        s->power_window_at = in.now_ms;
        s->power_flips = 0;
        s->power_card_in_window = false;
    }
    if (s->power_flips < 255) s->power_flips++;
    bool flapping = s->power_flips >= DS_POWER_FLAPPING_FLIPS;
    if (flapping) out->power_flapping = true;
    if (in.power != BADGE_POWER_BATTERY) return;   // battery -> USB: content with the bolt, nothing else
    // The mode switch: 2 min when the source was steady, 10 min of
    // uninterrupted battery when it flaps or flipped right after boot.
    if (switch_allowed) {
        s->grace_on = true;
        s->grace_at = in.now_ms;
        s->grace_ms = (flapping || held < DS_POWER_STABLE_MS) ? DS_POWER_GRACE_LONG_MS : DS_POWER_GRACE_MS;
    }
    // A card in the USB -> battery direction only; battery -> USB
    // shows the content with the bolt (the badge refresh does that).
    if (!s->power_cards || s->power_card_in_window) return;
    s->power_card_in_window = true;                // rate limit: one per window, dropped or not
    if (in.service != SERVICE_NONE) return;        // dropped, not queued
    s->power_card_up = true;
    s->power_card_at = in.now_ms;
}

static void poll_power(DeviceState* s, const DeviceInputs& in, DeviceOutputs* out) {
    if (s->power_card_up && in.now_ms - s->power_card_at >= DS_POWER_CARD_HOLD_MS) s->power_card_up = false;
    // Never on board ? (no level, and its detector has no rule anyway).
    bool switch_allowed = in.switch_allowed && in.batt_known;
    if (!s->power_seen) {                          // the boot state is not a transition
        s->power_seen = true;
        s->power_last = in.power;
        s->power_last_at = in.now_ms;
    } else if (in.power != s->power_last) {
        power_flip(s, in, out, switch_allowed);
    }
    // The grace: cancelled when the switch stops being allowed (URL removed,
    // override set), otherwise counted down; at its end the restart waits
    // for OTA / service frames / recovery to finish (deferred, not dropped:
    // the device is still on its cell).
    if (s->grace_on && !switch_allowed) s->grace_on = false;
    if (!s->grace_on) {
        out->power_switch = switch_allowed ? PSW_IDLE : PSW_OFF;
        return;
    }
    uint32_t elapsed = in.now_ms - s->grace_at;
    if (elapsed < s->grace_ms) {
        out->power_switch = PSW_GRACE;
        out->power_grace_s = (s->grace_ms - elapsed + 999u) / 1000u;
    } else if (in.ota_active || in.service != SERVICE_NONE || in.hold) {
        out->power_switch = PSW_BLOCKED;
    } else {
        out->power_switch = PSW_RESTART;
        out->power_restart = true;
    }
}

static void poll_ota(DeviceState* s, const DeviceInputs& in, bool content_refreshed) {
    if (!in.ota_active) {                          // ended (reboot, or failed -> restore)
        s->ota_up = false;
        s->ota_done = false;
        return;
    }
    if (s->ota_up && content_refreshed) {          // content pushed meanwhile: the card does not come back
        s->ota_up = false;
        s->ota_done = true;
    }
    if (!s->ota_done) s->ota_up = true;
}

void device_state_poll(DeviceState* s, const DeviceInputs& in, DeviceOutputs* out) {
    out->card = CARD_NONE;
    out->batt_low = device_battery_low(in.power, in.batt_known, in.batt_pct);
    out->led_offline = false;
    out->power_flapping = false;
    out->wifi_down_s = 0;
    out->power_switch = PSW_OFF;
    out->power_grace_s = 0;
    out->power_restart = false;

    bool content_refreshed = s->content_seen && in.content_seq != s->content_seq;
    s->content_seen = true;
    s->content_seq = in.content_seq;

    poll_wifi(s, in, content_refreshed);
    poll_power(s, in, out);
    poll_ota(s, in, content_refreshed);

    // Priority: OTA > POWER (transition) > OFFLINE > base. Service
    // frames are above all of them; hal_display's restore draws whatever
    // condition is left when they end.
    if (s->ota_up) out->card = CARD_OTA;
    else if (s->power_card_up) out->card = CARD_POWER_BATTERY;
    else if (s->offline_up) out->card = CARD_OFFLINE;

    // The OFFLINE LED rule follows the condition (outage >= T_long), not the
    // card: it stays while a content push replaced the card.
    if (s->wifi_down) {
        uint32_t down_ms = in.now_ms - s->wifi_down_at;
        out->led_offline = down_ms >= DS_T_LONG_MS;
        out->wifi_down_s = down_ms / 1000u;
    }
}
