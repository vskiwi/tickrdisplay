#include "hal_power.h"
#include "../log.h"
#include "hal_pins.h"
#include "../logic/battery.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_adc_cal.h>

#define ADC_SAMPLES         16

static PowerSense    s_sense = POWER_SENSE_INVALID;
static PowerMode     s_mode  = POWER_MODE_AUTO;
static BoardProfile  s_board = BOARD_UNKNOWN;    // decided once in power_init()
static PowerDebounce s_debounce;                 // runtime debounce; .stable feeds the hold band
static uint16_t      s_boot_vin_raw = 0;
static uint16_t      s_cell_num = CELL_DIVIDER_NUM_DEFAULT;
static uint16_t      s_cell_den = CELL_DIVIDER_DEN_DEFAULT;

// Boot decision: peak-hold of the board's metric (logic/battery.h
// power_boot_metric(): rail mV on A, rail-cell difference on B). Through
// the stacking pads the rail sags under boot transients - the first reading
// after an OTA reboot of the stack was 0.3 V low - while a cell can never
// push the metric up, so the highest reading within the window is the
// honest one.
static bool    s_boot_decided = false;
static uint32_t s_boot_rail_min = 0, s_boot_rail_max = 0;   // the window, for /api/power/raw
static int32_t  s_boot_diff_min = 0, s_boot_diff_max = 0;

// Pins whose level is latched (gpio_hold) through deep sleep. The sense
// enable is deliberately not among them: it is driven LOW before sleeping
// and left to float (= network off on rev B, no function on rev A).
static const gpio_num_t kHeldPins[] = {
    (gpio_num_t)PIN_EPD_PWR,     // HIGH = display power OFF
    (gpio_num_t)PIN_LED_RED,     // HIGH = LED off (active LOW)
    (gpio_num_t)PIN_LED_GREEN,
    (gpio_num_t)PIN_LED_BLUE,
    (gpio_num_t)PIN_AMP_EN,      // LOW = amplifier off
};

static void set_output(int pin, int level) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
}

// Averaged calibrated pin voltage (mV) and, through `raw_out`, the averaged
// raw counts of the same conversions (saturation check).
static uint32_t read_pin_mv(int pin, uint16_t* raw_out) {
    uint32_t acc_mv = 0, acc_raw = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        acc_raw += (uint32_t)analogRead(pin);
        acc_mv += analogReadMilliVolts(pin);
    }
    if (raw_out) *raw_out = (uint16_t)(acc_raw / ADC_SAMPLES);
    return acc_mv / ADC_SAMPLES;
}

void power_sense_enable(bool on) {
    set_output(PIN_BAT_SENSE_EN, on ? HIGH : LOW);
}

bool power_sense_enabled() {
    return digitalRead(PIN_BAT_SENSE_EN) == HIGH;
}

void power_init() {
    // Pre-load the safe levels into the GPIO registers, then release the
    // deep-sleep holds so the pads switch from "held" straight to the same
    // levels without glitching (EPD power stays off, LEDs stay off).
    set_output(PIN_EPD_PWR, HIGH);
    set_output(PIN_LED_RED, HIGH);
    set_output(PIN_LED_GREEN, HIGH);
    set_output(PIN_LED_BLUE, HIGH);
    set_output(PIN_AMP_EN, LOW);
    gpio_deep_sleep_hold_dis();
    for (gpio_num_t p : kHeldPins) gpio_hold_dis(p);

    // analogReadMilliVolts() uses the eFuse ADC calibration (Vref/two-point)
    // and the default 11 dB attenuation, so the raw ADC voltages of up to
    // ~2.5 V (5.2 V / 2.1) are in range.
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_CELL, ADC_11db);
    analogSetPinAttenuation(PIN_USB_VIN, ADC_11db);

    // Board profile (logic/battery.h): GPIO 33 with the sense enable LOW -
    // the pad's reset default (input, weak pull-down), which every reset
    // path and the un-held deep sleep leave it in; set explicitly anyway.
    // A live divider = rev A, the 3.3 V ceiling = rev B (network off),
    // the floor = ?. Cheap, so every boot.
    pinMode(PIN_BAT_SENSE_EN, INPUT_PULLDOWN);
    read_pin_mv(PIN_USB_VIN, &s_boot_vin_raw);
    s_board = board_profile_from_vin_raw(s_boot_vin_raw);
    s_debounce = PowerDebounce();

    // One measurement path for both revisions: enable the network
    // and give it the stock's settling time before anything reads 32/33.
    power_sense_enable(true);
    delay(POWER_SENSE_SETTLE_MS);
}

bool power_woke_from_deep_sleep() {
    return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

BoardProfile power_get_board() {
    return s_board;
}

const char* power_board_str() {
    return board_profile_str(s_board);
}

bool power_battery_measurable() {
    return board_battery_measurable(s_board);
}

void power_set_cell_divider(uint16_t num, uint16_t den) {
    if (!cell_divider_valid(num, den)) {
        num = CELL_DIVIDER_NUM_DEFAULT;
        den = CELL_DIVIDER_DEN_DEFAULT;
    }
    s_cell_num = num;
    s_cell_den = den;
}

uint32_t power_get_vsys_mv() {
    return cell_mv_from_pin(read_pin_mv(PIN_BAT_CELL, NULL), s_cell_num, s_cell_den);
}

uint32_t power_get_vin_mv() {
    return read_pin_mv(PIN_USB_VIN, NULL) * RAIL_DIVIDER_NUM / RAIL_DIVIDER_DEN;
}

uint32_t power_get_battery_mv() {
    return power_get_vsys_mv();
}

void power_set_mode(PowerMode mode) {
    s_mode = mode;
}

PowerMode power_get_mode() {
    return s_mode;
}

PowerSense power_get_sense() {
    return s_sense;
}

const char* power_source_label() {
    if (s_mode == POWER_MODE_AUTO && s_sense == POWER_SENSE_INVALID) return "unknown";
    return power_get_source() == POWER_USB ? "usb" : "battery";
}

float power_get_voltage_vsys() {
    return power_get_vsys_mv() / 1000.0f;
}

float power_get_voltage_vin() {
    return power_get_vin_mv() / 1000.0f;
}

// One detector sample: both channels, raw counts for validity, the rail in
// firmware units (rule A) and the pin millivolt difference (rule B).
struct SenseSample {
    uint16_t rail_raw, cell_raw;
    uint32_t rail_mv;    // x 21/10
    int32_t  diff_mv;    // at the pin
};
static void take_sample(SenseSample* s) {
    uint32_t rail_pin = read_pin_mv(PIN_USB_VIN, &s->rail_raw);
    uint32_t cell_pin = read_pin_mv(PIN_BAT_CELL, &s->cell_raw);
    s->rail_mv = rail_pin * RAIL_DIVIDER_NUM / RAIL_DIVIDER_DEN;
    s->diff_mv = (int32_t)rail_pin - (int32_t)cell_pin;
}
static PowerSense decide_sample(const SenseSample& s) {
    // Latch / hold band: the debounced state; before the first decision
    // lean towards USB (stay awake - the recoverable error).
    bool was_usb = s_debounce.stable != POWER_SENSE_BATTERY;
    return power_sense_board(s_board, s.rail_raw, s.cell_raw, s.rail_mv, s.diff_mv, was_usb);
}

PowerSource power_get_source() {
    if (s_board == BOARD_UNKNOWN) {
        s_sense = POWER_SENSE_INVALID;   // no detector for this board
    } else if (!s_boot_decided) {
        // Peak-hold (see s_boot_*): the sample with the highest board
        // metric within the window decides.
        SenseSample best = {};
        int32_t best_metric = 0;
        for (uint32_t i = 0; i < POWER_BOOT_PEAK_PASSES; i++) {
            SenseSample s;
            take_sample(&s);
            int32_t m = power_boot_metric(s_board, s.rail_mv, s.diff_mv);
            if (i == 0 || m > best_metric) { best_metric = m; best = s; }
            if (i == 0 || s.rail_mv < s_boot_rail_min) s_boot_rail_min = s.rail_mv;
            if (i == 0 || s.rail_mv > s_boot_rail_max) s_boot_rail_max = s.rail_mv;
            if (i == 0 || s.diff_mv < s_boot_diff_min) s_boot_diff_min = s.diff_mv;
            if (i == 0 || s.diff_mv > s_boot_diff_max) s_boot_diff_max = s.diff_mv;
            if (i + 1 < POWER_BOOT_PEAK_PASSES) delay(POWER_BOOT_PEAK_GAP_MS);
        }
        s_boot_decided = true;
        s_sense = power_debounce(&s_debounce, decide_sample(best), POWER_SENSE_DEBOUNCE);   // first sample: adopted at once
    } else {
        // Runtime (1 s badge polls): a change must repeat.
        SenseSample s;
        take_sample(&s);
        s_sense = power_debounce(&s_debounce, decide_sample(s), POWER_SENSE_DEBOUNCE);
    }
    // The override wins over every detector.
    return power_decide_usb(s_mode, s_sense) ? POWER_USB : POWER_BATTERY;
}

static void read_channel_raw(int pin, PowerChannelRaw* c) {
    uint32_t acc_raw = 0, acc_mv = 0;
    c->pin = (uint8_t)pin;
    c->raw_min = 4095;
    c->raw_max = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        uint16_t r = (uint16_t)analogRead(pin);
        acc_raw += r;
        if (r < c->raw_min) c->raw_min = r;
        if (r > c->raw_max) c->raw_max = r;
        acc_mv += analogReadMilliVolts(pin);
    }
    c->raw = (uint16_t)(acc_raw / ADC_SAMPLES);
    c->pin_mv = acc_mv / ADC_SAMPLES;
}

void power_read_raw(PowerRaw* out) {
    read_channel_raw(PIN_BAT_CELL, &out->vsys);
    out->vsys.mv = cell_mv_from_pin(out->vsys.pin_mv, s_cell_num, s_cell_den);
    read_channel_raw(PIN_USB_VIN, &out->vin);
    out->vin.mv = out->vin.pin_mv * RAIL_DIVIDER_NUM / RAIL_DIVIDER_DEN;
    out->diff_mv = (int32_t)out->vin.pin_mv - (int32_t)out->vsys.pin_mv;
    out->sense_en = power_sense_enabled() ? 1 : 0;
    out->atten_db = 11;
    out->divider_num = RAIL_DIVIDER_NUM;
    out->divider_den = RAIL_DIVIDER_DEN;
    out->cell_num = s_cell_num;
    out->cell_den = s_cell_den;

    // Same characterisation analogReadMilliVolts() uses internally; also
    // tells which eFuse calibration this chip carries and where raw 4095
    // lands in millivolts (the ceiling a saturated channel reports).
    esp_adc_cal_characteristics_t chars = {};
    // ADC_ATTEN_DB_12 is IDF 4.4.7's new name for the 11 dB setting (DB_11 is deprecated).
    esp_adc_cal_value_t kind = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &chars);
    out->cal = kind == ESP_ADC_CAL_VAL_EFUSE_TP ? "efuse_tp" :
               kind == ESP_ADC_CAL_VAL_EFUSE_VREF ? "efuse_vref" : "default_vref";
    out->vref_mv = (uint16_t)chars.vref;
    out->adc_max_mv = esp_adc_cal_raw_to_voltage(4095, &chars);

    out->usb = power_get_source() == POWER_USB;
    out->sense = s_sense;
    out->mode = s_mode;
    out->board = s_board;
    out->boot_vin_raw = s_boot_vin_raw;
    out->boot_rail_min_mv = s_boot_rail_min;
    out->boot_rail_max_mv = s_boot_rail_max;
    out->boot_diff_min_mv = s_boot_diff_min;
    out->boot_diff_max_mv = s_boot_diff_max;
    out->rail_on_mv = USB_PRESENT_MV;
    out->rail_off_mv = USB_ABSENT_MV;
    out->diff_on_mv = POWER_DIFF_USB_MV;
    out->diff_off_mv = POWER_DIFF_BATT_MV;
    out->sat_raw = POWER_ADC_SAT_RAW;
    out->floor_raw = POWER_ADC_FLOOR_RAW;

    // Input-only pins GPIO 34..39, diagnostics only. On rev B they are the
    // *disabled* sensing network pulled towards the bus: 4095 with a cable
    // while the sense enable is low, 0 (37: a few tens) once it is high;
    // on rev A 34 idles at ~110 and 35..39 read 0 in every state
    // (docs/HARDWARE.md "Power sensing"). No decision is based on them.
    static const uint8_t kAux[POWER_AUX_PINS] = { 34, 35, 36, 37, 38, 39 };
    for (int i = 0; i < POWER_AUX_PINS; i++) {
        PowerAuxRaw* a = &out->aux[i];
        a->pin = kAux[i];
        pinMode(a->pin, INPUT);
        a->level = (uint8_t)digitalRead(a->pin);
        analogSetPinAttenuation(a->pin, ADC_11db);
        a->raw = (uint16_t)analogRead(a->pin);
        a->pin_mv = analogReadMilliVolts(a->pin);
    }
}

void power_deep_sleep(uint64_t sleep_time_us) {
    // LEDs: detach PWM so the pads are plain GPIO again, then drive OFF.
    ledcDetachPin(PIN_LED_RED);
    ledcDetachPin(PIN_LED_GREEN);
    ledcDetachPin(PIN_LED_BLUE);
    set_output(PIN_LED_RED, HIGH);
    set_output(PIN_LED_GREEN, HIGH);
    set_output(PIN_LED_BLUE, HIGH);

    // Audio: amplifier standby, DAC off.
    dacDisable(PIN_AUDIO_DAC);
    set_output(PIN_AMP_EN, LOW);

    // Display power stays OFF (display_prepare_sleep() already set it).
    set_output(PIN_EPD_PWR, HIGH);

    // Sense network off for the sleep: LOW, not held - the pad
    // floats in deep sleep, which on rev B also leaves the network off.
    power_sense_enable(false);

    // Digital pads lose their state in deep sleep unless held; without this
    // the active-LOW LEDs could float on and the display rail could float.
    // TODO(hw-verify): measure sleep current and check LEDs stay dark.
    for (gpio_num_t p : kHeldPins) gpio_hold_en(p);
    gpio_deep_sleep_hold_en();

    if (sleep_time_us > 0) {
        esp_sleep_enable_timer_wakeup(sleep_time_us);
    }

    LOGVLN("Entering Deep Sleep...");
    Serial.flush();
    esp_deep_sleep_start();
}
