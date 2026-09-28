# Development guide

Everything build-related lives in [`tickr_display/`](../tickr_display/), a
[PlatformIO](https://platformio.org) project. All commands below are run from that directory.
Flashing and updating a device is covered in [`FLASHING.md`](FLASHING.md); the release
procedure in [`RELEASING.md`](RELEASING.md); known limitations in the [README](../README.md).

## Prerequisites

* Python 3.9+ and PlatformIO Core (`pip install platformio`) or the PlatformIO IDE extension.
* For the host unit tests: a C++ compiler (clang on macOS, gcc on Linux, MSYS2/MinGW on Windows).
* For UART flashing: a 3.3 V USB-UART adapter wired to the programming header
  ([`HARDWARE.md` → *Programming access*](HARDWARE.md#programming-access)).

The first build downloads the `espressif32 @ 7.1.3` platform, the Arduino-ESP32 2.0.17 framework
and the Xtensa toolchain (several hundred MB); later builds are incremental.

## Building

```bash
cd tickr_display
pio run                     # default env: tickr (stock 4 MB partition table)
pio run -e tickr_dev        # tickr + developer extras
pio run -e tickr -e tickr_dev
```

Outputs: `.pio/build/<env>/firmware.bin` (raw image, used by `pio run -t upload`) and a copy named
for distribution in the git-ignored `dist/`: `tickrdisplay-<version>.bin` (`tickr`),
`tickrdisplay-<version>-dev.bin` (`tickr_dev`).

### Environments

| Env | Partition table | Flash size | Purpose |
|-----|-----------------|------------|---------|
| `tickr` | `default.csv` | 4 MB | **Default and the only image to distribute.** Same layout as the stock TickrMeter firmware: `app0`/`app1` = 1 310 720 bytes each, 1.375 MB LittleFS. Safe on every device. |
| `tickr_dev` | `default.csv` | 4 MB | `tickr` plus every opt-in flag in the table below (`TICKR_ARDUINO_OTA`, `TICKR_MAX_PEERS=48`, `TICKR_PAIR_DEBUG_SCREEN`, `TICKR_RECOVERY_TEST`, `TICKR_DEV_PAGE`, `TICKR_LOG_VERBOSE`). OTA-safe (same table as `tickr`), but not a release artifact and not covered by the size gate; CI builds it only to keep the code compiling. |
| `native` | – | – | Host build of the pure-logic modules (`src/logic/*`) for the unit tests, `pio test -e native`. |

The firmware environments extend the `[esp32]` section of `platformio.ini`; only the build flags
and the `dist/` suffix differ.

### Build flags

Always on (`[esp32] build_flags`): `-DCORE_DEBUG_LEVEL=0` (Arduino core log level; raise to 3–5
while debugging), `-DARDUINOJSON_USE_DOUBLE=0`, `-DARDUINOJSON_USE_LONG_LONG=0`. The native env
defines `-DNATIVE_BUILD` for `#ifndef NATIVE_BUILD` guards around Arduino-only code.

Opt-in flags (off in `tickr`):

| Flag | `tickr_dev` | Effect |
|------|-------------|--------|
| `TICKR_ARDUINO_OTA` | on | Compiles `src/managers/arduino_ota.cpp` and links `ArduinoOTA` + `ESPmDNS` + IDF `libmdns` (≈ 31 KB flash, a mDNS task): LAN push-OTA on TCP 3232, mDNS `tickrdisplay-xxxxxx.local`. Without it every `ota_arduino_*()` call is an inline no-op, `/api/system/info` reports `arduino_ota.built = false` and `/system` hides the *OTA password* field. |
| `TICKR_MAX_PEERS=N` | 48 (default 16) | Size of the multi-device peer table (`src/logic/peer_table.h`, 40 B per entry, persisted in `/peers.bin`). `tickr_dev` builds the upper bound so CI exercises the memory and the streamed `GET /api/peers`. |
| `TICKR_PAIR_DEBUG_SCREEN` | on | Keeps `GET /api/screen/raw` and `/api/screen.bmp` readable while a pairing code is on the e-ink (release builds answer `503` so the code cannot be read over the network). Test automation only; never ship a build with it on. |
| `TICKR_RECOVERY_TEST` | on | Makes recovery mode drivable over the LAN: a software restart counts as a power cycle, `POST /api/recovery/enter` (token) enters it directly, the timeouts shrink to 2 / 4 min, and the boot frame is held so `/api/screen.bmp` can fetch *Restart 2 of 3*. None of this exists in the release image (`POST /api/recovery/enter` → `404`). |
| `TICKR_DEV_PAGE` | on | Serves the `/dev` Diagnostics & tests page (`www/src/dev.html`, ≈ 4.3 KB gz in PROGMEM) and reports `dev_page: true` in `GET /api/system/info`; `/system` → *Advanced* shows the link only then. The release image answers `404`. `build_www.py` emits the assets listed in `DEV_ONLY` inside `#ifdef TICKR_DEV_PAGE`, so one generated header serves both envs. |
| `TICKR_LOG_VERBOSE` | on | Compiles the informational serial lines – `LOGV(...)` / `LOGVLN(s)` in `src/log.h`: web server start, MQTT connect, pull progress, OTA progress, peer table load, pairing timing, payload dumps, display init. Without it each macro is a `do { if (0) … } while (0)` and the format strings are dropped. The release keeps error lines and one line per state transition. |
| `TICKR_POWER_PROBE` | **off in all envs** | Power-sensing diagnostics only: compiles `src/hal/hal_power_probe.cpp` (boot-time ADC2 scan and GPIO pull probe, `GET /api/power/probe`, `/api/power/probe/gpio4`, extra fields in `/api/power/raw`; ≈ 4.5 KB). Add `-DTICKR_POWER_PROBE=1` to `tickr_dev` when investigating a board revision – see [`HARDWARE.md` → *Power sensing*](HARDWARE.md#power-sensing). |

### Why the 4 MB table

OTA through the stock `/update` page writes into the existing `app0`/`app1` slots of the stock
`default.csv` table and never touches the table itself, so the image must fit into **1 310 720
bytes** and be built against that layout. The flash chip is 8 MB (on the units examined; check
yours with `esptool.py flash_id`), but the stock bootloader is configured for 4 MB, so the upper
half is unusable until a new bootloader and partition table are written over UART. The firmware
resolves every partition by label at runtime and never hard-codes offsets, so the same
`firmware.bin` runs on either table: tinkerers can set `board_build.partitions = default_8MB.csv`
and `board_upload.flash_size = 8MB` in a local `platformio.ini` and flash bootloader + table + app
over UART (partition backups and `write_flash` addresses are then only valid for that table).

### Size gate

`scripts/check_size.py` runs automatically after every firmware build and

* **fails** the build if `firmware.bin` is larger than **1 310 720 bytes** (the stock OTA slot), and
* **warns** when the image exceeds **95 %** of it.

CI runs it standalone on the release image (`dist/*.bin` minus `-dev.bin`):
`python scripts/check_size.py dist/*.bin` (`--limit BYTES`, `--warn-percent N`).

### Web UI (`www/src/`)

The HTML pages live as real files in `tickr_display/www/src/` and are compiled into the image by
`scripts/build_www.py`, a PlatformIO pre-script that runs on every `pio run` (and `pio check`):

```
www/src/panel.html   -> "/"        the shelf: cards, device sheet, Identify, Protect-this-device,
                                   Group wizard, Updates (from /api/screen/raw, /api/identity, /api/peers)
www/src/system.html  -> "/system"  settings + firmware: one form -> POST /config; upload -> POST /update
www/src/dev.html     -> "/dev"     diagnostics & tests: /api/test/*, partitions -- tickr_dev only
www/src/wifi.html    -> "/wifi"    set-up portal (served as "/" in AP mode), Recovery section
www/src/_*.{html,js,css}           partials: head/nav, token handling (_auth.js, _tokbar.html),
                                   pairing crypto (_crypto.js), palette (_common.css), theme (_theme.js)
```

* `{{include:<file>}}` splices a partial (files starting with `_` are never emitted as assets).
  Leading indentation, blank lines and whole-line `//` / `/* … */` comments are stripped (never
  start a line *inside a string literal* with `//` or `/*`); the result is gzip'ed (level 9,
  deterministic) and written to `src/web/generated/www_assets.h` (git-ignored build product) as
  `PROGMEM` arrays plus a table `WWW_ASSETS[] = {name, mime, data, len, etag}`.
* `src/web/www.cpp` serves the table: `www_send(request, "panel.html")` sends the bytes from flash
  with `Content-Encoding: gzip`, an `ETag` (CRC32 of the body) and `Cache-Control: no-cache`;
  `If-None-Match` gets `304`. No uncompressed fallback (`curl` needs `--compressed`).
* **No server-side templating.** Pages fetch what they need from the JSON API on load and send
  `localStorage.tickr_token` as `X-Api-Token`; without a token they open read-only with the inline
  token bar. A `401` never triggers the browser's Basic prompt because the challenge is sent only
  for navigations (`src/logic/auth_policy.h`).
* **Web size gate.** `build_www.py` prints raw/gzip sizes per page and fails the build when the
  gzipped total of the **release** assets (everything outside `DEV_ONLY`) exceeds **40 KB**
  (`WWW_GZ_LIMIT`); the dev total including `dev.html` is printed alongside. Standalone:
  `python scripts/build_www.py`.

To add a page: create `www/src/<name>.html`, register a route that calls
`www_send(request, "<name>.html")`; add its name to `DEV_ONLY` in `build_www.py` and guard the
route with `#ifdef TICKR_DEV_PAGE` if it is developer material.

### TLS root bundle (`certs/`)

The ticker presets verify their HTTPS connections against roots baked into the firmware
([`TICKERS.md`](TICKERS.md), [`SECURITY.md`](../SECURITY.md)). `scripts/build_ca_bundle.py` (a
pre-script like `build_www.py`) compiles every `certs/*.pem` (ISRG Root X1, DigiCert Global Root
G2, GlobalSign Root CA, GTS Root R4) into `src/generated/ca_bundle.h` (git-ignored) in the ESP-IDF
bundle format: per root a 4 B header + DER subject + DER SubjectPublicKeyInfo, sorted by subject
because the IDF verifier binary-searches. Standalone: `python scripts/build_ca_bundle.py`
(`--check` prints sizes only); no third-party Python module needed. **Adding a root** = drop a PEM
with a provenance header (name, subject, SHA-256 fingerprint, expiry, origin) into `certs/` and
rebuild; **removing** one = delete the file. The bundle is attached with `setCACertBundle()` to
connections without a user CA; `setCACert()` (the user's `/ca.pem`) and the bundle do not coexist
on one connection in Arduino-ESP32 2.0.17, so the firmware picks one per source kind. There is no
expiry check on the device (mbedTLS without `MBEDTLS_HAVE_TIME_DATE`).

### Warnings and static analysis

* Informational `Serial` lines go through `LOGV` / `LOGVLN` (`src/log.h`) and exist only with
  `TICKR_LOG_VERBOSE`; errors and state transitions use `Serial.print*` directly. New progress /
  timing / dump lines belong behind `LOGV`.
* `-Wall -Wextra` (plus `-Wunused-parameter -Wsign-compare`, which the Arduino-ESP32 framework
  disables globally) are applied to **`src/` only** by `scripts/src_warnings.py`, which also turns
  the framework/library include paths into `-isystem`. Keep `src/` warning-free. (`GxEPD2_EPD.h`
  globally ignores `-Wunused-parameter`, so that warning is silenced wherever `hal/hal_display.h`
  is included.)
* `pio check -e tickr --skip-packages --fail-on-defect=high` runs cppcheck on `src/`
  (`warning,style,performance,portability`); defects inside `.pio/libdeps` are suppressed.

## Flashing and serial monitor

UART: wire the adapter to the programming header, enter the bootloader (hold IO0 to GND, tap RST,
release IO0), then `pio run -e tickr -t upload` (add `--upload-port /dev/ttyUSB0` if needed;
`upload_speed` is 921600 – drop to 460800 if the adapter is flaky). OTA: `dist/tickrdisplay-<version>.bin`
through the stock `/update` page or TickrDisplay's `/system#firmware` / `scripts/flash_ota.sh`; with
a `tickr_dev` image, an *OTA password* and USB power, ArduinoOTA works too
(`pio run -e tickr_dev -t upload --upload-port <device-ip> --upload-flag --auth=<ota_password>`).
Details: [`FLASHING.md`](FLASHING.md).

`pio device monitor` (115200 baud) has `monitor_filters = esp32_exception_decoder, time`
preconfigured: a crash backtrace is decoded into `file:line` using the ELF of the last build
(`.pio/build/<env>/firmware.elf`) and every line is timestamped – rebuild before monitoring so the
ELF matches the running image.

## Unit tests

```bash
pio test -e native                  # all tests
pio test -e native -f test_sanity   # one test directory
pio test -e native -vv              # show compiler output / Unity details
```

The `native` environment compiles the pure-logic sources under `src/logic/` (minus `renderer.cpp`,
which drives the display HAL, and `command_queue.cpp`, which wraps a FreeRTOS queue) together with
`test/test_*/test_main.cpp`, using Unity, `-std=c++17`, `-Wall -Wextra` and AddressSanitizer/UBSan;
ArduinoJson is available on the host. The firmware environments have `test_ignore = *`; nothing
runs on the device. One directory per module:

* `test_sanity` – the runner works; how ArduinoJson treats the documented payload.
* `test_payload` – the screen-payload validator (fields, limits, error reasons).
* `test_rtttl` – the RTTTL melody parser.
* `test_battery` – battery/power helpers: detector rule, thresholds, percentage.
* `test_device_state` – the screen state machine and the runtime power-mode switch.
* `test_status_policy` – badge refresh policy and the screen-state names.
* `test_refresh_policy` – the e-ink refresh decision (none / defer / partial / full), forced-full
  counters, 30 s spacing, identical-frame skip ([`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules*).
* `test_screen_bmp` – the 1-bit BMP view of the shadow framebuffer.
* `test_ticker` – ticker helpers: age line, sparkline scaling, LED rule, direction from the change string.
* `test_spark_hist` – the on-device sparkline ring (re-quantisation, key change, RTC garbage).
* `test_source` – ticker source: mini-JSONPath extraction from the presets' response shapes
  (`fixtures.h`), preset resolution, number formatting, config schema migration, malformed bodies.
* `test_pull_scheduler` – USB-mode pull timing: interval + jitter, back-off on failures, fetch-now.
* `test_auth_policy` – the `401` challenge policy (Basic challenge only for navigations).
* `test_recovery_counter` – the power-cycle recovery counter on a fake NVS.
* `test_peer_table` – the peer table, the `/peers.bin` blob and the beacon wire format.
* `test_layout` – the shelf layout document validator.
* `test_pairing` – pairing primitives and the beacon signature, with a reference SHA-256 (mbedTLS
  is not available on the host); its fixed vectors are the ones `www/src/_crypto.js` is checked against.
* `test_relay` – relay request signature and header, single-use nonce store, route ↔ file mapping,
  the sleeper's wake-up flow; `X-Tickr-Group` vector cross-checked with Python's `hmac`.

### Adding a test for a pure module

1. Put the module under `src/logic/` (`rtttl.h` + `rtttl.cpp`). It must not include `Arduino.h` or
   anything from `src/hal/` or `src/managers/`; take plain C types/callbacks as parameters instead
   (a "play tone" callback rather than calling `dacWrite`). Device-only Arduino types go behind
   `#ifndef NATIVE_BUILD`.
2. Create `test/test_<module>/test_main.cpp`:

   ```cpp
   #include <unity.h>
   #include "logic/rtttl.h"
   void setUp() {}
   void tearDown() {}
   static void test_parses_default_section() { /* TEST_ASSERT_EQUAL_INT(...) */ }
   int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_parses_default_section); return UNITY_END(); }
   ```

3. Run `pio test -e native`. New files under `src/logic/` are picked up by the `build_src_filter`
   automatically; a module that *must* be excluded gets a `-<logic/that_file.cpp>` line in the filter.

## Versioning and releases

The firmware version is derived at build time by `scripts/version.py`: the `FIRMWARE_VERSION`
environment variable if set, otherwise `git describe --tags --always --dirty --match "v*"` – e.g.
`v0.3.1`, `v0.3.1-4-g1a2b3c4` (4 commits after the tag), `v0.3.1-4-g1a2b3c4-dirty` (uncommitted
changes); without a reachable `v*` tag it is `0.0.0-g<sha>`. The string is compiled in as
`FIRMWARE_VERSION`, reported in `/api/system/info`, `/api/identity` and the beacon, and names the
`dist/` image.

Releases are annotated tags `vMAJOR.MINOR.PATCH` on `main` (bump MAJOR when the JSON payload or MQTT
topic contract changes incompatibly, MINOR for features, PATCH for fixes). Pushing the tag makes CI
build the images with that version, run the tests and the size gate, and publish
`tickrdisplay-vX.Y.Z.bin` as the release asset. The step-by-step
checklist is [`RELEASING.md`](RELEASING.md).

## Continuous integration

Both pipelines run the same four jobs with `platformio==6.2.0` on Python 3.12, cache
`~/.platformio` keyed on `platformio.ini` and clone the full history so `git describe` sees the tags:
**firmware** builds `tickr` and `tickr_dev`, runs `check_size.py` on the release image and keeps
it as an artifact for 30 days (the `-dev` image excluded); **unit-tests** runs
`pio test -e native`; **static-analysis / cppcheck** runs the `pio check` command above; **release**
(on `v*` tags, after firmware + tests) publishes the `.bin` file.

* **GitHub Actions** (`.github/workflows/build.yml`): on pushes to `main`, `v*` tags, pull requests
  and manual dispatch; the release job creates a GitHub Release with generated release notes.
* **GitLab CI** (`.gitlab-ci.yml`): mirrors it; the release job (release-cli) links to the
  `firmware` job artifacts. `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer needs ptrace.

## Dependency pinning

`platformio.ini` pins the platform and every library to an exact version so that a checkout builds
the same image later. To upgrade: bump the version in `platformio.ini`, `pio pkg update` (or delete
`.pio/libdeps`), rebuild all envs, check the size report and `pio test -e native`, then commit.
`espressif32 @ 7.1.3` ships Arduino-ESP32 **2.0.17**; the sources use the 2.x LEDC API
(`ledcSetup`/`ledcAttachPin`), so a platform with Arduino-ESP32 3.x requires porting.
`ESP32Async/ESPAsyncWebServer` + `ESP32Async/AsyncTCP` support Arduino core 2.x and 3.x.

## Repository layout (build-related)

```
.github/workflows/build.yml   GitHub Actions: build, test, cppcheck, release
.gitlab-ci.yml                GitLab CI equivalent
scripts/flash_ota.sh          OTA upload to a device (TickrDisplay or the stock portal)
scripts/backup_device.sh      dump every partition of a device over HTTP
tickr_display/
  platformio.ini              environments, pinned dependencies, flags
  scripts/version.py          FIRMWARE_VERSION define + dist/ copy (pre-script)
  scripts/build_www.py        www/src/ -> gzip PROGMEM header, 40 KB web gate (pre-script and CLI)
  scripts/build_ca_bundle.py  certs/*.pem -> IDF root bundle header (pre-script and CLI)
  scripts/src_warnings.py     -Wall -Wextra for src/ only, -isystem for libs
  scripts/check_size.py       OTA image size gate (post-script and CLI)
  certs/                      root CAs of the ticker presets (PEM with provenance headers)
  www/src/                    web UI sources (HTML/CSS/JS, partials start with _)
  src/                        firmware sources (hal/, managers/, logic/, web/)
  src/log.h                   LOGV / LOGVLN – informational serial lines behind TICKR_LOG_VERBOSE
  src/web/generated/          build product of build_www.py (git-ignored)
  src/generated/              build product of build_ca_bundle.py (git-ignored)
  test/                       host unit tests (test_<module>/test_main.cpp)
  dist/                       versioned images (git-ignored)
```
