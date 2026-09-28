// Refresh policy v2 - see refresh_policy.h.
#include "refresh_policy.h"

RefreshKind refresh_decide(const RefreshInputs& in) {
    // Unknown panel content (first frame, CRC mismatch, crash / power-on
    // reset): a differential against a wrong image would invert wrong
    // pixels, so the only safe refresh is a full one.
    if (!in.prev_valid) return REFRESH_FULL;
    // The 24 h hygiene refresh exists to exercise the panel: never skipped.
    if (in.event == RP_EV_HYGIENE) return REFRESH_FULL;
    // The panel already shows exactly this frame (the ticker fetched the
    // same price, the badge policy redraws an unchanged frame).
    if (in.frame_identical) return REFRESH_NONE;
    // Long-lived cards and frames drawn onto unknown RAM.
    if (in.event == RP_EV_CONDITION || in.event == RP_EV_BOOT) return REFRESH_FULL;
    // Nearly every pixel changes: a differential would leave a ghost field.
    if (in.layout_changed) return REFRESH_FULL;
    // Ghosting hygiene: a forced full after N partials (and after an hour on
    // USB, where millis() runs through; the battery flow has one frame per
    // wake and no clock across sleeps, so only its wake counter counts).
    bool forced = in.battery ? in.partials_since_full >= RP_FORCE_FULL_BATT
                             : (in.partials_since_full >= RP_FORCE_FULL_EVERY ||
                                in.ms_since_full >= RP_FULL_MAX_AGE_MS);
    if (forced) return REFRESH_FULL;
    // Spacing of content / badge partials; service frames and restores are
    // exempt (the transitional card is up for 5 s, a pairing outcome follows
    // its code within seconds).
    if (!in.battery && (in.event == RP_EV_CONTENT || in.event == RP_EV_BADGE) &&
        in.ms_since_partial < RP_PARTIAL_MIN_MS)
        return REFRESH_DEFER;
    return REFRESH_PARTIAL;
}

uint8_t refresh_count_after(uint8_t partials_since_full, RefreshKind kind) {
    if (kind == REFRESH_FULL) return 0;
    if (kind == REFRESH_PARTIAL && partials_since_full < 255) return (uint8_t)(partials_since_full + 1);
    return partials_since_full;
}

bool refresh_double_full(bool enabled, bool prev_condition_card, RefreshKind kind) {
    return enabled && prev_condition_card && kind == REFRESH_FULL;
}

const char* refresh_kind_str(RefreshKind k) {
    switch (k) {
        case REFRESH_NONE:    return "none";
        case REFRESH_DEFER:   return "defer";
        case REFRESH_PARTIAL: return "partial";
        default:              return "full";
    }
}
