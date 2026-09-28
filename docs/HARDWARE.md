# TickrMeter hardware notes

Everything below was measured or observed on two production units of
different board revisions – **rev A** and **rev B** – which differ in flash
vendor, stock build and power-sensing wiring ([Power sensing](#power-sensing)
covers both). Other revisions may differ; please open an issue with your
findings if yours does. Nothing here is vendor documentation; the vendor
does not publish schematics.

## Overview

| Component | Specification |
|-----------|---------------|
| **MCU module** | Marked ESP32-WROOM-32E; chip reports ESP32-D0WD-V3 (rev 3.1) |
| **CPU** | Dual-core Xtensa LX6, 240 MHz |
| **Flash** | **8 MB physical on both known units**, read at runtime by `esp_flash_read_id()` (`GET /api/system/info` → `flash.jedec_id`): rev A GigaDevice GD25Q64, JEDEC `0xC84017`; rev B XMC XM25QH64, JEDEC `0x204017` (`17` → 2^23 bytes in both). The stock bootloader, app images and partition table are configured for **4 MB**, so with the stock bootloader only the first 4 MB are usable and `esp_flash_read()` fails beyond `0x400000`; the upper half needs a new bootloader + partition table over UART. Check yours with `esptool.py flash_id`. |
| **PSRAM** | None |
| **Display** | E-ink 2.9", 296×128, SSD1680 controller, SPI (write-only, no MISO) |
| **Power** | USB-C (power only), stacking pads (see below) and internal Li-Po; charger/switch IC on board, no fuel gauge (I2C scan found nothing) |
| **Audio** | Mono amplifier U7 (SOIC-8, marking K990/Y411) driven from DAC1, with standby pin |
| **LEDs** | Discrete R, G, B (active-low) |
| **Buttons** | None. Only a power slide switch. |

## Pin configuration

Constants: `tickr_display/src/hal/hal_pins.h`.

### Display (SSD1680)

| Function | GPIO | Notes |
|----------|------|-------|
| CLK | 13 | SPI clock |
| MOSI | 14 | SPI data |
| CS | 15 | Chip select |
| DC | 27 | Data/command |
| RST | 26 | Hardware reset |
| BUSY | 18 | Busy (LOW = ready) |

### Power control

| Function | GPIO | Notes |
|----------|------|-------|
| EP_3V3_EN | 19 | Display power rail (LOW = ON) |

### LEDs

| Colour | GPIO | Notes |
|--------|------|-------|
| Blue | 21 | Active LOW |
| Green | 22 | Active LOW |
| Red | 23 | Active LOW |

### Audio

| Function | GPIO | Notes |
|----------|------|-------|
| Amp enable | 17 | HIGH = amplifier ON |
| Audio out | 25 | DAC1 output |

### Battery / power sensing

Details and measured values: [Power sensing](#power-sensing).

| Function | GPIO | Notes |
|----------|------|-------|
| Rail – "VIN" (`PIN_USB_VIN`, `vin_v` in the API) | 33 | ADC1_CH5, divider **21/10**. The system rail after the OR-ing element: VBUS with a cable, VBUS minus the pad drop through the stacking pads, the cell minus a diode drop on battery. Rev B reads the 3.3 V ceiling while GPIO 4 is low – the boot-time board-profile signature. Decision channel of rule `rail` (rev A) and, with GPIO 32, of rule `diff` (rev B). |
| Cell – "VSYS" (`PIN_BAT_CELL`, `vsys_v` in the API) | 32 | ADC1_CH4, divider **189/100 by default** (config `adc_cell_num/den`). The cell / charger output on both revisions; source of `batt_pct` and of the low-battery latch. |
| Sense enable (`PIN_BAT_SENSE_EN`) | 4 | **Rev B: enable of the GPIO 32/33 sensing network** – high (or the internal pull-up) switches 32/33 to the dividers; low / pull-down (reset default) / float leaves them at the ceiling and lets GPIO 34–39 follow the bus. No function on rev A. Driven HIGH while awake, LOW before deep sleep (not held). The stock firmware wrote it high without `pinMode` and never saw a cell on rev B. |
| GPIO 34–39 | 34–39 | Input-only ADC1 pads, diagnostics only (`/api/power/raw` `aux[]`); no firmware decision is based on them. Rev B: follow the USB bus while the sense network is disabled. The stock firmware logs GPIO 37 (`Real charging mode: analog 37: %d …`) and uses it as a light-sleep wake source. |

### Test points on PCB

| Name | Connection |
|------|------------|
| EP_3V3 | Display 3.3 V rail |
| STBY | GPIO 17 (amp enable) |
| 3V3 | Main 3.3 V |

### Other GPIO

| GPIO | Function |
|------|----------|
| 0 | Boot strapping (pull LOW during reset for the UART bootloader); external pull-up on the board – reads 1 regardless of the internal pull |
| 1 | UART0 TX |
| 3 | UART0 RX |
| 2, 5, 12 | Free – each follows the internal pull-up/-down, nothing external (pull probe on rev B). 12 is the MTDI strapping pin; the display is write-only, so no MISO is wired |
| 4 | `PIN_BAT_SENSE_EN` (see above) |
| 16 | Free (pull probe on rev B). **Dummy SPI MISO input in every env** (`PIN_SPI_MISO_DUMMY`): the SPI driver needs an input pin, and passing −1 would select the VSPI default GPIO 19 = the display power enable. Do not move it to GPIO 4 – that holds rev B's sense network disabled |
| 32, 33 | ADC dividers (cell / rail); on rev B tied high while GPIO 4 is low |
| 34–39 | Input-only, no pull resistors. The digital level of 35–39 does not always follow the saturated ADC – the firmware reads them with the ADC only, never `digitalRead()` |

## Power inputs and the stacking pads

Two power inputs (not traced on the PCB):

* **USB-C** (power only).
* **Contact pads on the top and bottom faces of the case.** Units are
  stacked; the pads pass power from the lower unit to the upper one. The
  vendor calls the stack "magcharge".

What the pads carry, measured with each unit as the source (cable in) and as
the receiver (on top of the other):

* **Only the USB bus of the lower unit**, not its battery: with both units
  on battery the upper one sees its own cell.
* **Drop across the pad path ≈ 0.45–0.48 V** in firmware units with rev A as
  the receiver (5.18–5.27 V with the cable → 4.73–4.80 V on the pads) – a
  diode (OR-ing / back-feed protection) plus contact resistance. With rev B
  as the receiver the rail read 5.00 V against 5.27 V at the source.
* **Both directions on both revisions**: each revision on top of a
  cable-powered unit of the other is detected as `usb`, at runtime without a
  reboot (the cell bridges the transition, the 3-poll debounce switches the
  state) and at boot. No `power_source` override is needed for a stacked
  unit.

Rail readings (GPIO 33 × 21/10, steady state, spread ≤ 5 mV) by supply path:

| Unit | Cable in this unit | Through the pads | On its cell |
|------|--------------------|------------------|-------------|
| Rev A | 5.18–5.27 V | 4.73–4.80 V | 4.17–4.18 V (cell 4.18) |
| Rev B | 5.37 V | 4.90–5.00 V | 4.24 V (cell 3.98) |

**Practical consequences for users of a stack:**

* Exposed pads on a **metal surface can short** the lower unit's USB bus.
  How the vendor's own aluminium base keeps the pads off the metal is not
  known ([Unverified / open](#unverified--open)). Rule until it is: leave the
  pads bare, but keep the **bottom unit of a stack off bare metal** – a mat
  or silicone feet under it.
* **Taped or otherwise non-contacting pads = the upper unit runs on its
  cell** while looking like part of a powered stack. The firmware shows this
  correctly on both revisions (`power: battery`, battery glyph with a
  percentage on the e-ink), and such a unit deep-sleeps between refreshes as
  any battery unit does – in the panel it becomes a sleeper. Check that the
  pads actually make contact before trusting a "powered through the pads"
  configuration.

## Power sensing

### Board revisions – summary

| | **Rev A** | **Rev B** |
|---|---|---|
| Flash | GD25Q64, JEDEC `0xC84017`, 8 MB | XMC XM25QH64, JEDEC `0x204017`, 8 MB |
| Stock partition table | `default.csv` incl. `coredump` (6 partitions) | same layout **without** `coredump` (5 partitions) |
| Stock build | Feb 2025 build | a different build of the same code base; `xmc` flag in its status JSON |
| GPIO 33 (rail) | live divider in every state: cable 5.18–5.27, pads 4.73–4.80, cell 4.17–4.18 V (fw units) | 3.3 V ceiling (raw 4095) while GPIO 4 is low; with GPIO 4 high the same divider: cable 5.37, pads 4.90–5.00, cell 4.24 V |
| GPIO 32 (cell) | 4.18–4.23 V, follows the cell (−16 mV at the pin cable → battery) | ceiling while GPIO 4 is low; with GPIO 4 high 3.98–4.04 V (−27 mV at the pin cable → battery) |
| GPIO 4 | no effect in any mode (output high/low, pull-up/-down, float) | **sense enable**: high or internal pull-up switches 32/33 to the dividers and 34–39 to 0; low / pull-down / float = 32/33 at 3.3 V, 34–39 follow the bus |
| GPIO 34–39 (diagnostics) | 34 ≈ 80–113 raw, 35–39 = 0 in every state | GPIO 4 low: 34 ≈ 290 raw without a cable, all six 4095 with one (also through the pads). GPIO 4 high: 35/36/38/39 = 0, 37 ≈ 10–70 even with a cable |
| **USB detection** | rule **`rail`**: GPIO 33 > 4.5 V = USB, < 4.35 V = battery, hysteresis in between; boot peak-hold of the rail; 3-poll debounce | rule **`diff`**: GPIO 33 − GPIO 32 at the pin > +80 mV = USB, < −30 mV = battery, hold in between; boot peak-hold of the difference; 3-poll debounce |
| **Battery voltage / level** | GPIO 32 × 189/100 → `vsys_v`, `batt_pct`; 4.18 V on the cell → 100 % | the same, with GPIO 4 high; 3.98 V on the cell → 80 % |
| Deep-discharge protection | firmware (3.30 V latch, 3.45 V recover) + hardware | the same |
| Firmware profile (`board` in `/api/status`) | `"A"` | `"B"` |

**Profile detection.** `power_init()` reads GPIO 33 once per boot **with the
sense enable (GPIO 4) low** (its reset default, set explicitly) – a value
between the floor and the ceiling is a live divider (A), the ceiling is B,
the floor is `"?"` (no detector – `power: unknown`, behaves as USB) – and
then drives GPIO 4 high for the rest of the awake time. The `power_source`
override (`auto | usb | battery`) sits on top of both profiles.

### How the firmware measures

`analogReadMilliVolts()` on GPIO 32 / GPIO 33, 12 bit, 11 dB attenuation,
eFuse calibration, mean of 16 samples; the rail (GPIO 33) × 21/10, the cell
(GPIO 32) × 189/100 by default (`logic/battery.h`, config `adc_cell_num/den`).
The 2.1 rail factor was fitted on rev A against the *uncalibrated*
`raw × 3.3 / 4095` scale, not a multimeter, and is applied to a *calibrated*
pin voltage – it can be off by up to ≈ 10 % (a 2.0:1 divider fits as well).
The 1.89 cell factor is the stock firmware's raw-count factor translated to
calibrated millivolts (a full charging cell = 4.20–4.23 V on both units). The
ADC ceiling is ≈ 3150 mV at the pin, which the firmware reports as
**6.615 V** on the rail (= 3150 × 2.1) when a channel is saturated.

### Thresholds and where they come from

All constants live in `logic/battery.h` so a divider correction moves them
together.

| Quantity | Value | Source |
|----------|-------|--------|
| Cell channel | GPIO 32 on both revisions; 16-sample calibrated mV | measured on both units; the stock firmware reads the same pin |
| Cell divider | **189/100** default, config `adc_cell_num/den` | the stock's 1.702 V per raw count ⇒ ≈ 1.89 on calibrated mV; *not multimeter-checked* |
| Rail channel / divider | GPIO 33, **21/10** | fitted on rev A; 2.0 would fit a 4.99 V VBUS – *not multimeter-checked* |
| Stock cable detector | `analogRead(33) > 2500` raw (≈ 4.26 V on the stock scale) – **not copied** | sits at the full-cell level on rev A, meaningless on rev B with the network off |
| USB / battery rule, rev A | rail(33) × 2.1 > **4.50 V** ⇒ usb, < **4.35 V** ⇒ battery (hysteresis) | measured points below |
| USB / battery rule, rev B | rail(33) − cell(32) at the pin > **+80 mV** ⇒ usb, < **−30 mV** ⇒ battery, hold in between; `invalid` if a channel is ≥ 4080 or ≤ 8 raw | measured points below |
| Stock percentage tiers, as cell voltage via the stock formula | ≤ 30 % "high" saving + icon ≈ 3.77 V; < 4 % sleep card ≈ 3.35 V; < 2 % off ≈ 3.30 V | stock firmware (interpretation) |
| TickrDisplay battery thresholds | BATTERY LOW badge **15 % (≈ 3.62 V)**; EMPTY latch **3.30 V**, recover **3.45 V**; 5 % display steps | `battery.h`, `device_state.h` |
| Sense enable | GPIO 4 `PIN_BAT_SENSE_EN` high while awake (both revisions), low before deep sleep; 150 ms settle after the enable (the stock's `delay(150)`) | measured on rev B |

### Detector

One measurement path on both revisions; only the decision rule is picked per
board profile, because the two boards' OR-ing elements differ.

1. **Boot.** `power_init()` puts GPIO 4 (`PIN_BAT_SENSE_EN`) in its reset
   state (input, pull-down), reads GPIO 33 (16 samples) → **board profile**:
   live divider = A, ceiling = B (network off), floor = `?`. Then GPIO 4 →
   **output HIGH**, **150 ms** settle. The config is read (`power_source`
   override, `adc_cell_num/den`), then `power_get_source()` takes the **boot
   peak-hold**: 12 samples 20 ms apart, each = 16 conversions on GPIO 33
   (rail, × 21/10) and 16 on GPIO 32 (cell), plus the pin-millivolt
   difference rail − cell; the sample with the highest **board metric** (rail
   on A, difference on B) decides through the board's rule. The rail sags
   under boot current through the pads (a first reading after a stack-wide
   reboot was 0.3 V low), while a cell can never push the metric up – so the
   maximum is the honest sample.
2. **Rules** (`logic/battery.h`, `power_sense_board()`):
   * **A – `rail`**: USB above **4500 mV**, battery below **4350 mV**,
     hysteresis in between. Rail channel saturated (raw ≥ 4080) or floored
     (raw ≤ 8) ⇒ *invalid*.
   * **B – `diff`**: USB when rail − cell > **+80 mV** at the pin, battery
     when < **−30 mV**, hold in between. Either channel saturated / floored
     ⇒ *invalid* (this is also what a rev B reads while GPIO 4 is low:
     `power: unknown`).
   * **`?`** – no rule, *invalid*.
   * **Why two rules:** on the pads a rev A rail sits at the level of its
     full charging cell (+21 mV difference; with the boot sag through the
     pads, 4.4–4.56 V measured, it would read −140…−63 mV = "battery"), so
     the difference cannot serve rev A; and a rev B cell of 3.98 V already
     puts the rail at 4.24 V, a full one at ≈ 4.49 V, so the 4.35 / 4.5 V
     thresholds cannot serve rev B.
   * Hold / hysteresis band at boot resolves towards **USB** (stay awake –
     the recoverable error); at runtime it holds the debounced state.
3. **Runtime.** 1 s polls from the status bar, one sample each, through a
   **3-poll debounce**; *invalid* holds the state. *Invalid* in `auto` mode
   ⇒ behave as USB, report `power: "unknown"`. The `power_source` override
   wins over the detector. A cable pulled at runtime moves the icon and the
   API within the debounce; the deep-sleep decision itself is taken in
   `setup()`, so the firmware restarts into the battery flow **2 min** after
   the debounced flip (*On battery* card first), guarded by the override, a
   Pull URL, board `?`, 60 s of stability before the flip, flapping detection
   (3 flips in 10 min → 10-min grace) and a lock after a restart that reads
   USB (`logic/device_state.h`: `DS_POWER_GRACE_MS`, `DS_POWER_STABLE_MS`,
   `DS_POWER_GRACE_LONG_MS`). The margins of the measured points below are
   what those guards lean on. See [`DEVICE_UI.md`](DEVICE_UI.md) →
   *Power-mode switch*.
4. **Cell level, both revisions.** `vsys_v` / `batt_pct` / the low-battery
   latch (3.30 V, recover 3.45 V) from GPIO 32 × 189/100: rev A 4.18 V →
   100 %, rev B 3.98 V → 80 % on the cell (5 % steps); `null` and the *BAT*
   outline only on `?`. The BATTERY LOW badge (≤ 15 %, ≈ 3.62 V with this
   divider) and the EMPTY card (3.30 V) are drawn on both revisions; a board
   `?` gets neither (`batt_known false`).
5. **Deep sleep.** `power_deep_sleep()` drives GPIO 4 **LOW** (not held; the
   pad floats in sleep, which on rev B also leaves the network off), then
   the usual holds (EPD power, LEDs, amplifier).
6. **API.** `GET /api/power/raw` exposes every input of the detector –
   raw counts (mean, min, max), calibrated pin millivolts and the divided
   value per channel, `diff_mv`, `sense_en`, `board`, `rule`, the ADC
   calibration kind, both dividers, the thresholds, the boot window and
   GPIO 34–39 (`aux[]`); `POST /api/power/source` sets the override.
   `/api/status` carries `vsys_v` (cell), `vin_v` (rail), `board`, `power`;
   `batt_pct` is in `/api/screen/state` `.status`. Field lists:
   [`API.md`](API.md).
7. **Known risks / thin margins.** A weak adapter (4.75 V) behind the pads
   (−0.45 V) lands the rail at 4.30 V = the cell level: *battery* under both
   rules – the override exists for that; charging current through the pads
   lowers the reading further. Thinnest margins as measured: rev B on the
   cell −87 vs −30 mV (57 mV); rev B on the pads +250 vs +80 mV, read with a
   4.03 V cell; rev A on the pads 4.79 vs 4.5 V (0.29 V) with the peak-hold,
   where the difference (+42…+54 mV) sat inside the hold band – the case that
   keeps rule `rail` on rev A. Host tests: `test/test_battery` on the
   measured points.

## Programming access

* **USB-C is power only.** The data lines are not wired to the ESP32; there
  is no USB-serial bridge on the board.
* Two unpopulated 6-pin **headers inside the case** (open it to reach them):

  | Header | Pins | Use |
  |--------|------|-----|
  | Header 1 (manual programming) | IO0, RST, GND, RX, TX, 5V | Hold IO0 low, pulse RST, release IO0 → UART bootloader |
  | Header 2 (auto-program) | DTR, RTS, RX, GND, 5V, TX | Wire to an adapter with DTR/RTS for esptool auto-reset |

  Use a **3.3 V** logic-level adapter. 5V on the header can power the board
  from the adapter but is not required if USB-C is connected.
* No buttons exist for entering boot mode; use the header.
* **Cable-free path:** the stock firmware exposes an OTA upload page
  (`/update`, WiFiManager) which the vendor documents for local updates.
  Flashing TickrDisplay through it does not require opening the case; see
  [`FLASHING.md`](FLASHING.md).

## Stock firmware

Facts observed on the two units' flash contents and on the vendor's recovery
image. **None of the vendor's binaries are distributed here**: the stock
firmware is the vendor's copyrighted work, and a device dump also contains
your Wi-Fi credentials ([`LEGAL.md`](LEGAL.md)). Everything below is
factual, non-copyrightable observation (version strings, offsets, host names,
log format strings) read with `strings`.

| Property | On device | Vendor `recoveryfw.bin` |
|----------|-----------|-------------------------|
| Framework | Arduino core for ESP32 (arduino-lib-builder), built with PlatformIO (Windows host path `C:/Users/<user>/.platformio/...` leaked in strings) | same |
| ESP-IDF | **v4.4.7** (`v4.4.7-dirty`, Mar 2024) | **v4.4.5** (Jun 2023) |
| Newest build date string | Feb 2025 (rev A); rev B is a different build of the same code base | Aug 2023 |
| Source files named in asserts/logs | `src/display_func.cpp`, `src/battery.cpp`, `src/led.cpp`, `src/mqtt.cpp` | same |
| Wi-Fi provisioning | tzapu/WiFiManager, open AP **`TickrMeter`** (no password), portal at `192.168.4.1` | same |
| Local OTA | WiFiManager `/update` page (Arduino `Update` library) | same |
| Recovery | Downloads `http://api.tickrmeter.io/recoveryfw.bin` (plain HTTP) | – |
| Cloud endpoints | `https://api.tickrmeter.io/api/pages/` (page content), `comm.tickrmeter.io` (real-time channel; there is an `mqtt.cpp`, broker details not analysed) | same |
| Power handling (log strings, `src/battery.cpp`) | `Real charging mode: analog 37: %d, vin: %d, isCharging: %d`, `Battery level: %d, Charging: %d, vin: %d`, `Battery is very low, going to sleep`, `Woke up because of GPIO 37`; status JSON carries `battery`, `vin`, `isCharging`, `xmc`, `flashID` | same |
| Flash encryption / secure boot | **off** / **off** | – |
| Flash-size code in image headers | 4 MB (`0x20`) | 4 MB |

TickrDisplay does not talk to any vendor service; the vendor protocol was
not analysed beyond the host names.

### Stock partition table (4 MB, arduino-esp32 `default.csv`)

| Name | Type | Subtype | Offset | Size |
|------|------|---------|--------|------|
| nvs | data | nvs | 0x009000 | 0x005000 |
| otadata | data | ota | 0x00E000 | 0x002000 |
| app0 | app | ota_0 | 0x010000 | 0x140000 |
| app1 | app | ota_1 | 0x150000 | 0x140000 |
| spiffs | data | spiffs | 0x290000 | 0x160000 |
| coredump | data | coredump | 0x3F0000 | 0x010000 |

Rev B's table has the same offsets without the `coredump` entry.
TickrDisplay's default environment uses the same layout so that an image
uploaded through the stock `/update` page lands in the inactive OTA slot and
the stock firmware can be kept in the other slot as a fallback. The 8 MB
chip does not change this: the stock bootloader is built for 4 MB, so the
upper half is only reachable with a new bootloader and partition table
written over UART, never over OTA.

### Official vendor binaries

Do not copy the vendor firmware into this repository. Fetch it from the
vendor if you need it:

| File | URL | SHA-256 (as observed) |
|------|-----|-----------------------|
| `recoveryfw.bin` | `https://api.tickrmeter.io/recoveryfw.bin` | `4f931ac193bc13a3510bfa5107ea66fdd0a725abaf4163b4208f8b823ddae7f3` |

The vendor may replace the file at any time; verify with
`shasum -a 256 recoveryfw.bin`. The stock firmware itself fetches this URL
over **plain HTTP** when it enters recovery.

### Taking your own flash dump

You need a 3.3 V USB-UART adapter on the internal header
([Programming access](#programming-access)) and
[esptool](https://github.com/espressif/esptool):

```bash
pip install esptool
# Enter the bootloader: hold IO0 to GND, pulse RST to GND, release IO0.
esptool.py --chip esp32 --port /dev/tty.usbserial-XXXX flash_id            # real flash size / JEDEC ID
esptool.py --chip esp32 --port /dev/tty.usbserial-XXXX --baud 921600 \
    read_flash 0x0 0x400000 tickrmeter_full_4mb.bin                        # the 4 MB the bootloader uses
strings -t x tickrmeter_full_4mb.bin > strings_offset.txt                  # readable strings with offsets
dd if=tickrmeter_full_4mb.bin of=ptable.bin bs=4096 skip=8 count=1         # partition table at 0x8000
python "$IDF_PATH/components/partition_table/gen_esp32part.py" ptable.bin
dd if=tickrmeter_full_4mb.bin of=app0.bin bs=65536 skip=1 count=20         # app0 image
esptool.py --chip esp32 image_info --version 2 app0.bin                    # IDF version, build date, flash-size code
```

**WARNING – keep the dump private.** NVS is not encrypted on this device, so
the dump contains your Wi-Fi SSID and password in plain text (`sta.ssid` /
`sta.pswd`), the derived PMK, your router's BSSID and the factory MAC. Never
attach it to a bug report and never commit it. Without opening the case, use
the backup endpoint of TickrDisplay once installed ([`FLASHING.md`](FLASHING.md)).

### Security posture (as found)

No flash encryption and no secure boot – the flash is plain text and the
images unsigned, so nothing had to be bypassed to read or replace the
firmware. NVS is unencrypted (credentials readable from a dump). The stock
firmware exposes the standard WiFiManager pages (`/`, `/wifi`, `/wifisave`,
`/param`, `/info`, `/u`, `/update`, `/close`, `/exit`, `/restart`, `/erase`);
`/update` is the vendor's documented "force firmware update locally" path
and is what makes cable-free installation of TickrDisplay possible. The
set-up access point `TickrMeter` is open (no password) at `192.168.4.1`. The
recovery image is fetched over HTTP without TLS.

## Unverified / open

> **Unverified:** the full deep-sleep cycle on the cell with the unified
> detector (wake → `sense_en` high → percentage) and a rev B on the pads with
> a *full* cell have not been run; deep-sleep current and battery life are
> not measured.

> **Unverified:** the true divider ratios on GPIO 32/33 – 2.0 vs 2.1 on the
> rail, 1.89 vs 2.0 on the cell. Neither node is reachable without opening
> the case, so the cell divider stays a config field (`adc_cell_num/den`).
> Either the two channels have different dividers (32 ≈ 1.9, 33 ≈ 2.0–2.1)
> or GPIO 32 is not the bare cell. The 33 − 32 gap on battery – 0.46–0.48 V
> on rev A, ≈ 0.17 V on rev B after the dividers – points at a silicon diode
> on rev A and a Schottky / ideal diode on rev B between the cell and the
> rail (interpretation).

> **Unverified:** the BATTERY LOW badge and the BATTERY EMPTY card have not
> been seen on the e-ink; the runtime USB → battery switch (cable pulled from
> an awake unit) has not been exercised on hardware.

Open hardware questions:

* Battery capacity and charger IC part number; whether the charger IC exposes
  CHRG/STDBY status lines to a GPIO.
* Exact behaviour of the power switch (does it cut the battery or only the
  regulator?).
* Whether the stacking pads sit before or after the lower unit's charger, and
  whether the upper unit charges its cell from them.
* **Stacking pads on a metal base.** The vendor's own aluminium base is
  metal – does it recess or insulate the pads, or carry power itself? Rule
  until known: bare pads, bottom unit on a mat / feet.
* Rev B: net topology of GPIO 34–39 (one net or six) and why their digital
  level does not always follow the saturated ADC (a high-impedance source
  that charges the ADC sample capacitor but not the digital input buffer
  would behave like this).
* The module is marked ESP32-WROOM-32E, which does not bring GPIO 37/38 out
  to module pins, yet GPIO 37 – the stock firmware's channel – follows the
  bus like 35/36/39 on rev B. Module marking vs actual module, or coupling
  inside the module; not traced.
* Board revisions other than A and B – unknown; reports welcome.

## Appendix – measured points

Release build, `power_mode auto`, `GET /api/power/raw`, three samples per
row, spreads ≤ 5 mV; cell = GPIO 32 × 1.89, rail = GPIO 33 × 2.1.

| Configuration | Unit | `rule` | Cell | Rail | `diff_mv` | Result |
|---|---|---|---|---|---|---|
| Cable in A, B on top (pads) | A | rail | 4.21 V | 5.27 V | +282 | usb |
| same | B | diff | 4.03 V | 5.00 V | +250 | usb (`boot_diff` +279…+284) |
| Cable out, both on their cells (stacked) | A | rail | 4.18 V | 4.17 V | −228 | battery, 100 % |
| same | B | diff | 3.98 V | 4.24 V | −87…−92 | battery, 80 % |
| Cable in B, A on top (pads), runtime switch | A | rail | 4.23 V | 4.79 V | +42…+45 (hold band) | usb – by the rail |
| same | B | diff | 4.04 V | 5.37 V | +415 | usb |
| A power-switch cycle on B's pads (boot path) | A | rail | 4.23 V | 4.80 V | +45 | usb; `boot_rail_min` = `max` = 4819 mV |

Design points in pin millivolts (rail / cell, steady reads on which the
thresholds and host tests rest): A cable 2495 / 2233 (+262), A pads 2255 /
2234 (+21), A battery 1989 / 2217 (−228); B pads 2334 / 2132 (+202),
B battery 2019 / 2105 (−86). The rail on the pads varies with the source's
supply (rev A 4.73–4.80 V, rev B 4.90–5.00 V across sessions).
