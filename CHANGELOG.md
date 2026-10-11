# Changelog

All notable changes to TickrDisplay are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and this project adheres to [Semantic Versioning](https://semver.org/).
Until `1.0.0` minor versions may contain breaking changes.

## [Unreleased]

### Fixed

- **Power**: a battery-powered device rebooted on every wake-up (a reboot
  loop, no content) when its ticker source was the CoinGecko or Kraken
  preset or the 2×2 grid – the main task ran out of stack during the TLS fetch that the
  battery flow performs at boot. The main task now has a larger stack and the
  boot-time configuration copy no longer sits on it. USB-powered devices and
  battery devices on a Binance single ticker were not affected.
- **Tickers**: the TLS handshake of a fetch is bounded by the same 10-second
  timeout as the rest of the request (the core's default was 120 s); a server
  that goes silent after the TCP connect no longer holds the main task long
  enough for the task watchdog to reboot the device.

- **Power**: the battery percentage of a rev B unit read 75–85 % on a full
  cell – its cell channel divides differently from rev A's. The cell
  divider is now chosen per board revision (rev A 189/100, rev B 200/100);
  a configured `adc_cell_num/den` still overrides it, and the old explicit
  default in a saved configuration is read as "auto". The USB / battery
  decision is unaffected (it never used the cell divider).
- **Power**: on battery the cell is read once at boot, before Wi-Fi draws
  current, and that reading is the level of the whole wake-up; the level
  shown last is kept across deep sleep so the 5 % display step no longer
  flips between wake-ups on a reading that drifted by less.

### Added

- **LED**: a *LED on battery* setting (`led_battery`: `off` · `hold`,
  default `off`). With `hold` the LED's colour stays lit through the deep
  sleep – each channel fully on or off – while the cell is at 30 % or
  more, and comes back at the wake-up without a blink; below 30 % the bar
  is dark until the cell is charged. A lit LED shortens battery life
  several times ([`docs/DEVICE_UI.md`](docs/DEVICE_UI.md) → *LED and
  sound*).
- **Power**: a **settings window** for battery devices – switched off and
  on with the power switch while on battery, a device with a source stays
  awake for 3 minutes with *On battery* and its `http://<ip>/` on the
  panel, so the ticker or the interval can be changed from the browser
  without USB; then it restarts into the battery flow. Only after a
  power-on (not after an update, a wake-up or a crash)
  ([`docs/DEVICE_UI.md`](docs/DEVICE_UI.md) → *Power-mode switch*).
- **Web UI**: *LED on battery* in the Content editor next to the LED
  rule; the interval hint on a battery device names the cost of a
  1-minute interval.
- **API**: `GET /api/status` reports `boot_count`, `crash_count`,
  `crash_last_reason` (kept across every reset but a power-on),
  `loop_stack_min_free` and `cell_divider` (the divider in effect);
  `led_battery` in `GET /api/config` / `POST /config`; `adc_cell_num/den`
  accept `0`/`0` = auto ([`docs/API.md`](docs/API.md)).

### Changed

- **LED**: on battery the end-of-wake green blink after a successful fetch
  is skipped when the `sign` rule has coloured the LED – a red "down"
  colour is no longer overwritten by a green "fetched" blink; the red blink
  after a miss stays. The battery percentage reads 100 % from 4.15 V – a
  charger stops at 4.20 V and the cell relaxes below it.

## [0.3.0] - 2026-10-10

### Added

- **Display**: the ticker's short name on a black badge top left of the
  panel (`BTC`, `XBT`, `ETH` – up to 7 characters, 18 pt white on black,
  smaller when longer), with the change beside it and the price below. The
  name is derived from the symbol when the new *Short name* setting is
  empty: exchange symbols upper-cased, a pair typed as one word loses its
  quote currency, the common CoinGecko ids map to their tickers. A payload
  without a short name draws the frame as before. A changed badge is a full
  refresh.
- **Display**: a **2×2 grid** of up to four ticker sources on one panel –
  each cell with its badge, the change with its triangle and the price
  (whole at 18 pt, else without its fraction, else smaller); the age line
  and the status badges live in the last cell; two or three tickers use the
  same cells. Any mix of presets. All sources are fetched in one cycle and
  drawn in one frame; a source that fails keeps its last price with `?` in
  place of the change, also across deep sleep; a changed set or order of
  tickers is a full refresh. On battery the interval has a 15-minute floor
  while a grid is configured.
- **Web UI**: the Ticker editor gets a *View* choice (one ticker / 2×2
  grid) with up to four rows – preset, symbol, market, short name, reorder /
  remove, *Test* per row – and the *Advanced* settings per row, among them
  the new *Short name on the panel* field; *Test* shows the derived short
  name as the field's placeholder.
- **API**: `tk_short`, `tk_view`, `tk_n` and the per-row fields `tk1_*` …
  `tk3_*` in `GET /api/config` and `POST /config`; `POST /api/source/test`
  returns `short`; the payload field `short` for proxies;
  `GET /api/screen/state` adds `short` and, for a grid, `layout: "grid"`
  with `symbols[]` and `shorts[]`. Configurations saved by earlier releases
  load unchanged as a single ticker.

### Changed

- **Display**: a price with an integer part of 1 000 or more drops its
  fraction (`84 014.90` → `84 015`, rounded half-up) when that keeps the
  largest price size on the panel instead of stepping down to the next one;
  a price that fits whole keeps its decimals, and the *Decimals* /
  *Separator* settings are unchanged.
- **Build**: smaller firmware image – the `printf` family comes from the ESP32
  ROM (no floating-point or 64-bit integer formats in the image; floats are
  formatted by `src/logic/fmt_float`, a build step rejects `%f`-style formats
  under `src/`), core dumps to flash are disabled (a panic still prints its
  backtrace and reboots), and the web pages are packed with zopfli when the
  module is installed.

### Fixed

- **Web UI**: the *Firmware* line of a device's status card showed the
  version with a doubled prefix (`vv0.2.0`); the version string is now shown
  as the firmware reports it.

## [0.2.0] - 2026-09-29

### Added

- **Wi-Fi**: a link supervisor on USB power – the device re-issues its own
  connect attempts when the link stays down (25 s, then 15 s → 60 s
  back-off), also when the Wi-Fi core has given up or the address was lost
  while associated, and restarts itself after 30 min without a link; idle
  while the set-up portal, the recovery access point or a firmware write is
  active, not used on the battery flow. Link diagnostics in
  `GET /api/status` (`wifi_disconnects`, `wifi_last_reason`, `wifi_down_s`,
  `wifi_reconnects`, `wifi_restarts`), kept in RTC memory across software
  restarts and deep sleep, and one serial line per loss, attempt and
  restart in the release build. DHCP hostname `<device-name>-XXXXXX` from
  the device name (RFC 1123 label, `tickr` when empty) and the MAC suffix,
  applied from the next boot after a rename; `/api/system/info` →
  `wifi.hostname`.
- **Tickers**: Binance futures presets *USDⓈ-M* (`fapi`) and *COIN-M*
  (`dapi`) – perpetual and quarterly contracts through the market field
  (`USDT`, `USDT_261225`, `USD_PERP`, `USD_261225`); on a perpetual a second
  request fetches the **funding rate**, drawn in place of the age line as
  `FR +0.0100%`; a missed funding request keeps the last text without
  failing the fetch. `POST /api/source/test` reports it as `funding`.
- **Tools**: `scripts/flash_ota.ps1`, the OTA upload script for Windows
  PowerShell (same arguments and behaviour as `flash_ota.sh`, no external
  tools).

### Changed

- **Documentation**: flashing and updating are described for the browser
  first (`/system` → *Firmware*: version, slot, MD5, upload, *Boot other
  partition*; how to tell two images apart); the shell scripts moved to an
  optional *Command-line tools* section of `docs/FLASHING.md`.

### Fixed

- **Wi-Fi**: a device that lost the router (reboot, firmware update, band
  steering) could stay on the *No Wi-Fi* card until a power cycle – the
  Wi-Fi core's auto-reconnect stops on some disconnect reasons and nothing
  retried.

## [0.1.0] - 2026-09-28

First public release. Everything below describes what the firmware does on
the day of the release; the development history before it was private.

### Added

- **Cloud-free firmware for the TickrMeter** (ESP32-WROOM-32E, 2.9" SSD1680
  e-ink, RGB LED, DAC speaker) with a web UI served by the device: the
  **shelf** at `/`, **System** at `/system`, an own Wi-Fi set-up portal
  (open access point `TickrDisplay`, captive DNS) and a light/dark theme.
- **Content sources**: `POST /api/screen`, **MQTT** (topic, credentials,
  retained online/offline status), a **Pull URL** fetched every refresh
  interval, and an on-device **Ticker** with CoinGecko, Kraken and Binance
  presets or any JSON API via *Custom JSON* – price, 24 h change with ▲/▼,
  relative age and a 48-point sparkline kept across deep sleep; *Test*
  runs one fetch before saving.
- **One JSON payload for every path**: `title`, `value`, `alert.led` (any
  RGB colour, 8-bit PWM per channel), `alert.sound` (`beep`, `double_beep`,
  `long_beep`, `none` or an RTTTL melody), `alert.volume`, plus the ticker
  fields `change`, `dir`, `age_s`, `time`, `spark[]`; validated completely
  before anything is drawn or played.
- **Power**: USB or battery mode detected automatically on both known board
  revisions; battery mode deep-sleeps, wakes every N minutes, fetches,
  redraws once and sleeps, with exponential back-off and low-battery
  protection (badge, *Battery empty* card, latch); a USB device that loses
  its cable shows *On battery* and restarts into battery mode.
- **Multi-device**: LAN discovery over UDP beacons, live e-ink previews on
  the shelf, drag-and-drop arrangement stored on the devices, *Identify*
  numbers on the panels, a device sheet per card and a phone layout.
  **Groups** joined by Bluetooth-style pairing (6-digit code + check word,
  X25519 key exchange), *For all…* content, a **relay** on a USB member that
  parks content and firmware URLs for sleeping battery members, and a
  group firmware update from the *Updates* card.
- **Security**: one shared API token (`X-Api-Token` or HTTP Basic,
  constant-time compare) for every mutating, uploading or secret-revealing
  endpoint; ticker HTTPS pinned to your uploaded root CA or to the roots
  baked into the firmware, never an unchecked connection; signed
  device-to-device requests with
  single-use nonces; **recovery mode** via the power switch (three power
  cycles) for a new token, Wi-Fi change, previous firmware or factory reset.
- **Install and updates without a cable**: flashable through the stock
  firmware's own update page; afterwards browser upload, `curl`
  (`POST /update`) or fetch-from-URL into the inactive OTA slot with
  bootloader rollback, boot-slot switch to **return to stock**, partition
  and flash downloads, `scripts/flash_ota.sh` and `scripts/backup_device.sh`.
- **E-ink refresh policy**: differential (partial) refresh by default – no
  inversion flash on every new frame – with a forced full refresh
  periodically, for condition cards, layout changes and boot frames; the
  shown frame survives deep sleep in RTC memory so a battery wake resumes
  with a partial. Refresh counters in `GET /api/screen/state`.
- **On-device UI**: *Ready* card with the device URL, USB / battery /
  Wi-Fi-lost badges, *No Wi-Fi*, *Updating* and battery cards, boot splash
  with the recovery hint, set-up / recovery / pairing / identify frames,
  LED overlays per state.
- **Developer tooling**: PlatformIO environments `tickr`, `tickr_dev`
  (ArduinoOTA, `/dev` diagnostics page) and `native`; pinned
  toolchain and libraries; host unit tests (`pio test -e native`); GitHub
  Actions and GitLab CI; image-size gate against the OTA slot and a gzip
  gate for the web UI; `TICKR_LOG_VERBOSE` build flag for the informational
  serial log.

[Unreleased]: https://github.com/vskiwi/tickrdisplay/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/vskiwi/tickrdisplay/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/vskiwi/tickrdisplay/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/vskiwi/tickrdisplay/releases/tag/v0.1.0
