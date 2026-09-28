#pragma once
//
// LAN push-OTA (ArduinoOTA, TCP port 3232 + mDNS _arduino._tcp) so that
//   pio run -t upload --upload-port <ip> --upload-flag --auth=<password>
//   espota.py -i <ip> -f firmware.bin -a <password>
// work without putting the device into AP mode.
//
// OPT-IN AT BUILD TIME: compiled only with -DTICKR_ARDUINO_OTA (the `tickr_dev`
// environment). The release `tickr` image leaves it out: ArduinoOTA drags in
// ESPmDNS + the IDF mDNS component (~31 KB of flash, a 4 KB-stack task and
// ~2 KB of static RAM) for a feature the supported update paths (/update,
// scripts/flash_ota.sh) do not need. Without the flag every function below is
// an inline no-op, so callers need no #ifdefs.
//
// Security / power policy (enforced here, not by the caller):
//   * never started without a password  -> `password` empty => disabled, warning in the log
//   * only started when USB powered      -> on battery the device deep-sleeps anyway
//   * only started when WiFi is in STA mode with an IP
//
#include <Arduino.h>

#ifdef TICKR_ARDUINO_OTA

// Start ArduinoOTA. `password` is either a plain-text password or its 32-hex-char MD5
// (detected automatically -> setPasswordHash). Returns true if the service was started.
bool ota_arduino_begin(const char* password, bool usb_powered);

// Call from loop(). Cheap no-op when not started.
void ota_arduino_handle();

bool ota_arduino_enabled();
const char* ota_arduino_hostname();
uint16_t ota_arduino_port();
// True while an ArduinoOTA transfer is being written to flash.
bool ota_arduino_in_progress();
// True in builds that include ArduinoOTA at all.
inline bool ota_arduino_built() { return true; }

#else

inline bool ota_arduino_begin(const char*, bool) { return false; }
inline void ota_arduino_handle() {}
inline bool ota_arduino_enabled() { return false; }
inline const char* ota_arduino_hostname() { return ""; }
inline uint16_t ota_arduino_port() { return 3232; }
inline bool ota_arduino_in_progress() { return false; }
inline bool ota_arduino_built() { return false; }

#endif
