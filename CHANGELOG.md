# Changelog

All notable changes to TickrDisplay are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and this project adheres to [Semantic Versioning](https://semver.org/).
Until `1.0.0` minor versions may contain breaking changes.

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

[0.1.0]: https://github.com/vskiwi/tickrdisplay/releases/tag/v0.1.0
