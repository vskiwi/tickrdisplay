// Unity tests for the pure battery/power helpers (src/logic/battery.cpp).
// Host build: `pio test -e native -f test_battery`.
#include <unity.h>
#include "logic/battery.h"

// Measured on the bench (docs/HARDWARE.md "Power sensing"), calibrated
// pin millivolts and raw counts with the sense network enabled.
//                        rail(33) raw/mv      cell(32) raw/mv
#define A_CABLE_RAIL_RAW   2796
#define A_CABLE_RAIL_MV    2495
#define A_CABLE_CELL_RAW   2485
#define A_CABLE_CELL_MV    2233
#define A_PADS_RAIL_RAW    2530     // docs/HARDWARE.md row 1: A on top of the cable-powered B,
#define A_PADS_RAIL_MV     2255     //   vin 4.735 / vsys 4.691 in 2.1 units, same instant
#define A_PADS_CELL_RAW    2487
#define A_PADS_CELL_MV     2234
#define A_BATT_RAIL_RAW    2195
#define A_BATT_RAIL_MV     1989
#define A_BATT_CELL_RAW    2466
#define A_BATT_CELL_MV     2217
#define B_CABLE_RAIL_RAW   2669     // through the stacking pads (-0.45 V)
#define B_CABLE_RAIL_MV    2334
#define B_CABLE_CELL_RAW   2425
#define B_CABLE_CELL_MV    2132
#define B_BATT_RAIL_RAW    2286
#define B_BATT_RAIL_MV     2019
#define B_BATT_CELL_RAW    2390
#define B_BATT_CELL_MV     2105

static int32_t  diff(int32_t rail_mv, int32_t cell_mv) { return rail_mv - cell_mv; }
static uint32_t rail_fw(uint32_t rail_pin_mv) { return rail_pin_mv * RAIL_DIVIDER_NUM / RAIL_DIVIDER_DEN; }

void test_mv_to_percent_clamps_and_interpolates() {
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_percent(4300));
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_percent(4200));
    TEST_ASSERT_EQUAL_UINT8(95, battery_mv_to_percent(4150));
    TEST_ASSERT_EQUAL_UINT8(25, battery_mv_to_percent(3700));
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_percent(3300));
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_percent(2900));
}

void test_mv_to_percent_monotonic() {
    uint8_t prev = 0;
    for (uint32_t mv = 3000; mv <= 4300; mv += 10) {
        uint8_t p = battery_mv_to_percent(mv);
        TEST_ASSERT_TRUE(p >= prev);
        prev = p;
    }
}

void test_quantize() {
    TEST_ASSERT_EQUAL_UINT8(0, battery_percent_quantize(2, 5));
    TEST_ASSERT_EQUAL_UINT8(5, battery_percent_quantize(3, 5));
    TEST_ASSERT_EQUAL_UINT8(95, battery_percent_quantize(97, 5));
    TEST_ASSERT_EQUAL_UINT8(100, battery_percent_quantize(98, 5));
    TEST_ASSERT_EQUAL_UINT8(47, battery_percent_quantize(47, 0));
}

void test_level_changed_hysteresis() {
    TEST_ASSERT_FALSE(battery_level_changed(50, 54, 5));
    TEST_ASSERT_TRUE(battery_level_changed(50, 55, 5));
    TEST_ASSERT_TRUE(battery_level_changed(50, 45, 5));
    TEST_ASSERT_FALSE(battery_level_changed(50, 46, 5));
}

void test_low_battery_latch() {
    TEST_ASSERT_TRUE(battery_is_low(3299, false));
    TEST_ASSERT_FALSE(battery_is_low(3300, false));
    TEST_ASSERT_TRUE(battery_is_low(3400, true));
    TEST_ASSERT_FALSE(battery_is_low(3450, true));
}

// Cell divider 189/100 on the measured GPIO 32 points: a full,
// charging cell on both units, a resting cell of ~4.19 V (A) / ~3.98 V (B).
void test_cell_divider_on_measured_points() {
    const uint16_t n = CELL_DIVIDER_NUM_DEFAULT, d = CELL_DIVIDER_DEN_DEFAULT;
    TEST_ASSERT_EQUAL_UINT16(189, n);
    TEST_ASSERT_EQUAL_UINT16(100, d);
    TEST_ASSERT_EQUAL_UINT32(4220, cell_mv_from_pin(A_CABLE_CELL_MV, n, d));
    TEST_ASSERT_EQUAL_UINT32(4190, cell_mv_from_pin(A_BATT_CELL_MV, n, d));
    TEST_ASSERT_EQUAL_UINT32(4029, cell_mv_from_pin(B_CABLE_CELL_MV, n, d));
    TEST_ASSERT_EQUAL_UINT32(3978, cell_mv_from_pin(B_BATT_CELL_MV, n, d));
    // Every implied cell voltage is a plausible Li-ion value (the firmware's
    // old 2.1 would give 4.66 V on rev A - impossible for a cell).
    TEST_ASSERT_TRUE(cell_mv_from_pin(A_CABLE_CELL_MV, n, d) <= 4250);
    TEST_ASSERT_TRUE(cell_mv_from_pin(A_CABLE_CELL_MV, 21, 10) > 4600);
    // Explicit ratios: 2.0 and 2.1 stay usable per config.
    TEST_ASSERT_EQUAL_UINT32(4434, cell_mv_from_pin(A_BATT_CELL_MV, 200, 100));
    TEST_ASSERT_EQUAL_UINT32(4655, cell_mv_from_pin(A_BATT_CELL_MV, 21, 10));
    // Invalid ratios fall back to the default.
    TEST_ASSERT_EQUAL_UINT32(4190, cell_mv_from_pin(A_BATT_CELL_MV, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(4190, cell_mv_from_pin(A_BATT_CELL_MV, 50, 100));
}

void test_cell_divider_valid() {
    TEST_ASSERT_TRUE(cell_divider_valid(189, 100));
    TEST_ASSERT_TRUE(cell_divider_valid(21, 10));
    TEST_ASSERT_TRUE(cell_divider_valid(100, 100));    // 1.0
    TEST_ASSERT_TRUE(cell_divider_valid(4000, 1000));  // 4.0
    TEST_ASSERT_FALSE(cell_divider_valid(0, 100));
    TEST_ASSERT_FALSE(cell_divider_valid(189, 0));
    TEST_ASSERT_FALSE(cell_divider_valid(99, 100));    // < 1.0: pin above the node
    TEST_ASSERT_FALSE(cell_divider_valid(401, 100));   // > 4.0
    TEST_ASSERT_FALSE(cell_divider_valid(2000, 1001)); // den too large
}

// Battery percentage the status bar would show on the measured points
// (5 % steps); on USB the bar prints "USB" instead.
void test_percent_on_measured_points() {
    const uint16_t n = CELL_DIVIDER_NUM_DEFAULT, d = CELL_DIVIDER_DEN_DEFAULT;
    uint8_t a = battery_mv_to_percent(cell_mv_from_pin(A_BATT_CELL_MV, n, d));   // 4190 mV
    uint8_t b = battery_mv_to_percent(cell_mv_from_pin(B_BATT_CELL_MV, n, d));   // 3978 mV
    TEST_ASSERT_EQUAL_UINT8(99, a);
    TEST_ASSERT_EQUAL_UINT8(74, b);
    TEST_ASSERT_EQUAL_UINT8(100, battery_percent_quantize(a, BATTERY_DISPLAY_STEP_PCT));
    TEST_ASSERT_EQUAL_UINT8(75, battery_percent_quantize(b, BATTERY_DISPLAY_STEP_PCT));
    // Neither is anywhere near the low-battery latch.
    TEST_ASSERT_FALSE(battery_is_low(cell_mv_from_pin(A_BATT_CELL_MV, n, d), false));
    TEST_ASSERT_FALSE(battery_is_low(cell_mv_from_pin(B_BATT_CELL_MV, n, d), false));
    // Latch in pin terms: 3300 mV cell = 1746 mV at the pin; 3450 = 1825.
    TEST_ASSERT_TRUE(battery_is_low(cell_mv_from_pin(1745, n, d), false));
    TEST_ASSERT_FALSE(battery_is_low(cell_mv_from_pin(1826, n, d), true));
}

// Rev A rule: the rail in firmware units against 4.5 / 4.35 V with the
// latch, on the measured points +-20 mV pin noise (~ +-42 mV fw).
void test_power_sense_rail_measured_points() {
    TEST_ASSERT_EQUAL_UINT32(5239, rail_fw(A_CABLE_RAIL_MV));
    TEST_ASSERT_EQUAL_UINT32(4735, rail_fw(A_PADS_RAIL_MV));
    TEST_ASSERT_EQUAL_UINT32(4176, rail_fw(A_BATT_RAIL_MV));
    for (int32_t noise = -20; noise <= 20; noise += 10) {
        for (int prev = 0; prev < 2; prev++) {
            bool was_usb = prev == 1;
            TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_rail(A_CABLE_RAIL_RAW, rail_fw(A_CABLE_RAIL_MV + noise), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_rail(A_PADS_RAIL_RAW,  rail_fw(A_PADS_RAIL_MV + noise),  was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_rail(A_BATT_RAIL_RAW,  rail_fw(A_BATT_RAIL_MV + noise),  was_usb));
        }
    }
    // Enter above USB_PRESENT_MV, stay down to USB_ABSENT_MV, leave below it.
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_rail(2500, USB_PRESENT_MV, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_rail(2500, USB_PRESENT_MV + 1, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_rail(2500, USB_ABSENT_MV, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_rail(2500, USB_ABSENT_MV - 1, true));
    // Pass-through supply hovering around the plain threshold does not flap.
    bool usb = power_sense_rail(2500, 4735, false) == POWER_SENSE_USB;
    const uint32_t trace[] = { 4520, 4450, 4600, 4400, 4480, 4540 };
    for (uint32_t mv : trace) {
        usb = power_sense_rail(2500, mv, usb) == POWER_SENSE_USB;
        TEST_ASSERT_TRUE(usb);
    }
    // Validity is the rail channel's alone.
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_rail(4095, 6615, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_rail(POWER_ADC_SAT_RAW, 6600, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_rail(0, 0, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_rail(POWER_ADC_FLOOR_RAW, 30, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_rail(POWER_ADC_SAT_RAW - 1, 6500, false));
}

// Why rev A cannot use the differential rule: on the pads the rail sits at
// the level of the full charging cell (+21 mV = inside the hold band), and
// the boot sag through the pads (4.4-4.56 V fw measured before the peak-hold
// was added) makes it -140..-63 mV = "battery" - a powered stack would
// deep-sleep. The rail rule keeps 0.23 V of margin there.
void test_rev_a_pads_needs_rail_rule() {
    TEST_ASSERT_EQUAL(21, diff(A_PADS_RAIL_MV, A_PADS_CELL_MV));
    TEST_ASSERT_TRUE(diff(A_PADS_RAIL_MV, A_PADS_CELL_MV) <= POWER_DIFF_USB_MV);
    const uint32_t sag_rail_pin[] = { 4400 * 10 / 21, 4560 * 10 / 21 };   // 2095, 2171
    for (uint32_t r : sag_rail_pin) {
        int32_t d = diff((int32_t)r, A_PADS_CELL_MV);
        TEST_ASSERT_TRUE(d < POWER_DIFF_BATT_MV);
        TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_diff(A_PADS_RAIL_RAW, A_PADS_CELL_RAW, d, true));
    }
    // The board rule picks the rail for A, the difference for B.
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_sense_board(BOARD_A, A_PADS_RAIL_RAW, A_PADS_CELL_RAW,
                                                         rail_fw(A_PADS_RAIL_MV), 21, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_sense_board(BOARD_A, A_PADS_RAIL_RAW, A_PADS_CELL_RAW,
                                                         4560, -63, false));   // sagging boot sample
    TEST_ASSERT_EQUAL_STRING("rail", power_rule_str(BOARD_A));
    TEST_ASSERT_EQUAL_STRING("diff", power_rule_str(BOARD_B));
    TEST_ASSERT_EQUAL_STRING("none", power_rule_str(BOARD_UNKNOWN));
    TEST_ASSERT_EQUAL(4735, power_boot_metric(BOARD_A, 4735, 21));
    TEST_ASSERT_EQUAL(21,   power_boot_metric(BOARD_B, 4735, 21));
    TEST_ASSERT_EQUAL(0,    power_boot_metric(BOARD_UNKNOWN, 4735, 21));
}

// Rev B rule: the differential on the measured points, each with +-20 mV of
// noise on either channel (the steady reads spread <= 7 raw ~ 6 mV). The
// rev A points are included to show the rule itself agrees there too -
// except on the pads (test above).
void test_power_sense_diff_measured_points() {
    TEST_ASSERT_EQUAL(262, diff(A_CABLE_RAIL_MV, A_CABLE_CELL_MV));
    TEST_ASSERT_EQUAL(-228, diff(A_BATT_RAIL_MV, A_BATT_CELL_MV));
    TEST_ASSERT_EQUAL(202, diff(B_CABLE_RAIL_MV, B_CABLE_CELL_MV));
    TEST_ASSERT_EQUAL(-86, diff(B_BATT_RAIL_MV, B_BATT_CELL_MV));
    // Why rev B cannot use the rail rule: 4.24 V on a 3.98 V cell is 0.11 V
    // under the 4.35 V leave threshold; a full 4.2 V cell (2222 mV at the
    // pin) minus the 86 mV drop would read ~4.49 V - over it.
    TEST_ASSERT_EQUAL_UINT32(4239, rail_fw(B_BATT_RAIL_MV));
    TEST_ASSERT_TRUE(rail_fw(2222 - 86) >= USB_ABSENT_MV);
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_sense_rail(2500, rail_fw(2222 - 86), true));   // wrong: on the cell
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_board(BOARD_B, 2500, 2400, rail_fw(2222 - 86), -86, true));
    for (int32_t noise = -20; noise <= 20; noise += 10) {
        for (int prev = 0; prev < 2; prev++) {
            bool was_usb = prev == 1;
            TEST_ASSERT_EQUAL(POWER_SENSE_USB,
                power_sense_diff(A_CABLE_RAIL_RAW, A_CABLE_CELL_RAW, diff(A_CABLE_RAIL_MV + noise, A_CABLE_CELL_MV), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_USB,
                power_sense_diff(A_CABLE_RAIL_RAW, A_CABLE_CELL_RAW, diff(A_CABLE_RAIL_MV, A_CABLE_CELL_MV + noise), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY,
                power_sense_diff(A_BATT_RAIL_RAW, A_BATT_CELL_RAW, diff(A_BATT_RAIL_MV + noise, A_BATT_CELL_MV), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY,
                power_sense_diff(A_BATT_RAIL_RAW, A_BATT_CELL_RAW, diff(A_BATT_RAIL_MV, A_BATT_CELL_MV + noise), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_USB,
                power_sense_diff(B_CABLE_RAIL_RAW, B_CABLE_CELL_RAW, diff(B_CABLE_RAIL_MV + noise, B_CABLE_CELL_MV), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_USB,
                power_sense_diff(B_CABLE_RAIL_RAW, B_CABLE_CELL_RAW, diff(B_CABLE_RAIL_MV, B_CABLE_CELL_MV + noise), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY,
                power_sense_diff(B_BATT_RAIL_RAW, B_BATT_CELL_RAW, diff(B_BATT_RAIL_MV + noise, B_BATT_CELL_MV), was_usb));
            TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY,
                power_sense_diff(B_BATT_RAIL_RAW, B_BATT_CELL_RAW, diff(B_BATT_RAIL_MV, B_BATT_CELL_MV + noise), was_usb));
        }
    }
    // Margins of the weakest points to the thresholds.
    TEST_ASSERT_TRUE(diff(B_CABLE_RAIL_MV, B_CABLE_CELL_MV) - POWER_DIFF_USB_MV >= 120);   // 122
    TEST_ASSERT_TRUE(POWER_DIFF_BATT_MV - diff(B_BATT_RAIL_MV, B_BATT_CELL_MV) >= 50);     // 56
}

// The hold band between the thresholds keeps the previous state.
void test_power_sense_diff_hysteresis() {
    TEST_ASSERT_EQUAL(POWER_DIFF_USB_MV, 80);
    TEST_ASSERT_EQUAL(POWER_DIFF_BATT_MV, -30);
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_diff(2500, 2200, POWER_DIFF_USB_MV + 1, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_diff(2500, 2200, POWER_DIFF_USB_MV, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_diff(2500, 2200, POWER_DIFF_USB_MV, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_diff(2500, 2200, 0, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_diff(2500, 2200, 0, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_diff(2500, 2200, POWER_DIFF_BATT_MV, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_diff(2500, 2200, POWER_DIFF_BATT_MV, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_diff(2500, 2200, POWER_DIFF_BATT_MV - 1, true));
    // A rail wandering inside the band after a cable decision does not flap.
    bool usb = power_sense_diff(2500, 2200, 262, true) == POWER_SENSE_USB;
    const int32_t trace[] = { 70, 40, 90, -20, 10, 60 };
    for (int32_t d : trace) {
        usb = power_sense_diff(2500, 2200, d, usb) == POWER_SENSE_USB;
        TEST_ASSERT_TRUE(usb);
    }
}

// No information: a saturated or floored channel, whatever the difference.
void test_power_sense_diff_invalid() {
    // Rev B with the sense enable LOW: both channels at the 3.3 V ceiling.
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(4095, 4095, 0, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(4095, 4095, 0, true));
    // One channel saturated (would be a huge positive / negative difference).
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(4095, 2200, 1000, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(2500, 4095, -700, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(POWER_ADC_SAT_RAW, 2200, 300, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(2500, POWER_ADC_SAT_RAW, -300, false));
    // Floored (unconnected / shorted) channel.
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(0, 2200, -2000, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(2500, 0, 2300, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(POWER_ADC_FLOOR_RAW, 2200, -2000, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_sense_diff(0, 0, 0, false));
    // Just inside the informative range is a valid reading.
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_sense_diff(POWER_ADC_SAT_RAW - 1, POWER_ADC_FLOOR_RAW + 1, 2000, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_diff(POWER_ADC_FLOOR_RAW + 1, POWER_ADC_SAT_RAW - 1, -2000, true));
}

// Known risk (logic/battery.h): a weak adapter behind the stacking pads.
// 4.75 V - 0.45 V pad drop = 4.30 V rail (2048 mV at the pin through 2.1).
// Rule A: 4.30 < 4.35 V -> "battery" (as before). Rule B against a full
// 4.20 V cell (2222 mV through 1.89): difference -174 mV -> "battery" too.
// The upper unit would deep-sleep; the `power_source` override covers it.
void test_pads_weak_adapter_both_rules() {
    const int32_t rail_weak = 4300 * 10 / 21;                                             // 2047
    const int32_t cell_full = 4200 * CELL_DIVIDER_DEN_DEFAULT / CELL_DIVIDER_NUM_DEFAULT;   // 2222
    const int32_t cell_low  = 3800 * CELL_DIVIDER_DEN_DEFAULT / CELL_DIVIDER_NUM_DEFAULT;   // 2010
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_board(BOARD_A, 2400, 2500, rail_fw(rail_weak), rail_weak - cell_full, true));
    TEST_ASSERT_TRUE(rail_weak - cell_full < POWER_DIFF_BATT_MV);
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_board(BOARD_B, 2400, 2500, rail_fw(rail_weak), rail_weak - cell_full, true));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_sense_board(BOARD_B, 2400, 2500, rail_fw(rail_weak), rail_weak - cell_full, false));
    // Overrides remain the answer for that stack.
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_USB, POWER_SENSE_BATTERY));
    // Rule B with a cell at 3.8 V behind the same adapter: rail 37 mV above
    // -> hold band, the previous (USB) decision stands.
    TEST_ASSERT_TRUE(rail_weak - cell_low > POWER_DIFF_BATT_MV && rail_weak - cell_low <= POWER_DIFF_USB_MV);
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_sense_diff(2400, 2300, rail_weak - cell_low, true));
    // Rule B, rev B on the pads with a FULL cell (extrapolated - the measured
    // point had the cell at 4.03 V): 2334 - 2222 = +112 mV, 32 mV over the
    // threshold. Still USB; the thinnest margin of the rule, to be checked
    // on the bench with a charged rev B unit on top of the rev A unit.
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_sense_diff(B_CABLE_RAIL_RAW, B_CABLE_CELL_RAW, B_CABLE_RAIL_MV - cell_full, false));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_sense_diff(B_CABLE_RAIL_RAW, B_CABLE_CELL_RAW, 202, false));
}

// Board profile from the boot reading of GPIO 33 with the sense enable low
// (docs/HARDWARE.md "Power sensing"): the rev A unit reads 2195..3190 raw in every
// power state, the rev B unit reads 4095 (network off) in every power state.
void test_board_profile_from_measured_vin() {
    TEST_ASSERT_EQUAL(BOARD_A, board_profile_from_vin_raw(2447));   // rev A on the pads
    TEST_ASSERT_EQUAL(BOARD_A, board_profile_from_vin_raw(A_BATT_RAIL_RAW));
    TEST_ASSERT_EQUAL(BOARD_A, board_profile_from_vin_raw(3190));   // rev A on USB
    TEST_ASSERT_EQUAL(BOARD_A, board_profile_from_vin_raw(POWER_ADC_SAT_RAW - 1));
    TEST_ASSERT_EQUAL(BOARD_B, board_profile_from_vin_raw(4095));   // rev B, cable in or out
    TEST_ASSERT_EQUAL(BOARD_B, board_profile_from_vin_raw(POWER_ADC_SAT_RAW));
    TEST_ASSERT_EQUAL(BOARD_UNKNOWN, board_profile_from_vin_raw(0));
    TEST_ASSERT_EQUAL(BOARD_UNKNOWN, board_profile_from_vin_raw(POWER_ADC_FLOOR_RAW));
    TEST_ASSERT_EQUAL_STRING("A", board_profile_str(BOARD_A));
    TEST_ASSERT_EQUAL_STRING("B", board_profile_str(BOARD_B));
    TEST_ASSERT_EQUAL_STRING("?", board_profile_str(BOARD_UNKNOWN));
    // The cell is measurable on both known revisions.
    TEST_ASSERT_TRUE(board_battery_measurable(BOARD_A));
    TEST_ASSERT_TRUE(board_battery_measurable(BOARD_B));
    TEST_ASSERT_FALSE(board_battery_measurable(BOARD_UNKNOWN));
}

void test_power_debounce() {
    PowerDebounce d;
    // Nothing known yet: an INVALID sample stays unknown, the first real one is adopted.
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_debounce(&d, POWER_SENSE_INVALID, POWER_SENSE_DEBOUNCE));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_debounce(&d, POWER_SENSE_USB, POWER_SENSE_DEBOUNCE));
    // Cable pulled: battery must repeat 3 times.
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_debounce(&d, POWER_SENSE_BATTERY, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_debounce(&d, POWER_SENSE_BATTERY, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_BATTERY, 3));
    // A single glitch does not flip it; an INVALID sample resets the run.
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_USB, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_BATTERY, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_USB, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_USB, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_INVALID, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_USB, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_USB, 3));
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     power_debounce(&d, POWER_SENSE_USB, 3));
    // polls <= 1: immediate.
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&d, POWER_SENSE_BATTERY, 1));
}

// The boot peak-hold as hal_power.cpp runs it: the sample with the highest
// board metric out of the window decides through the board rule.
struct Point { BoardProfile board; uint16_t rail_raw, cell_raw; int32_t rail_mv, cell_mv; };
static PowerSense boot_decide(const Point& p, int32_t sag_passes, int32_t sag_pin_mv) {
    int32_t best_metric = 0, best_rail = 0, best_diff = 0;
    for (uint32_t i = 0; i < POWER_BOOT_PEAK_PASSES; i++) {
        int32_t rail_pin = p.rail_mv + ((int32_t)i < sag_passes ? sag_pin_mv : 0);
        int32_t d = diff(rail_pin, p.cell_mv);
        int32_t m = power_boot_metric(p.board, rail_fw((uint32_t)rail_pin), d);
        if (i == 0 || m > best_metric) { best_metric = m; best_rail = rail_pin; best_diff = d; }
    }
    PowerDebounce db;
    return power_debounce(&db, power_sense_board(p.board, p.rail_raw, p.cell_raw, rail_fw((uint32_t)best_rail), best_diff, true),
                          POWER_SENSE_DEBOUNCE);
}

// End-to-end on the measured points: profile, boot peak-hold, board rule,
// runtime debounce, final decision - one path, two rules.
void test_power_chain_both_revisions() {
    const Point a_cable = { BOARD_A, A_CABLE_RAIL_RAW, A_CABLE_CELL_RAW, A_CABLE_RAIL_MV, A_CABLE_CELL_MV };
    const Point a_pads  = { BOARD_A, A_PADS_RAIL_RAW,  A_PADS_CELL_RAW,  A_PADS_RAIL_MV,  A_PADS_CELL_MV };
    const Point a_batt  = { BOARD_A, A_BATT_RAIL_RAW,  A_BATT_CELL_RAW,  A_BATT_RAIL_MV,  A_BATT_CELL_MV };
    const Point b_cable = { BOARD_B, B_CABLE_RAIL_RAW, B_CABLE_CELL_RAW, B_CABLE_RAIL_MV, B_CABLE_CELL_MV };
    const Point b_batt  = { BOARD_B, B_BATT_RAIL_RAW,  B_BATT_CELL_RAW,  B_BATT_RAIL_MV,  B_BATT_CELL_MV };

    // Profiles from GPIO 33 with the enable low: A live, B saturated.
    TEST_ASSERT_EQUAL(BOARD_A, board_profile_from_vin_raw(A_CABLE_RAIL_RAW));
    TEST_ASSERT_EQUAL(BOARD_B, board_profile_from_vin_raw(4095));

    // Boot with a cable / on the pads: the rail sags during boot (through the
    // pads the first readings were 0.3 V fw = ~140 mV at the pin low); the
    // peak within the window decides. Sagging first half, then steady.
    const Point* usb_boot[] = { &a_cable, &a_pads, &b_cable };
    for (const Point* p : usb_boot) {
        PowerSense s = boot_decide(*p, 6, -140);
        TEST_ASSERT_EQUAL(POWER_SENSE_USB, s);
        TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_AUTO, s));
    }
    // Sagging for the whole window (the OTA-reboot case, 4.4 V measured):
    // with no prior state the boot resolves the band towards USB (stay
    // awake), so rule A accepts 4.44 V (>= 4.35) on the pads; below the
    // band (4.32 V) it is battery - the limit of the detector.
    TEST_ASSERT_EQUAL(POWER_SENSE_USB,     boot_decide(a_pads, (int32_t)POWER_BOOT_PEAK_PASSES, -140));   // 4.44 V
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, boot_decide(a_pads, (int32_t)POWER_BOOT_PEAK_PASSES, -200));   // 4.32 V
    // Boot on the cell: no sample can lift the metric into the USB range.
    const Point* batt_boot[] = { &a_batt, &b_batt };
    for (const Point* p : batt_boot) {
        PowerSense s = boot_decide(*p, 0, 0);
        TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, s);
        TEST_ASSERT_FALSE(power_decide_usb(POWER_MODE_AUTO, s));   // -> deep sleep
        // A +40 mV upward glitch on the rail does not either (A: 4.26 V
        // against 4.35; B: -46 against -30 - the rule's thinnest margin).
        TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, boot_decide(*p, 3, 40));
    }
    // Margins of the boot decision on the cell, with the USB-leaning band:
    // A 0.17 V fw (the rail on the cell is capped at cell - 0.47 V), B 56 mV.
    TEST_ASSERT_TRUE(USB_ABSENT_MV - rail_fw(A_BATT_RAIL_MV) >= 170);
    TEST_ASSERT_TRUE(POWER_DIFF_BATT_MV - diff(B_BATT_RAIL_MV, B_BATT_CELL_MV) >= 56);

    // Runtime on rev B: cable pulled, three 1 s polls until the state
    // follows; plugged back in, three polls again. The hold band uses the
    // debounced state.
    PowerDebounce rt;
    power_debounce(&rt, power_sense_diff(b_cable.rail_raw, b_cable.cell_raw, 202, true), POWER_SENSE_DEBOUNCE);
    for (int i = 0; i < 2; i++) {
        bool was_usb = rt.stable != POWER_SENSE_BATTERY;
        TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_debounce(&rt, power_sense_diff(b_batt.rail_raw, b_batt.cell_raw, -86, was_usb), POWER_SENSE_DEBOUNCE));
    }
    TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&rt, power_sense_diff(b_batt.rail_raw, b_batt.cell_raw, -86, true), POWER_SENSE_DEBOUNCE));
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL(POWER_SENSE_BATTERY, power_debounce(&rt, power_sense_diff(b_cable.rail_raw, b_cable.cell_raw, 202, false), POWER_SENSE_DEBOUNCE));
    }
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_debounce(&rt, power_sense_diff(b_cable.rail_raw, b_cable.cell_raw, 202, false), POWER_SENSE_DEBOUNCE));

    // Rev B with the enable low (dev probe `low`, or before power_init()):
    // both channels saturated -> INVALID -> hold / "unknown" -> stay awake.
    TEST_ASSERT_EQUAL(POWER_SENSE_USB, power_debounce(&rt, power_sense_diff(4095, 4095, 0, true), POWER_SENSE_DEBOUNCE));
    PowerDebounce fresh;
    TEST_ASSERT_EQUAL(POWER_SENSE_INVALID, power_debounce(&fresh, power_sense_diff(4095, 4095, 0, true), POWER_SENSE_DEBOUNCE));
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_AUTO, POWER_SENSE_INVALID));

    // Override still on top of both.
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_USB, POWER_SENSE_BATTERY));
    TEST_ASSERT_FALSE(power_decide_usb(POWER_MODE_BATTERY, POWER_SENSE_USB));
    // Unknown board: no detector -> INVALID -> stay awake.
    TEST_ASSERT_EQUAL(BOARD_UNKNOWN, board_profile_from_vin_raw(0));
    TEST_ASSERT_FALSE(board_battery_measurable(BOARD_UNKNOWN));
}

void test_power_decide() {
    // auto: detector wins, invalid falls back to USB (stay awake)
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_AUTO, POWER_SENSE_USB));
    TEST_ASSERT_FALSE(power_decide_usb(POWER_MODE_AUTO, POWER_SENSE_BATTERY));
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_AUTO, POWER_SENSE_INVALID));
    // overrides ignore the detector
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_USB, POWER_SENSE_BATTERY));
    TEST_ASSERT_TRUE(power_decide_usb(POWER_MODE_USB, POWER_SENSE_INVALID));
    TEST_ASSERT_FALSE(power_decide_usb(POWER_MODE_BATTERY, POWER_SENSE_USB));
    TEST_ASSERT_FALSE(power_decide_usb(POWER_MODE_BATTERY, POWER_SENSE_INVALID));
}

void test_backoff() {
    TEST_ASSERT_EQUAL_UINT32(5, backoff_minutes(5, 0, 60));
    TEST_ASSERT_EQUAL_UINT32(5, backoff_minutes(5, 1, 60));
    TEST_ASSERT_EQUAL_UINT32(10, backoff_minutes(5, 2, 60));
    TEST_ASSERT_EQUAL_UINT32(40, backoff_minutes(5, 4, 60));
    TEST_ASSERT_EQUAL_UINT32(60, backoff_minutes(5, 5, 60));
    TEST_ASSERT_EQUAL_UINT32(60, backoff_minutes(5, 40, 60));
    TEST_ASSERT_EQUAL_UINT32(120, backoff_minutes(120, 3, 60));
}

void test_rssi_bars() {
    TEST_ASSERT_EQUAL_INT8(3, wifi_rssi_to_bars(-40, true));
    TEST_ASSERT_EQUAL_INT8(2, wifi_rssi_to_bars(-60, true));
    TEST_ASSERT_EQUAL_INT8(1, wifi_rssi_to_bars(-80, true));
    TEST_ASSERT_EQUAL_INT8(0, wifi_rssi_to_bars(-95, true));
    TEST_ASSERT_EQUAL_INT8(-1, wifi_rssi_to_bars(0, true));
    TEST_ASSERT_EQUAL_INT8(-1, wifi_rssi_to_bars(-40, false));
}

void setUp() {}
void tearDown() {}

int runUnityTests() {
    UNITY_BEGIN();
    RUN_TEST(test_mv_to_percent_clamps_and_interpolates);
    RUN_TEST(test_mv_to_percent_monotonic);
    RUN_TEST(test_quantize);
    RUN_TEST(test_level_changed_hysteresis);
    RUN_TEST(test_low_battery_latch);
    RUN_TEST(test_cell_divider_on_measured_points);
    RUN_TEST(test_cell_divider_valid);
    RUN_TEST(test_percent_on_measured_points);
    RUN_TEST(test_power_sense_rail_measured_points);
    RUN_TEST(test_rev_a_pads_needs_rail_rule);
    RUN_TEST(test_power_sense_diff_measured_points);
    RUN_TEST(test_power_sense_diff_hysteresis);
    RUN_TEST(test_power_sense_diff_invalid);
    RUN_TEST(test_pads_weak_adapter_both_rules);
    RUN_TEST(test_board_profile_from_measured_vin);
    RUN_TEST(test_power_debounce);
    RUN_TEST(test_power_chain_both_revisions);
    RUN_TEST(test_power_decide);
    RUN_TEST(test_backoff);
    RUN_TEST(test_rssi_bars);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
    delay(2000);
    runUnityTests();
}
void loop() {}
#else
int main() { return runUnityTests(); }
#endif
