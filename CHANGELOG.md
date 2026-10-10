# Changelog

All notable changes to TickrDisplay are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and this project adheres to [Semantic Versioning](https://semver.org/).
Until `1.0.0` minor versions may contain breaking changes.

## [Unreleased]

### Added

- **Tickers**: a **2×2 grid** of up to four ticker sources on one panel –
  each cell with the short-name badge, the change with its triangle and the
  price (whole at 18 pt, else without its fraction, else smaller), the age
  line and the badges in the last cell; two or three tickers use the same
  cells. Any mix of presets. All sources are fetched in one cycle and drawn
  in one frame; a source that fails keeps its last price with `?` in place
  of the change, also across deep sleep; a changed set or order of tickers
  is a full refresh. On battery the interval has a 15-minute floor with a
  grid. Editor: *View* (one ticker / grid) with up to four rows – preset,
  symbol, market, short name, reorder / remove, *Test* per row – and the
  *Advanced* settings per row. API: `tk_view`, `tk_n`, `tk1_*` … `tk3_*`
  in `/api/config` and `POST /config`; `/api/screen/state` reports
  `layout: "grid"` with `symbols[]` and `shorts[]`.
- **Tickers**: the ticker's short name on a black badge top left of the panel
  (`BTC`, `XBT`, `ETH` – up to 7 characters, 18 pt white on black, smaller
  when longer), with the change beside it and the price below. Derived from
  the symbol when the new *Short name* field under *Advanced* is empty
  (`tk_short`): exchange symbols upper-cased, a pair typed as one word loses
  its quote currency, the common CoinGecko ids map to their tickers; *Test*
  returns the derived name. A proxy sets it with the payload field `short`;
  a payload without one draws the frame as before. `/api/screen/state` adds
  `short`.

### Changed

- **Build**: smaller firmware image – the `printf` family comes from the ESP32
  ROM (no floating-point or 64-bit integer formats in the image; floats are
  formatted by `src/logic/fmt_float`, a build step rejects `%f`-style formats
  under `src/`), core dumps to flash are disabled (a panic still prints its
  backtrace and reboots), and the web pages are packed with zopfli when the
  module is installed.
- **Ticker look**: a price with an integer part of 1 000 or more drops its
  fraction (`84 014.90` → `84 015`, rounded half-up) when that keeps the
  largest price size on the panel instead of stepping down to the next one;
  a price that fits whole keeps its decimals, and the *Decimals* /
  *Separator* settings are unchanged.

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

[Unreleased]: https://github.com/vskiwi/tickrdisplay/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/vskiwi/tickrdisplay/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/vskiwi/tickrdisplay/releases/tag/v0.1.0
