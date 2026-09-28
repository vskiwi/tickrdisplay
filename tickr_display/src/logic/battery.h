#pragma once
// Pure power/battery helpers. No Arduino / ESP-IDF dependencies so the
// module can be compiled and unit-tested on the host (see test/test_battery).
#include <stdint.h>

// --- Thresholds shared by main.cpp / hal_power ----------------------------

// Cell voltage (GPIO 32, divider-corrected mV) below this in battery mode =>
// "Low battery" screen + long sleep.
#define BATTERY_LOW_MV            3300u
// The cell must recover above this before normal operation resumes
// (hysteresis so a cell hovering around 3.3 V does not toggle every wake-up).
#define BATTERY_RECOVER_MV        3450u

// Resistive dividers, calibrated pin millivolts -> node millivolts.
//   Rail (GPIO 33): 21/10, fitted on the rev A unit (docs/HARDWARE.md "Power sensing").
//   Cell (GPIO 32): 189/100 by default - the stock firmware's raw-count
//   factor (1.702 V per uncalibrated count, docs/HARDWARE.md "Power sensing")
//   puts a full charging cell at 4.20-4.23 V on both units, which on the
//   calibrated pin millivolts is an effective 1.89. The value is a config
//   field (AppConfig::adc_cell_num/den) so it can be corrected per device.
//   TODO(hw-verify): multimeter on the cell vs pin_mv of GPIO 32.
#define RAIL_DIVIDER_NUM          21u
#define RAIL_DIVIDER_DEN          10u
#define CELL_DIVIDER_NUM_DEFAULT  189u
#define CELL_DIVIDER_DEN_DEFAULT  100u

// USB / battery decision (docs/HARDWARE.md "Power sensing"). Both revisions
// read the same two channels; the decision rule is picked per board profile
// because the two boards' OR-ing elements differ (power_sense_board()).
//
// Measured on the bench (steady GET reads, calibrated pin mV; rail x 2.1 in
// brackets = "firmware units"):
//   rev A cable        rail 2495 (5.24)  cell 2233   diff +262
//   rev A on the pads  rail 2255 (4.73)  cell 2234   diff  +21   (docs/HARDWARE.md row 1)
//   rev A battery      rail 1989 (4.18)  cell 2217   diff -228
//   rev B cable (pads) rail 2334 (4.90)  cell 2132   diff +202
//   rev B battery      rail 2019 (4.24)  cell 2105   diff  -86   (Schottky-sized drop)
//
// Rev A - RAIL thresholds (the detector proven on the rev A bench unit):
// USB above USB_PRESENT_MV, battery below USB_ABSENT_MV, hysteresis in
// between (fw units). Margins: 0.23 V below the pads, 0.17 V above a full
// cell. The differential cannot serve rev A: on the pads the rail sits at
// the level of a full charging cell (+21 mV), and the boot sag through the
// pads (4.4-4.56 V measured) turns that into -140..-63 mV = "battery".
#define USB_PRESENT_MV            4500u
#define USB_ABSENT_MV             4350u
// Rev B - DIFFERENCE rail - cell at the pin (coordinator's refinement): USB
// above POWER_DIFF_USB_MV, battery below POWER_DIFF_BATT_MV, hold in
// between. Margins: 122 mV to the pads point, 56 mV to the battery point.
// The rail thresholds would leave rev B only 0.11 V (4.24 V on a 3.98 V
// cell vs 4.35 V; a full 4.2 V cell would read ~4.49 V - no margin at all).
#define POWER_DIFF_USB_MV         80
#define POWER_DIFF_BATT_MV        (-30)
// Known risk (both rules): a weak adapter (4.75 V) behind the pads lands
// the rail at the cell level - the `power_source` override exists for it.
// Raw 12-bit ADC counts at/above this = channel saturated (pin >= ~3.1 V at
// 11 dB): the reading carries no information (rev B with the sense network
// disabled reads 4095 on both channels). At/below the floor the pin is at
// ~0 V: unconnected or shorted - equally no information.
#define POWER_ADC_SAT_RAW         4080u
#define POWER_ADC_FLOOR_RAW       8u
// Boot: settle after enabling the sense network (the stock's delay(150)),
// then a peak-hold window - the reading with the highest board metric
// (power_boot_metric(): rail mV on A, rail-cell difference on B) out of
// POWER_BOOT_PEAK_PASSES readings POWER_BOOT_PEAK_GAP_MS apart decides. The
// rail sags under boot current (through the pads the first reading after an
// OTA reboot was 0.3 V low); a cell can never push the metric up.
#define POWER_SENSE_SETTLE_MS     150u
#define POWER_BOOT_PEAK_PASSES    12u
#define POWER_BOOT_PEAK_GAP_MS    20u
// Consecutive 1 s polls a changed reading must survive before the runtime
// state follows it (the boot reading is adopted at once).
#define POWER_SENSE_DEBOUNCE      3u
// Battery percentage granularity used for the battery badge (hysteresis step).
#define BATTERY_DISPLAY_STEP_PCT  5u

// Li-Ion open-circuit voltage (mV) -> 0..100 %. Piecewise-linear over a
// typical 1S LiPo discharge curve; clamps outside [3300, 4200].
uint8_t battery_mv_to_percent(uint32_t mv);

// Snap a percentage to the display granularity (e.g. 5 %): 0,5,...,100.
uint8_t battery_percent_quantize(uint8_t pct, uint8_t step);

// True when |cur - prev| >= step, i.e. the displayed level should change.
bool battery_level_changed(uint8_t prev_pct, uint8_t cur_pct, uint8_t step);

// Low-battery latch with hysteresis. `was_low` is the previous state.
bool battery_is_low(uint32_t mv, bool was_low);

// Cell divider: pin millivolts -> cell millivolts, and the range a config
// value is accepted in (den 1..1000, ratio 1.0 .. 4.0; anything else falls
// back to the default).
uint32_t cell_mv_from_pin(uint32_t pin_mv, uint16_t num, uint16_t den);
bool     cell_divider_valid(uint16_t num, uint16_t den);

// Power-source detector result.
enum PowerSense : uint8_t {
    POWER_SENSE_BATTERY = 0,
    POWER_SENSE_USB     = 1,
    POWER_SENSE_INVALID = 2,   // saturated or floored channel - no information
};

// Rail detector (rev A): `rail_raw` decides validity (saturated / floored
// = INVALID), `rail_mv` = rail x 21/10 against USB_PRESENT_MV /
// USB_ABSENT_MV with `was_usb` as the latch.
PowerSense power_sense_rail(uint16_t rail_raw, uint32_t rail_mv, bool was_usb);
// Differential detector (rev B): `rail_raw` / `cell_raw` are the raw counts
// of the same conversions (validity), `diff_mv` = rail pin mV - cell pin
// mV, `was_usb` the previous state for the hold band. INVALID when either
// channel is saturated or floored.
PowerSense power_sense_diff(uint16_t rail_raw, uint16_t cell_raw, int32_t diff_mv, bool was_usb);

// Board revision profile, decided once per boot from the raw GPIO 33 counts
// read with the sense enable (GPIO 4) LOW (docs/HARDWARE.md "Power sensing"):
//   A  - GPIO 33 is a live divider (rev A unit): the network is always on.
//   B  - GPIO 33 saturated (rev B unit): the network is off until GPIO 4 goes
//        high; afterwards the same two channels as rev A.
//   ?  - GPIO 33 floored: no detector; INVALID -> "unknown", cell unknown.
enum BoardProfile : uint8_t {
    BOARD_UNKNOWN = 0,
    BOARD_A       = 1,
    BOARD_B       = 2,
};
BoardProfile board_profile_from_vin_raw(uint16_t vin_raw);
const char*  board_profile_str(BoardProfile b);     // "A" | "B" | "?"
// True for A and B: the cell voltage (battery %, low-battery latch) is
// measurable on GPIO 32 with the sense network enabled.
bool board_battery_measurable(BoardProfile b);

// One sample through the board's rule: A -> power_sense_rail(), B ->
// power_sense_diff(), ? -> INVALID. `rail_mv` in firmware units (x 21/10),
// `diff_mv` at the pin.
PowerSense power_sense_board(BoardProfile b, uint16_t rail_raw, uint16_t cell_raw,
                             uint32_t rail_mv, int32_t diff_mv, bool was_usb);
// The quantity the boot peak-hold maximises for this board: the rail (A)
// or the difference (B); 0 for ?.
int32_t power_boot_metric(BoardProfile b, uint32_t rail_mv, int32_t diff_mv);
const char* power_rule_str(BoardProfile b);          // "rail" | "diff" | "none"

// Debounce for the runtime polls: the first informative sample is adopted
// at once (boot decision), afterwards a different reading must repeat
// `polls` times in a row before it replaces the stable state; INVALID
// samples keep the stable state and reset the counter.
struct PowerDebounce {
    PowerSense stable = POWER_SENSE_INVALID;
    PowerSense cand   = POWER_SENSE_INVALID;
    uint8_t    count  = 0;
};
PowerSense power_debounce(PowerDebounce* d, PowerSense sample, uint8_t polls);

// Configured override (AppConfig::power_source): 0 = auto (detector),
// 1 = always USB, 2 = always battery.
enum PowerMode : uint8_t {
    POWER_MODE_AUTO    = 0,
    POWER_MODE_USB     = 1,
    POWER_MODE_BATTERY = 2,
};
// Final decision for the firmware: true = run as USB-powered. In auto mode
// an INVALID reading falls back to USB - staying awake on a battery is
// recoverable (the API/UI show "unknown" and the user sets the override),
// deep-sleeping on a mains-powered panel is not.
bool power_decide_usb(PowerMode mode, PowerSense sense);

// Exponential backoff for battery-mode retries:
//   fail_count = 0 -> base
//   fail_count = n -> min(base * 2^(n-1), cap), but never less than base.
uint32_t backoff_minutes(uint32_t base_min, uint32_t fail_count, uint32_t cap_min);

// RSSI (dBm) -> 0..3 bars; -1 when not connected ("no network").
int8_t wifi_rssi_to_bars(int rssi, bool connected);
