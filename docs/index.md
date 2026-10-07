---
title: TickrDisplay – cloud-free open firmware for the TickrMeter e-ink ticker
description: Open-source replacement firmware for the TickrMeter ESP32 e-ink ticker. Crypto quotes fetched by the device itself, HTTP/MQTT push, several devices on one shelf, no vendor cloud or account.
---

# TickrDisplay

**Cloud-free, open-source firmware for the TickrMeter e-ink ticker.**
Crypto quotes fetched by the device itself, any text pushed over HTTP or MQTT, several devices arranged on one virtual shelf – no vendor account, no subscription, nothing leaves your LAN.

<p align="center">
  <img src="images/hero-ticker.png" width="640" alt="TickrDisplay e-ink frame: BTC/USDT from Binance with price, 24-hour change and sparkline">
</p>

- **Source code and documentation:** [github.com/vskiwi/tickrdisplay](https://github.com/vskiwi/tickrdisplay)
- **Download firmware:** [latest release](https://github.com/vskiwi/tickrdisplay/releases/latest)
- **License:** GPL-3.0-or-later

> **Status: alpha.** Flashing third-party firmware voids the vendor warranty – read the [disclaimer](https://github.com/vskiwi/tickrdisplay#disclaimer) first.

## What it does

- **Cloud-free crypto tickers** – CoinGecko, Kraken and Binance presets, or any JSON API via *Custom JSON*; HTTPS verified against roots baked into the firmware.
- **Push your own content** – `POST /api/screen`, MQTT or a Pull URL: title, value, LED colour, beep or RTTTL melody. Works from Home Assistant, Node-RED, curl or scripts.
- **Two power modes** – always-on over USB with instant updates; deep sleep with periodic wake-ups on battery.
- **The shelf** – every device on your LAN as a live card with an e-ink preview, drag-and-drop arrangement, groups without a server, firmware updates for the whole group.
- **Local web UI and JSON API** on `http://<device-ip>/`, optional API token, pairing with a code shown on the screen.
- **Cable-free install** through the stock firmware's own update page; dual-slot updates with return to stock; recovery mode via the power switch.
- **Gentle on the e-ink** – partial refreshes by default, a full refresh only where it is needed.

## Documentation

- [README](https://github.com/vskiwi/tickrdisplay#readme) – overview, quick start, configuration
- [Flashing and going back to stock](https://github.com/vskiwi/tickrdisplay/blob/main/docs/FLASHING.md)
- [Tickers and data sources](https://github.com/vskiwi/tickrdisplay/blob/main/docs/TICKERS.md)
- [HTTP / MQTT API](https://github.com/vskiwi/tickrdisplay/blob/main/docs/API.md)
- [Web UI](https://github.com/vskiwi/tickrdisplay/blob/main/docs/WEB_UI.md) and [device UI](https://github.com/vskiwi/tickrdisplay/blob/main/docs/DEVICE_UI.md)
- [Several devices, groups, shelf](https://github.com/vskiwi/tickrdisplay/blob/main/docs/MULTI_DEVICE.md)
- [Hardware notes](https://github.com/vskiwi/tickrdisplay/blob/main/docs/HARDWARE.md)
- [Development](https://github.com/vskiwi/tickrdisplay/blob/main/docs/DEVELOPMENT.md), [changelog](https://github.com/vskiwi/tickrdisplay/blob/main/CHANGELOG.md), [security](https://github.com/vskiwi/tickrdisplay/blob/main/SECURITY.md)

Built with PlatformIO for the ESP32. Issues and pull requests are welcome on [GitHub](https://github.com/vskiwi/tickrdisplay/issues).
