#pragma once
#include <Arduino.h>
#include "../logic/battery.h"   // PowerSense, PowerMode, thresholds, dividers

enum PowerSource {
    POWER_BATTERY,
    POWER_USB
};

// Must be called first in setup(): releases GPIO holds left over from deep
// sleep, configures the ADC, decides the board profile (GPIO 33 read with
// the sense enable LOW), then drives PIN_BAT_SENSE_EN high and waits
// POWER_SENSE_SETTLE_MS. From here on GPIO 32 = cell, GPIO 33 = rail on
// both revisions (docs/HARDWARE.md "Power sensing").
void power_init();

// True when this boot is a wake-up from deep sleep (not a cold boot/reset).
bool power_woke_from_deep_sleep();

// Board revision profile decided in power_init() (logic/battery.h
// BoardProfile): A - network always on; B - switched by GPIO 4; ? - GPIO 33
// floored, no detector -> "unknown" (behaves as USB). A and B share one
// detector and one cell measurement.
BoardProfile power_get_board();
const char*  power_board_str();          // "A" | "B" | "?"
// True on A and B. False on ?: power_get_*_mv() carry no information there
// (the API reports null, the battery badge no percentage, the low-battery
// check is skipped).
bool power_battery_measurable();

// Sense-network enable (PIN_BAT_SENSE_EN). power_init() turns it on,
// power_deep_sleep() off; the dev probe's `restore` mode calls it too.
void power_sense_enable(bool on);
bool power_sense_enabled();              // digitalRead() of the pad

// Cell divider (AppConfig::adc_cell_num/den), applied by main.cpp before
// the first reading; falls back to the default when invalid.
void power_set_cell_divider(uint16_t num, uint16_t den);

// Configured override (AppConfig::power_source), applied by main.cpp before
// the first power_get_source(). Default POWER_MODE_AUTO.
void      power_set_mode(PowerMode mode);
PowerMode power_get_mode();

// Final decision. Same channels on both revisions, the rule per board
// (power_sense_board()): A - rail thresholds 4.5 / 4.35 V with hysteresis;
// B - rail-cell difference at the pin, USB above +80 mV, battery below
// -30 mV, hold in between; INVALID when a channel is saturated or floored.
// Boot = peak-hold of the board's metric over POWER_BOOT_PEAK_PASSES
// readings, runtime = 1 s polls through a POWER_SENSE_DEBOUNCE debounce.
// Unknown board: INVALID. INVALID in auto mode falls back to USB
// (power_decide_usb()); overrides bypass the detector.
PowerSource power_get_source();
// Detector result of the last power_get_source() (INVALID = "unknown").
PowerSense  power_get_sense();
// "usb" | "battery" | "unknown" for the API; "unknown" only in auto mode
// with an invalid reading (the firmware then behaves as USB).
const char* power_source_label();

// Divider-corrected voltages, averaged over several calibrated ADC samples.
// Meaningful when power_battery_measurable(); on board ? they read the
// ceiling or the floor.
uint32_t power_get_vsys_mv();   // GPIO 32 x cell divider - the cell ("vsys_v" in the API)
uint32_t power_get_vin_mv();    // GPIO 33 x 21/10 - the rail: USB ~5.2 / pads ~4.7 / cell minus a diode
// Voltage the battery level is derived from: the cell (= power_get_vsys_mv()).
uint32_t power_get_battery_mv();

// Float wrappers kept for the web UI / legacy callers.
float power_get_voltage_vsys();
float power_get_voltage_vin();

// --- Raw diagnostics (GET /api/power/raw, docs/HARDWARE.md "Power sensing") -----
// Everything the divider maths is based on, so the constants can be checked
// against a multimeter on each board revision.
struct PowerChannelRaw {
    uint8_t  pin;
    uint16_t raw;        // mean of ADC counts (12 bit, 0..4095); 4095 = saturated
    uint16_t raw_min;
    uint16_t raw_max;
    uint32_t pin_mv;     // mean of analogReadMilliVolts(): eFuse-calibrated voltage AT THE PIN
    uint32_t mv;         // pin_mv x the channel's divider - what the firmware reports as vsys/vin
};
struct PowerAuxRaw {
    uint8_t  pin;        // GPIO 34..39 (input-only; the stock firmware reads "analog 37")
    uint16_t raw;
    uint32_t pin_mv;
    uint8_t  level;      // digitalRead()
};
#define POWER_AUX_PINS 6
struct PowerRaw {
    PowerChannelRaw vsys;          // GPIO 32, the cell
    PowerChannelRaw vin;           // GPIO 33, the rail
    int32_t     diff_mv;           // vin.pin_mv - vsys.pin_mv (the detector's input)
    uint8_t     sense_en;          // level of PIN_BAT_SENSE_EN (1 while awake)
    uint8_t     atten_db;          // 11
    uint16_t    divider_num;       // rail: 21
    uint16_t    divider_den;       //       10
    uint16_t    cell_num;          // cell: 189 by default (config)
    uint16_t    cell_den;          //       100
    const char* cal;               // "efuse_tp" | "efuse_vref" | "default_vref"
    uint16_t    vref_mv;           // characterised Vref
    uint32_t    adc_max_mv;        // calibrated reading of raw 4095 (the ceiling)
    bool        usb;               // final decision (mode + detector)
    PowerSense  sense;             // detector: battery / usb / invalid
    PowerMode   mode;              // configured override
    BoardProfile board;            // A / B / ? (power_init())
    uint16_t    boot_vin_raw;      // GPIO 33 counts the profile was decided from (sense enable low)
    uint32_t    boot_rail_min_mv;  // lowest / highest rail (fw units) and rail-cell difference
    uint32_t    boot_rail_max_mv;  //   (pin mV) in the boot peak-hold window; the maximum of
    int32_t     boot_diff_min_mv;  //   the board's metric decided (A: rail, B: diff)
    int32_t     boot_diff_max_mv;
    uint16_t    rail_on_mv;        // USB_PRESENT_MV  (rule A)
    uint16_t    rail_off_mv;       // USB_ABSENT_MV
    int16_t     diff_on_mv;        // POWER_DIFF_USB_MV  (rule B)
    int16_t     diff_off_mv;       // POWER_DIFF_BATT_MV
    uint16_t    sat_raw;           // raw >= this = saturated
    uint16_t    floor_raw;         // raw <= this = floored
    PowerAuxRaw aux[POWER_AUX_PINS];
};
void power_read_raw(PowerRaw* out);

// Powers down peripherals (LEDs, amplifier), drops the sense enable,
// latches safe GPIO levels and enters deep sleep. The display must already
// be put to sleep by the caller (display_prepare_sleep()). Never returns.
void power_deep_sleep(uint64_t sleep_time_us);
