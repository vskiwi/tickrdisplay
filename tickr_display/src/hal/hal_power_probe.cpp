// Developer-only power-sensing probe; see hal_power_probe.h. Whole file is
// compiled only with -DTICKR_POWER_PROBE (env tickr_dev).
#ifdef TICKR_POWER_PROBE
#include "hal_power_probe.h"
#include "hal_power.h"
#include "hal_pins.h"
#include "../logic/battery.h"
#include <Arduino.h>
#include <driver/adc.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_adc_cal.h>
#include <soc/gpio_periph.h>
#include <soc/io_mux_reg.h>

#define ADC2_SAMPLES   8
#define PROBE_SETTLE_MS 5   // internal pull (~45 k) against a possible 100 nF on a status line: 4.5 ms

// GPIO 0..39 as they exist on the ESP32 (20, 24, 28-31 do not). Exclusions
// follow docs/HARDWARE.md "Pin configuration" and hal_pins.h. Strapping pins
// (0, 2, 12, 15) are only sampled at reset, so reading them with a pull for
// a few ms at runtime changes nothing; they are put back afterwards anyway.
static const PowerProbePin kPins[] = {
    {  0,  1, PROBE_PIN_FREE,       "strapping BOOT (pull-up at reset)" },
    {  1, -1, PROBE_PIN_EXCLUDED,   "UART0 TX" },
    {  2,  2, PROBE_PIN_FREE,       "strapping (pull-down at reset)" },
    {  3, -1, PROBE_PIN_EXCLUDED,   "UART0 RX" },
    {  4,  0, PROBE_PIN_BOOT_ONLY,  "PIN_BAT_SENSE_EN: driven HIGH by power_init() (rev B sense-network enable); POST /api/power/probe/gpio4 overrides it, a runtime pull probe would undo it" },
    {  5, -1, PROBE_PIN_FREE,       "strapping SDIO timing (pull-up at reset)" },
    {  6, -1, PROBE_PIN_EXCLUDED,   "SPI flash" },
    {  7, -1, PROBE_PIN_EXCLUDED,   "SPI flash" },
    {  8, -1, PROBE_PIN_EXCLUDED,   "SPI flash" },
    {  9, -1, PROBE_PIN_EXCLUDED,   "SPI flash" },
    { 10, -1, PROBE_PIN_EXCLUDED,   "SPI flash" },
    { 11, -1, PROBE_PIN_EXCLUDED,   "SPI flash" },
    { 12,  5, PROBE_PIN_FREE,       "strapping MTDI / flash voltage (pull-down at reset)" },
    { 13,  4, PROBE_PIN_EXCLUDED,   "EPD SPI CLK" },
    { 14,  6, PROBE_PIN_EXCLUDED,   "EPD SPI MOSI" },
    { 15,  3, PROBE_PIN_EXCLUDED,   "EPD CS (strapping)" },
    { 16, -1, PROBE_PIN_BOOT_ONLY,  "SPI dummy MISO input after HAL init (PIN_SPI_MISO_DUMMY, every env)" },
    { 17, -1, PROBE_PIN_EXCLUDED,   "amplifier enable" },
    { 18, -1, PROBE_PIN_EXCLUDED,   "EPD BUSY" },
    { 19, -1, PROBE_PIN_EXCLUDED,   "EPD power EP_3V3_EN" },
    { 21, -1, PROBE_PIN_EXCLUDED,   "LED blue" },
    { 22, -1, PROBE_PIN_EXCLUDED,   "LED green" },
    { 23, -1, PROBE_PIN_EXCLUDED,   "LED red" },
    { 25,  8, PROBE_PIN_EXCLUDED,   "DAC1 audio" },
    { 26,  9, PROBE_PIN_EXCLUDED,   "EPD RST" },
    { 27,  7, PROBE_PIN_EXCLUDED,   "EPD DC" },
    { 32, -1, PROBE_PIN_FREE,       "ADC1 cell channel (pull test is harmless; the ADC re-attaches on the next read)" },
    { 33, -1, PROBE_PIN_FREE,       "ADC1 rail channel (idem)" },
    { 34, -1, PROBE_PIN_INPUT_ONLY, NULL },
    { 35, -1, PROBE_PIN_INPUT_ONLY, NULL },
    { 36, -1, PROBE_PIN_INPUT_ONLY, "SENSOR_VP" },
    { 37, -1, PROBE_PIN_INPUT_ONLY, "stock firmware: 'analog 37' charging indicator and wake source" },
    { 38, -1, PROBE_PIN_INPUT_ONLY, NULL },
    { 39, -1, PROBE_PIN_INPUT_ONLY, "SENSOR_VN" },
};
#define PIN_COUNT (sizeof(kPins) / sizeof(kPins[0]))
// The table above hard-codes the pin roles; keep it in step with hal_pins.h.
static_assert(PIN_SPI_MISO_DUMMY == 16, "hal_power_probe.cpp pin table assumes the dummy MISO on GPIO 16");
static_assert(PIN_BAT_SENSE_EN == 4, "hal_power_probe.cpp pin table assumes the sense enable on GPIO 4");
#define PIN_GPIO4_TEST PIN_BAT_SENSE_EN

static const char*    s_boot_hint = "not-run";
static uint16_t       s_boot_vin_raw = 0;
static uint32_t       s_boot_vin_mv = 0;
static PowerProbeAdc2 s_adc2[PIN_COUNT];
static size_t         s_adc2_n = 0;
static PowerProbeGpio s_gpio_boot[PIN_COUNT];

size_t power_probe_pin_count() { return PIN_COUNT; }
const PowerProbePin* power_probe_pin(size_t i) { return i < PIN_COUNT ? &kPins[i] : NULL; }
const char* power_probe_boot_hint() { return s_boot_hint; }
uint16_t power_probe_boot_vin_raw() { return s_boot_vin_raw; }
uint32_t power_probe_boot_vin_pin_mv() { return s_boot_vin_mv; }
size_t power_probe_adc2_count() { return s_adc2_n; }
const PowerProbeAdc2* power_probe_adc2(size_t i) { return i < s_adc2_n ? &s_adc2[i] : NULL; }
size_t power_probe_gpio_count() { return PIN_COUNT; }
const PowerProbeGpio* power_probe_gpio_boot(size_t i) { return i < PIN_COUNT ? &s_gpio_boot[i] : NULL; }
uint16_t power_probe_settle_ms() { return PROBE_SETTLE_MS; }

// Restores what the probe may have changed on a pad: the RTC mux (ADC /
// rtc_gpio routing) back to the digital GPIO matrix, the output driver off,
// and the IO_MUX register (function select, input enable, pull-up/-down)
// exactly as it was before.
static void restore_pin(uint8_t gpio, uint32_t saved_mux) {
    if (rtc_gpio_is_valid_gpio((gpio_num_t)gpio)) rtc_gpio_deinit((gpio_num_t)gpio);
    pinMode(gpio, INPUT);
    WRITE_PERI_REG(GPIO_PIN_MUX_REG[gpio], saved_mux);
}

static uint8_t read_level(uint8_t gpio, uint8_t mode) {
    pinMode(gpio, mode);
    delay(PROBE_SETTLE_MS);
    return (uint8_t)digitalRead(gpio);
}

static void probe_gpio(const PowerProbePin& p, PowerProbeGpio* g, bool at_boot) {
    g->gpio = p.gpio;
    g->lvl_float = g->lvl_pu = g->lvl_pd = PROBE_NA;
    g->probed = false;
    if (p.kind == PROBE_PIN_EXCLUDED) return;
    if (p.kind == PROBE_PIN_BOOT_ONLY && !at_boot) return;

    uint32_t saved_mux = READ_PERI_REG(GPIO_PIN_MUX_REG[p.gpio]);
    g->lvl_float = read_level(p.gpio, INPUT);
    if (p.kind != PROBE_PIN_INPUT_ONLY) {          // 34-39 have no pull resistors; gpio_config() rejects them
        g->lvl_pu = read_level(p.gpio, INPUT_PULLUP);
        g->lvl_pd = read_level(p.gpio, INPUT_PULLDOWN);
    }
    restore_pin(p.gpio, saved_mux);
    g->probed = true;
}

static void scan_adc2(const PowerProbePin& p) {
    PowerProbeAdc2* a = &s_adc2[s_adc2_n++];
    a->gpio = p.gpio;
    a->ch = p.adc2_ch;
    a->ok = false;
    a->raw = 0; a->raw_min = 4095; a->raw_max = 0; a->mv = 0;

    uint32_t saved_mux = READ_PERI_REG(GPIO_PIN_MUX_REG[p.gpio]);
    // ADC_ATTEN_DB_12 is IDF 4.4.7's name for the 11 dB setting (same as hal_power.cpp).
    if (adc2_config_channel_atten((adc2_channel_t)p.adc2_ch, ADC_ATTEN_DB_12) != ESP_OK) {
        restore_pin(p.gpio, saved_mux);
        return;
    }
    esp_adc_cal_characteristics_t chars = {};
    esp_adc_cal_characterize(ADC_UNIT_2, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &chars);

    uint32_t acc = 0;
    int n_ok = 0;
    for (int i = 0; i < ADC2_SAMPLES; i++) {
        int v = 0;
        if (adc2_get_raw((adc2_channel_t)p.adc2_ch, ADC_WIDTH_BIT_12, &v) != ESP_OK) break;
        n_ok++;
        acc += (uint32_t)v;
        if (v < a->raw_min) a->raw_min = (uint16_t)v;
        if (v > a->raw_max) a->raw_max = (uint16_t)v;
    }
    if (n_ok == ADC2_SAMPLES) {
        a->ok = true;
        a->raw = (uint16_t)(acc / ADC2_SAMPLES);
        a->mv = esp_adc_cal_raw_to_voltage(a->raw, &chars);
    }
    restore_pin(p.gpio, saved_mux);
}

void power_probe_boot() {
    // 1. What the detector would say right now, i.e. with the sense enable
    //    still at its reset default (low): rev A answers through its rail
    //    rule, rev B has both channels saturated -> "invalid" (hal_power.cpp
    //    repeats the same 16-sample reads after power_init() has driven
    //    GPIO 4 high).
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_USB_VIN, ADC_11db);
    analogSetPinAttenuation(PIN_BAT_CELL, ADC_11db);
    uint32_t acc_raw = 0, acc_mv = 0, cell_raw = 0, cell_mv = 0;
    for (int i = 0; i < 16; i++) {
        acc_raw += (uint32_t)analogRead(PIN_USB_VIN);
        acc_mv += analogReadMilliVolts(PIN_USB_VIN);
        cell_raw += (uint32_t)analogRead(PIN_BAT_CELL);
        cell_mv += analogReadMilliVolts(PIN_BAT_CELL);
    }
    s_boot_vin_raw = (uint16_t)(acc_raw / 16);
    s_boot_vin_mv = acc_mv / 16;
    PowerSense sense = power_sense_board(board_profile_from_vin_raw(s_boot_vin_raw), s_boot_vin_raw,
                                         (uint16_t)(cell_raw / 16), s_boot_vin_mv * RAIL_DIVIDER_NUM / RAIL_DIVIDER_DEN,
                                         (int32_t)s_boot_vin_mv - (int32_t)(cell_mv / 16), true);
    s_boot_hint = sense == POWER_SENSE_USB ? "usb" : sense == POWER_SENSE_BATTERY ? "battery" : "invalid";
    Serial.printf("[probe] boot ADC1 rail(GPIO%d): raw %u, %u mV at pin; cell(GPIO%d): raw %u, %u mV -> %s\n",
                  PIN_USB_VIN, s_boot_vin_raw, s_boot_vin_mv, PIN_BAT_CELL, (unsigned)(cell_raw / 16),
                  (unsigned)(cell_mv / 16), s_boot_hint);

    // 2. ADC2 (only possible before Wi-Fi).
    s_adc2_n = 0;
    for (size_t i = 0; i < PIN_COUNT; i++) {
        const PowerProbePin& p = kPins[i];
        if (p.adc2_ch < 0 || p.kind == PROBE_PIN_EXCLUDED) continue;
        scan_adc2(p);
        const PowerProbeAdc2& a = s_adc2[s_adc2_n - 1];
        Serial.printf("[probe] ADC2 GPIO%u ch%d: %s raw %u (%u..%u) %u mV\n", a.gpio, a.ch,
                      a.ok ? "ok" : "FAILED", a.raw, a.raw_min, a.raw_max, a.mv);
    }

    // 3. Digital pull probe.
    for (size_t i = 0; i < PIN_COUNT; i++) {
        probe_gpio(kPins[i], &s_gpio_boot[i], true);
        if (!s_gpio_boot[i].probed) continue;
        const PowerProbeGpio& g = s_gpio_boot[i];
        Serial.printf("[probe] GPIO%u: float %u pu %u pd %u -> %s\n", g.gpio, g.lvl_float, g.lvl_pu, g.lvl_pd,
                      power_probe_verdict(g, kPins[i]));
    }
}

void power_probe_gpio_now(PowerProbeGpio* out) {
    for (size_t i = 0; i < PIN_COUNT; i++) probe_gpio(kPins[i], &out[i], false);
}

const char* power_probe_verdict(const PowerProbeGpio& g, const PowerProbePin& p) {
    if (!g.probed) return "skipped";
    if (p.kind == PROBE_PIN_INPUT_ONLY) return "level_only";
    if (g.lvl_pu == g.lvl_pd) return g.lvl_pu ? "bound_high" : "bound_low";
    if (g.lvl_pu == 1 && g.lvl_pd == 0) return "free";
    return "odd";   // follows the pull inverted - should not happen
}

// --- GPIO 4 enable test (see the header) -----------------------------------
static const char* s_gpio4_mode = "untouched";   // = the firmware's HIGH until the first call

bool power_probe_gpio4_set(const char* mode) {
    static const struct { const char* name; uint8_t pin_mode; int8_t level; } kModes[] = {
        { "high",     OUTPUT,         HIGH },
        { "low",      OUTPUT,         LOW  },
        { "pulldown", INPUT_PULLDOWN, -1   },
        { "pullup",   INPUT_PULLUP,   -1   },
        { "float",    INPUT,          -1   },
        { "restore",  0,              -1   },
    };
    size_t i = 0;
    while (i < sizeof(kModes) / sizeof(kModes[0]) && strcmp(mode, kModes[i].name) != 0) i++;
    if (i == sizeof(kModes) / sizeof(kModes[0])) return false;

    if (kModes[i].pin_mode == 0) {
        // The firmware's state: sense enable driven HIGH (power_init()).
        power_sense_enable(true);
    } else {
        // Level first, then the output enable: no glitch through the old
        // output-register value when switching to OUTPUT.
        if (kModes[i].level >= 0) digitalWrite(PIN_GPIO4_TEST, kModes[i].level);
        pinMode(PIN_GPIO4_TEST, kModes[i].pin_mode);
    }
    s_gpio4_mode = kModes[i].name;
    delay(GPIO4_SETTLE_MS);
    Serial.printf("[probe] GPIO%d %s -> level %u\n", PIN_GPIO4_TEST, s_gpio4_mode, power_probe_gpio4_level());
    return true;
}

const char* power_probe_gpio4_mode() { return s_gpio4_mode; }

uint8_t power_probe_gpio4_level() { return (uint8_t)digitalRead(PIN_GPIO4_TEST); }
#endif // TICKR_POWER_PROBE
