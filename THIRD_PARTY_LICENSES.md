# Third-party components

TickrDisplay is licensed under **GPL-3.0-or-later** (see [`LICENSE`](LICENSE)).
The compiled firmware statically links the components below. Their licences
are all compatible with distribution of the combined work under GPL-3.0.
Copyright notices and licence texts of each component are preserved in the
respective upstream repositories and in the PlatformIO `.pio/libdeps`
directory of a build; binary releases must ship this file and `LICENSE`.

Versions are the ones resolved by PlatformIO from the constraints in
`tickr_display/platformio.ini` when this file was last updated; verify
against `.pio/libdeps/*/` for an actual build.

## Libraries (pulled via `lib_deps`)

| Component | Version | Licence | Upstream | Notes / obligations |
|-----------|---------|---------|----------|---------------------|
| GxEPD2 | 1.6.9 | **GPL-3.0** | https://github.com/ZinggJM/GxEPD2 | Copyleft. Drives the combined-work licence to GPL-3.0; corresponding source must accompany binaries. |
| Adafruit GFX Library | 1.12.6 | BSD (2-clause) | https://github.com/adafruit/Adafruit-GFX-Library | Keep copyright notice. The bundled `Fonts/Free*` are converted from GNU FreeFont (GPL with font-embedding exception). |
| Adafruit BusIO | 1.17.4 | MIT | https://github.com/adafruit/Adafruit_BusIO | Keep copyright notice. |
| ArduinoJson | 6.21.6 | MIT | https://github.com/bblanchon/ArduinoJson | Keep copyright notice. |
| PubSubClient | 2.8 | MIT | https://github.com/knolleary/pubsubclient | Keep copyright notice. |
| ESPAsyncWebServer (ESP32Async) | 3.12.1 | **LGPL-3.0** | https://github.com/ESP32Async/ESPAsyncWebServer | Provide source / ability to relink → satisfied by publishing full source and build instructions. |
| AsyncTCP (ESP32Async) | 3.5.0 | **LGPL-3.0** | https://github.com/ESP32Async/AsyncTCP | Same as above. |

Not linked into the release image: **WiFiManager (tzapu, MIT)** was replaced by
the firmware's own set-up portal on top of ESPAsyncWebServer and the Arduino core's
`DNSServer`; **ArduinoOTA / ESPmDNS** (Arduino core, LGPL-2.1) are linked only into
the developer build `tickr_dev`.

## Own cryptographic code (no third-party origin)

| Component | Where | Notes |
|-----------|-------|-------|
| Browser-side X25519 (RFC 7748, BigInt Montgomery ladder), SHA-256 (FIPS 180-4), HMAC (RFC 2104), HKDF (RFC 5869) for the pairing wizard | `tickr_display/www/src/_crypto.js` (part of the shelf page served at `/`) | Written for this project (its header says so); **no TweetNaCl, noble or other third-party code was copied**, so no additional licence applies – it is part of the GPL-3.0 work. Unit tests check it against the RFC 7748 §5.2/§6.1, FIPS 180-4, RFC 4231 and RFC 5869 test vectors. Not constant-time (acceptable for an ephemeral key in the user's own browser). |
| Device-side HKDF, box construction, constant-time compare, check-word / code helpers | `tickr_display/src/logic/pairing.cpp` | Own code over mbedTLS `mbedtls_md_hmac` / `mbedtls_ecdh_*` (Apache-2.0, part of ESP-IDF, see below). The host tests use a small reference SHA-256 (`test/test_pairing/sha256_ref.h`, own code) where mbedTLS is absent. |

## Platform / framework

| Component | Version | Licence | Upstream | Notes |
|-----------|---------|---------|----------|-------|
| Arduino core for ESP32 (`framework-arduinoespressif32`) | 2.0.17 (via PlatformIO `espressif32 @ 7.1.3`) | LGPL-2.1 | https://github.com/espressif/arduino-esp32 | Includes `WiFi`, `DNSServer`, `HTTPClient`, `WiFiClientSecure`, `Update`, `LittleFS`, `SPI`, `ledc`, `dac` APIs used by this project (`ArduinoOTA`/`ESPmDNS` only in `tickr_dev`). |
| ESP-IDF (bundled in the Arduino core) | 4.4.x | Apache-2.0 | https://github.com/espressif/esp-idf | Includes FreeRTOS (MIT), lwIP (BSD), mbedTLS (Apache-2.0), NimBLE/Bluedroid (not used), esp_littlefs / littlefs (BSD-3). |
| PlatformIO Core (build tool) | any | Apache-2.0 | https://platformio.org | Build tool only, not linked. |
| xtensa-esp32-elf-gcc toolchain | as pinned by platform | GPL-3.0 (compiler), GCC runtime exception for libgcc/libstdc++ | https://github.com/espressif/crosstool-NG | Runtime library exception applies to the linked runtime. |

## Content

| Item | Source | Notes |
|------|--------|-------|
| RTTTL melody presets in the web UI (Mario, Star Wars, …) | Community ring-tone transcriptions | Demo content, user selectable; may be removed if desired (see `docs/LEGAL.md` §g). |
| Root CA certificates baked into the firmware for the ticker presets (`tickr_display/certs/`: DigiCert Global Root G2, GTS Root R4, GlobalSign Root CA, ISRG Root X1) | Public CA root certificates, taken from the operating system's root store; SHA-256 fingerprints in the file headers | Public keys and subjects only (ESP-IDF bundle format, 1 618 B); not copyrightable expression, distributed by every OS and browser vendor. No licence obligation; verify the fingerprints against the publishers' before changing a file (`SECURITY.md`). |

## Not included

TickrDisplay contains **no code, images, fonts or data from the TickrMeter
stock firmware**, and no vendor binaries or flash dumps are distributed. See
`docs/LEGAL.md` and `docs/HARDWARE.md` → *Stock firmware*.

## Updating this file

When bumping a dependency in `platformio.ini`, update the version column and
re-check the licence in the library's `library.json` / `LICENSE`. A quick way
to list what was actually resolved:

```bash
cd tickr_display && pio pkg list
for d in .pio/libdeps/*/*/; do echo "$d"; grep -E '^(name|version|license)=' "$d/library.properties"; done
```
