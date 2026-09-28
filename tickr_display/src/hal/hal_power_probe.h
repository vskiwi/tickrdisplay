#pragma once
// Developer-only power-sensing probe: -DTICKR_POWER_PROBE (env tickr_dev).
// Purpose: find where the second board revision (rev B, docs/HARDWARE.md
// "Power sensing") senses USB/charging, since none of its ADC1 channels
// reacts to the power source. Two instruments:
//
//  * Boot-time ADC2 scan. ADC2 is unusable once Wi-Fi is up, so the
//    channels that are not wired to a peripheral (GPIO 0, 2, 4, 12) are read
//    ONCE at the very start of setup() - before any HAL / Wi-Fi init - with
//    11 dB attenuation and eFuse calibration, and the result is kept in
//    static storage for GET /api/power/raw (`adc2_boot`, `boot_power_hint`).
//  * Digital pull probe. Every GPIO that is not occupied by a peripheral,
//    the flash (6-11) or UART0 is read as INPUT (floating), INPUT_PULLUP
//    and INPUT_PULLDOWN (input-only 34-39: level only). A pin that keeps
//    its level regardless of the pull is tied externally (charger status,
//    VBUS divider, ...); a pin that follows the pull is free. Run once at
//    boot and again on demand through GET /api/power/probe. Every pin is put
//    back to its previous IO_MUX state afterwards.
//  * GPIO 4 enable test (docs/HARDWARE.md "Power sensing"). The stock
//    firmware wrote GPIO 4 high - without ever enabling the output, so the
//    pad stayed at its reset default (input, weak pull-down) - and waited
//    150 ms before reading the battery ADC on GPIO 32. The test answered
//    it: GPIO 4 is the sense-network enable on rev B, and the
//    firmware now drives it high while awake (PIN_BAT_SENSE_EN).
//    The endpoint stays for re-checks: POST /api/power/probe/gpio4 drives
//    the pad, GPIO 32/33 are re-read with the usual power_read_raw();
//    `restore` returns it to the firmware's HIGH.
//
// Nothing here exists in the release image.
#ifdef TICKR_POWER_PROBE
#include <stdint.h>
#include <stddef.h>
#include "../logic/battery.h"   // POWER_SENSE_SETTLE_MS

enum PowerProbePinKind : uint8_t {
    PROBE_PIN_FREE = 0,     // full probe at boot and at runtime
    PROBE_PIN_INPUT_ONLY,   // GPIO 34-39: level only (no pulls on these pads)
    PROBE_PIN_BOOT_ONLY,    // free before HAL init, taken afterwards (GPIO 4 = sense enable, GPIO 16 = SPI dummy MISO)
    PROBE_PIN_EXCLUDED,     // peripheral / flash / UART0 - never touched
};

struct PowerProbePin {
    uint8_t     gpio;
    int8_t      adc2_ch;    // ADC2 channel or -1
    uint8_t     kind;       // PowerProbePinKind
    const char* note;       // exclusion reason or a hint (strapping pin, ...)
};

struct PowerProbeAdc2 {
    uint8_t  gpio;
    int8_t   ch;
    bool     ok;            // adc2_get_raw() succeeded (false = ADC2 busy / error)
    uint16_t raw, raw_min, raw_max;
    uint32_t mv;            // eFuse-calibrated pin millivolts (mean)
};

// Level values: 0 / 1, PROBE_NA when the mode does not apply.
#define PROBE_NA 2
struct PowerProbeGpio {
    uint8_t gpio;
    uint8_t lvl_float;
    uint8_t lvl_pu;
    uint8_t lvl_pd;
    bool    probed;         // false = skipped at this stage (see PowerProbePin::note)
};

// The pin table (GPIO 0..39 that exist on the ESP32), in ascending order.
size_t               power_probe_pin_count();
const PowerProbePin* power_probe_pin(size_t i);

// Run FIRST in setup(): reads GPIO 33/32 as the detector would (sense
// enable still low), scans the free ADC2 channels and probes the free
// GPIOs. Logs to Serial. Leaves every pin as it found it.
void power_probe_boot();

// Boot-time results.
const char*           power_probe_boot_hint();       // "usb" | "battery" | "invalid" (detector before power_init(); rev B: invalid)
uint16_t              power_probe_boot_vin_raw();
uint32_t              power_probe_boot_vin_pin_mv();
size_t                power_probe_adc2_count();      // scanned channels only
const PowerProbeAdc2* power_probe_adc2(size_t i);
size_t                power_probe_gpio_count();      // = power_probe_pin_count(); entries for excluded pins have probed = false
const PowerProbeGpio* power_probe_gpio_boot(size_t i);

// Runtime probe of the same pins (PROBE_PIN_BOOT_ONLY pins are skipped).
// `out` must hold power_probe_gpio_count() entries. Blocks ~ 3 x settle per pin.
void     power_probe_gpio_now(PowerProbeGpio* out);
uint16_t power_probe_settle_ms();

// "bound_high" | "bound_low" | "free" | "level_only" | "odd" | "skipped"
const char* power_probe_verdict(const PowerProbeGpio& g, const PowerProbePin& p);

// GPIO 4 enable test. Modes:
//   high | low          output, driven
//   pulldown | pullup   input with the internal pull (pulldown = the stock's
//                       effective state = the pad's reset default)
//   float               plain input, no pull
//   restore             the firmware's state: output HIGH (power_sense_enable)
// Applies the mode, then blocks GPIO4_SETTLE_MS (the stock's delay(150)).
// Returns false for an unknown mode string (pin untouched). The mode stays
// until the next call - the bench sequence is high, low, pulldown, restore.
// While the pad is not high, rev B's detector reads INVALID (power: unknown).
#define GPIO4_SETTLE_MS POWER_SENSE_SETTLE_MS
bool        power_probe_gpio4_set(const char* mode);
const char* power_probe_gpio4_mode();    // last applied mode, "untouched" before the first call
uint8_t     power_probe_gpio4_level();   // digitalRead(4) right now
#endif // TICKR_POWER_PROBE
