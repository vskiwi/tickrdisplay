#pragma once
// Power sensing API (docs/HARDWARE.md "Power sensing"). Token-protected.
//   GET  /api/power/raw     raw ADC counts + calibrated millivolts of the two
//                           sensing channels (cell / rail) and of GPIO 34..39,
//                           the rail-cell difference the decision is made from,
//                           the sense-enable level, both dividers, the
//                           calibration kind, the detector state and the
//                           configured override. Read-only, ~70 conversions.
//   POST /api/power/source  form `mode=auto|usb|battery` - the override
//                           (AppConfig::power_source); applied at once and
//                           saved. 200 {"ok":true,"mode":"..."} / 400.
//   GET  /api/power/probe   tickr_dev only (-DTICKR_POWER_PROBE): digital
//                           pull probe of every free GPIO at boot and now,
//                           with the excluded pins and why (hal_power_probe.h).
//                           /api/power/raw then also carries `boot_power_hint`,
//                           `adc2_boot` (ADC2 read before Wi-Fi) and `gpio4_mode`.
//   POST /api/power/probe/gpio4?mode=high|low|pulldown|pullup|float|restore
//                           tickr_dev only: GPIO 4 enable test (docs/HARDWARE.md
//                           "Power sensing") - set GPIO 4, wait 150 ms, re-read GPIO 32/33
//                           and 34-39; `restore` = back to the firmware's HIGH.
//                           GET returns the current mode/level.
#include <ESPAsyncWebServer.h>
#include "config_manager.h"

void power_api_register_routes(AsyncWebServer& server, AppConfig* cfg);
