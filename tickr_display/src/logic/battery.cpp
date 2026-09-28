#include "battery.h"

namespace {
struct OcvPoint { uint16_t mv; uint8_t pct; };

// Typical 1S Li-Ion/LiPo resting voltage curve (descending).
const OcvPoint kOcvTable[] = {
    {4200, 100}, {4100, 90}, {4000, 78}, {3900, 62}, {3800, 45},
    {3700, 25},  {3600, 12}, {3500, 5},  {3400, 2},  {3300, 0},
};
const int kOcvCount = sizeof(kOcvTable) / sizeof(kOcvTable[0]);
}

uint8_t battery_mv_to_percent(uint32_t mv) {
    if (mv >= kOcvTable[0].mv) return 100;
    if (mv <= kOcvTable[kOcvCount - 1].mv) return 0;
    for (int i = 0; i < kOcvCount - 1; i++) {
        const OcvPoint& hi = kOcvTable[i];
        const OcvPoint& lo = kOcvTable[i + 1];
        if (mv <= hi.mv && mv > lo.mv) {
            uint32_t span = hi.mv - lo.mv;
            uint32_t pct = lo.pct + ((mv - lo.mv) * (hi.pct - lo.pct) + span / 2) / span;
            return (uint8_t)pct;
        }
    }
    return 0;
}

uint8_t battery_percent_quantize(uint8_t pct, uint8_t step) {
    if (step == 0) return pct;
    if (pct > 100) pct = 100;
    uint32_t q = ((uint32_t)pct + step / 2) / step * step;
    return q > 100 ? 100 : (uint8_t)q;
}

bool battery_level_changed(uint8_t prev_pct, uint8_t cur_pct, uint8_t step) {
    int diff = (int)cur_pct - (int)prev_pct;
    if (diff < 0) diff = -diff;
    return diff >= (int)step;
}

bool battery_is_low(uint32_t mv, bool was_low) {
    if (was_low) return mv < BATTERY_RECOVER_MV;
    return mv < BATTERY_LOW_MV;
}

uint32_t cell_mv_from_pin(uint32_t pin_mv, uint16_t num, uint16_t den) {
    if (!cell_divider_valid(num, den)) {
        num = CELL_DIVIDER_NUM_DEFAULT;
        den = CELL_DIVIDER_DEN_DEFAULT;
    }
    return pin_mv * num / den;
}

bool cell_divider_valid(uint16_t num, uint16_t den) {
    if (den == 0 || den > 1000) return false;
    return num >= den && num <= 4u * den;   // ratio 1.0 .. 4.0
}

static bool channel_informative(uint16_t raw) {
    return raw < POWER_ADC_SAT_RAW && raw > POWER_ADC_FLOOR_RAW;
}

PowerSense power_sense_rail(uint16_t rail_raw, uint32_t rail_mv, bool was_usb) {
    if (!channel_informative(rail_raw)) return POWER_SENSE_INVALID;
    bool usb = was_usb ? rail_mv >= USB_ABSENT_MV : rail_mv > USB_PRESENT_MV;
    return usb ? POWER_SENSE_USB : POWER_SENSE_BATTERY;
}

PowerSense power_sense_diff(uint16_t rail_raw, uint16_t cell_raw, int32_t diff_mv, bool was_usb) {
    if (!channel_informative(rail_raw) || !channel_informative(cell_raw)) return POWER_SENSE_INVALID;
    if (diff_mv > POWER_DIFF_USB_MV) return POWER_SENSE_USB;
    if (diff_mv < POWER_DIFF_BATT_MV) return POWER_SENSE_BATTERY;
    return was_usb ? POWER_SENSE_USB : POWER_SENSE_BATTERY;
}

PowerSense power_sense_board(BoardProfile b, uint16_t rail_raw, uint16_t cell_raw,
                             uint32_t rail_mv, int32_t diff_mv, bool was_usb) {
    switch (b) {
    case BOARD_A: return power_sense_rail(rail_raw, rail_mv, was_usb);
    case BOARD_B: return power_sense_diff(rail_raw, cell_raw, diff_mv, was_usb);
    default:      return POWER_SENSE_INVALID;
    }
}

int32_t power_boot_metric(BoardProfile b, uint32_t rail_mv, int32_t diff_mv) {
    return b == BOARD_A ? (int32_t)rail_mv : b == BOARD_B ? diff_mv : 0;
}

const char* power_rule_str(BoardProfile b) {
    return b == BOARD_A ? "rail" : b == BOARD_B ? "diff" : "none";
}

BoardProfile board_profile_from_vin_raw(uint16_t vin_raw) {
    if (vin_raw >= POWER_ADC_SAT_RAW) return BOARD_B;
    if (vin_raw <= POWER_ADC_FLOOR_RAW) return BOARD_UNKNOWN;
    return BOARD_A;
}

const char* board_profile_str(BoardProfile b) {
    return b == BOARD_A ? "A" : b == BOARD_B ? "B" : "?";
}

bool board_battery_measurable(BoardProfile b) {
    return b == BOARD_A || b == BOARD_B;
}

PowerSense power_debounce(PowerDebounce* d, PowerSense sample, uint8_t polls) {
    if (sample == POWER_SENSE_INVALID || sample == d->stable) {
        d->cand = d->stable;
        d->count = 0;
        return d->stable;
    }
    if (d->stable == POWER_SENSE_INVALID || polls <= 1) {   // first information: adopt at once
        d->stable = d->cand = sample;
        d->count = 0;
        return d->stable;
    }
    if (sample != d->cand) {
        d->cand = sample;
        d->count = 1;
    } else if (++d->count >= polls) {
        d->stable = sample;
        d->count = 0;
    }
    return d->stable;
}

bool power_decide_usb(PowerMode mode, PowerSense sense) {
    if (mode == POWER_MODE_USB) return true;
    if (mode == POWER_MODE_BATTERY) return false;
    return sense != POWER_SENSE_BATTERY;   // USB, or INVALID -> stay awake
}

uint32_t backoff_minutes(uint32_t base_min, uint32_t fail_count, uint32_t cap_min) {
    if (base_min == 0) base_min = 1;
    if (fail_count == 0) return base_min;
    if (base_min >= cap_min) return base_min;
    uint32_t shift = fail_count - 1;
    if (shift > 16) shift = 16;
    uint32_t v = base_min << shift;
    if (v < base_min) return cap_min; // overflow guard
    return v > cap_min ? cap_min : v;
}

int8_t wifi_rssi_to_bars(int rssi, bool connected) {
    if (!connected || rssi == 0) return -1;
    if (rssi > -55) return 3;
    if (rssi > -75) return 2;
    if (rssi > -90) return 1;
    return 0;
}
