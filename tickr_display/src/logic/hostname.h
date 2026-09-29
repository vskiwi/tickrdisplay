#pragma once
// Network hostname of the device (docs/DEVICE_UI.md "Wi-Fi link supervision"):
// "<device-name>-XXXXXX", the user's device name made safe
// for DHCP option 12 / mDNS (RFC 1123 label) plus the last three bytes of
// the STA MAC address, the same suffix the default "esp32-XXXXXX" carries.
// Pure logic, unit-tested on the host (test/test_hostname); the Wi-Fi
// manager applies it before the driver starts.
//
// Sanitising: letters, digits and '-' are kept, space and '_' become '-',
// everything else (Cyrillic, punctuation) is dropped, runs of '-' collapse,
// '-' at either end is removed, the result is lower-case. An empty result
// falls back to HOSTNAME_FALLBACK. The whole name is cut to HOSTNAME_MAX_LEN
// (the ESP-IDF limit) so that the suffix always fits.
#include <stdint.h>
#include <stddef.h>

#define HOSTNAME_MAX_LEN    32          // characters, excluding the NUL (ESP-IDF limit)
#define HOSTNAME_SUFFIX_LEN 7           // "-XXXXXX"
#define HOSTNAME_FALLBACK   "tickr"

// The sanitised label alone (no suffix), at most `max_len` characters,
// never ending in '-'. Returns its length.
size_t hostname_sanitize(const char* name, char* out, size_t out_len, size_t max_len);

// "<label>-XXXXXX" from the device name and the STA MAC; `out_len` must be
// >= HOSTNAME_MAX_LEN + 1. Returns the length.
size_t hostname_build(const char* name, const uint8_t mac[6], char* out, size_t out_len);
