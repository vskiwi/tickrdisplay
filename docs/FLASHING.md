# Flashing, updating and recovering a TickrMeter

How to get TickrDisplay onto a TickrMeter, back up the stock firmware, update TickrDisplay later
and go back to the vendor firmware. Pin-outs and header locations are in [`HARDWARE.md`](HARDWARE.md);
the endpoints used below are specified in [`API.md`](API.md).

**Read the whole document before flashing.** The device has no USB-serial bridge (USB-C is power
only), so the *only* way to flash without opening the case is over Wi-Fi, and the only way to fix
a device that no longer boots to a web page is the internal UART header.

## 1. How the two firmwares coexist

The stock firmware is a plain Arduino-ESP32 application built with the standard Arduino
`default.csv` partition layout (two 1.25 MB OTA slots `app0`/`app1`, [section 8](#8-stock-partition-table)).
Its Wi-Fi setup portal is [WiFiManager](https://github.com/tzapu/WiFiManager), which ships a firmware
upload page at `http://192.168.4.1/update` (the vendor documents it in "How to force firmware update
locally" on help.tickrmeter.com).

Uploading TickrDisplay through that page writes it into the *inactive* OTA slot and marks that slot
as the boot partition. **The stock firmware is not erased** – it stays in the other slot. TickrDisplay
has the same mechanism (`POST /update`, page `/system#firmware`) plus a way to switch the boot slot
back, so the device can always be returned to stock without opening it, as long as TickrDisplay boots.

```
                    stock /update              TickrDisplay /update
  app0: stock  --------------------->  app0: stock (kept)  ---------------->  app0: TickrDisplay v2
  app1: empty                          app1: TickrDisplay v1 (boot)          app1: TickrDisplay v1
                                                                             (boot = app0)
```

TickrDisplay never hard-codes partition addresses; it asks the ESP-IDF partition API at runtime, so
the same `firmware.bin` runs on any partition table with `app0`/`app1` slots it fits in.

## 2. Flashing TickrDisplay for the first time

Build the image first, or download `tickrdisplay-<version>.bin` from a release:

```sh
cd tickr_display
pio run                       # -> .pio/build/tickr/firmware.bin and dist/tickrdisplay-<version>.bin
```

`tickr` is the default environment; see [`DEVELOPMENT.md`](DEVELOPMENT.md) for `tickr_dev` and the
native test environment. The image must fit into one OTA slot, **1 310 720 bytes (0x140000)**
on the stock table; the build fails if it does not.

### 2a. Over Wi-Fi, without opening the case (recommended)

You need: the device on **USB power** (do not do this on battery), a laptop/phone with Wi-Fi and
a browser, and a way to make your normal Wi-Fi *unavailable* to the device.

1. Make the device's known Wi-Fi network unreachable (turn off the access point, change its
   password temporarily, or move the device out of range). The stock firmware only opens its
   setup portal when it cannot connect.
2. Power-cycle the device with the switch on the back (off, wait 2 s, on).
3. After ~10–30 s an open access point named **`TickrMeter`** appears. Join it.
4. Open **`http://192.168.4.1/update`** in a *regular browser tab*. Do **not** use the captive
   portal pop-up that your OS may show – file uploads do not work from there (WiFiManager itself
   prints this warning). If the pop-up opens, close it and type the URL manually.
5. Choose `tickrdisplay-<version>.bin` and press **Update**. The page shows no progress; the
   upload takes 1–3 minutes. The stock portal answers "Update successful. Device rebooting now..."
   and the device restarts. (The same upload from a terminal: [section 7](#7-command-line-tools-optional).)

6. Restore your Wi-Fi. TickrDisplay boots and reuses the Wi-Fi credentials the ESP32 Wi-Fi stack
   keeps in NVS, so it normally joins your network by itself (check your router for a new
   `TickrDisplay`/`espressif` client, or watch the e-ink status line). If it cannot connect within
   30 s it opens the open **`TickrDisplay`** access point and shows *Wi-Fi setup* on the e-ink:
   join it – phones and laptops usually pop up the set-up page by themselves; otherwise open
   `http://192.168.244.1` (not `192.168.4.1` – that is the stock portal) – pick your network, enter
   the password, press **Connect** and wait for "Connected! IP x.x.x.x". The access point closes a
   few seconds later and the device is reachable at `http://<device-ip>/`. While the access point is
   up, `http://192.168.244.1/system` works as well (its *Firmware* section is the upload page), so a
   device that cannot join any network can still be re-flashed or returned to stock.

If the portal times out (TickrDisplay closes it 180 s after the last client left the access point
and restarts; the stock WiFiManager portal closes after a few minutes without activity) just
power-cycle again.

### 2b. Over UART (requires opening the case)

Inside the case there is a 6-pin programming header: `IO0, RST, GND, RX, TX, 5V`
([`HARDWARE.md` → *Programming access*](HARDWARE.md#programming-access)). Connect a 3.3 V USB-UART
adapter (GND–GND, adapter TX → RX, adapter RX → TX; power the board from USB-C, not from the
adapter's 5 V unless you know what you are doing).

Enter the bootloader: hold `IO0` to GND, pulse `RST` to GND, release `IO0`. Then:

```sh
# identify the chip / flash and confirm the connection
esptool.py --port /dev/ttyUSB0 flash_id

# FULL BACKUP of the stock flash - keep this file forever (4 MB; use 0x800000 if flash_id reports 8 MB)
esptool.py --port /dev/ttyUSB0 --baud 921600 read_flash 0 0x400000 stock_full.bin

# flash TickrDisplay (bootloader + partition table + app)
cd tickr_display && pio run -t upload --upload-port /dev/ttyUSB0
```

`pio run -t upload` rewrites the bootloader and the partition table – with the same 4 MB
`default.csv` layout as stock unless you change `platformio.ini` yourself – so **after a UART flash
the stock firmware is gone from the device**; recovery is then `write_flash 0x0 stock_full.bin` (5c)
or re-uploading the vendor image via `/update` (5b).

## 3. Back up the stock firmware

Do this **immediately after the first boot of TickrDisplay**, before anything else. The stock app
is still sitting in the other OTA slot, and the `nvs` partition still contains the vendor device
identity and the stock Wi-Fi credentials. Everything below is read-only.

### 3a. In the browser

1. Open `http://<device-ip>/system#firmware`. The *Firmware* section shows **Running slot**
   (`app0` or `app1`) – the *other* slot holds the stock firmware right after the first flash.
   To be sure, open *Advanced* → **System info (JSON)** (`/api/system/info`): in `partitions[]`
   the slot whose `description.idf_ver` is **v4.4.5** is the stock image (both images call
   themselves `arduino-lib-builder`; TickrDisplay is built on IDF 4.4.7, and the descriptor date
   is the Arduino core's build date, not the sketch's).
2. Type `http://<device-ip>/api/system/partition/<slot>` (e.g. `.../partition/app0`) into the
   address bar. The browser asks for a user name and password: **any user name, the API token as
   the password**. The download `app0_0x010000.bin` (1.25 MB, the whole slot) starts.
3. Do the same for `nvs` (vendor device identity, stock Wi-Fi credentials – keep it private) and
   `otadata`. Keep the three files together; the address in the file name is where the data
   came from.

A dumped app slot is a plain ESP32 application image. It can be uploaded through
`/system#firmware` later (5b) or written back with `esptool.py write_flash 0x10000 app0_0x010000.bin`
(address = the one in the file name); the trailing `0xFF` padding of the slot is harmless. On a
**`tickr_dev` build** `http://<device-ip>/dev` (Diagnostics) has the same downloads as links per
partition. The release image has no `/dev` page.

### 3b. From a terminal

One command for every partition plus a table of the app slots, or single `curl` downloads:
[section 7](#7-command-line-tools-optional).

## 4. Updating TickrDisplay

Two paths, both over the normal home network (no AP mode needed): HTTP upload to `POST /update`
from the *Firmware* section of `/system` (4a, every build; the same upload from a terminal is in
[section 7](#7-command-line-tools-optional)), and ArduinoOTA push from PlatformIO / `espota.py`
(4b, **developer build `tickr_dev` only**).

### 4a. In the browser (`/system#firmware`, `POST /update`)

Everything happens on one page; you need the API token in that browser. Without it the page opens
*read-only*: a bar at the top asks for the token, and the file chooser, *Upload & flash* and the
*Recovery* buttons are greyed out until you enter it.

1. **Open** `http://<device-ip>/` → ⚙ (*System*) → **Firmware**, or `http://<device-ip>/system#firmware`
   directly.
2. **Note what is running.** The section header carries the version; the first rows are
   *Version · built <date>*, *Running slot* (`app0`/`app1`), *Next slot* (where the upload goes)
   and *Image <size> · MD5 <sketch_md5>*. Write down the version, the slot and the MD5 – they tell
   the old firmware from the new one afterwards (step 5). The same facts, machine-readable: *Advanced*
   → **System info (JSON)** (`/api/system/info`, [`API.md`](API.md)); the device sheet on the
   shelf shows the version in its *Firmware* row.
3. **Keep the previous firmware.** The device has two slots: the upload goes to the *inactive*
   one and the image you are running now **stays in the other slot** untouched – that is your
   first way back (step 6). Also keep a *file* of it: the release asset
   `tickrdisplay-<version>.bin` you flashed before (do not delete downloaded releases), or a dump
   of the running slot – `http://<device-ip>/api/system/partition/<running slot>` in the address
   bar, any user name + token (3a). The second update overwrites what the first left in the other
   slot – if that was the stock firmware, back it up first (section 3).
4. **Upload.** Choose `tickrdisplay-<version>.bin` → **Upload & flash** → confirm the dialog
   (*Flash "…" (N bytes) to appX and reboot?*). The page shows *Uploading… N %*, then
   *Update successful (N bytes to appX). Rebooting…* and reloads itself after 15 s. The e-ink
   shows *Updating / keep the power on* and the LED is blue while flash is written; an aborted
   upload restores the previous screen and leaves the running firmware untouched.
   (`GET /update` answers `301` to `/system#firmware`.)
5. **Check that the new image runs.** After the reload the *Firmware* section shows the new
   *Version*, the *Running slot* has flipped to the other slot and the *MD5* changed. Ways to tell
   two images apart, most to least reliable:
   * **Version string** (`firmware.version` in `/api/system/info`, the device sheet, the beacon).
     It is the build's `git describe --tags --always --dirty --match "v*"` unless the build set
     `FIRMWARE_VERSION` itself: `v0.1.0` = exactly the tagged release (what CI publishes as a
     release asset); `v0.1.0-4-g1a2b3c4` = 4 commits after the tag, `g` + the commit's short SHA;
     a trailing `-dirty` = built from a working copy with uncommitted changes; `0.0.0-g<sha>` = no
     reachable tag. Two builds of the same commit share the version but can differ in content
     when either was `-dirty`.
   * **`sketch_md5`** – the MD5 of the running image; differs for any two different builds.
   * **Running slot** – flips on every update; the previous image is in the other one.
   * **`firmware.build`** – the compile date and time of the sketch.
   * **What the API answers** – the current firmware reports the Wi-Fi link counters
     `wifi_disconnects`, `wifi_last_reason`, `wifi_down_s`, `wifi_reconnects`, `wifi_restarts` in
     `GET /api/status` and announces the DHCP hostname `<device-name>-XXXXXX`
     ([`DEVICE_UI.md`](DEVICE_UI.md) → *Wi-Fi link supervision*); an image without them is an
     older one.
6. **Going back.** *Firmware* → *Recovery* → **Boot other partition (appX)** → confirm: nothing
   is flashed, the device restarts from the other slot (the previous TickrDisplay – or the stock
   firmware, if that is what the other slot holds; the button says which slot and is disabled when
   the other slot holds no bootable image). If the previous image is no longer on the device,
   upload its saved file (step 3) exactly like a new one (step 4). Without a token or network:
   recovery mode (5a).

**From a URL:** `POST /api/system/update_from_url` with `{"url":"http://host/firmware.bin"}` makes
the device fetch and flash the image itself (plain `http://` only).

**Several devices at once:** on the shelf (`/`) the *...* drawer has an **Updates** card – *Update all
members* uploads one `.bin` to every online USB member of the group in turn, this device last (its
reboot ends the page, which reloads after 15 s), with per-device progress. Sleeping battery members
get the URL you enter there parked on a relay and fetch it on their next wake-up (`update_from_url`).

> **Unverified:** Group OTA via the Updates card has not been exercised on hardware.

**API token.** If an *API Token* is set (`/system` → *Security*, or the shelf's *Protect this device*
card), every OTA / system endpoint (including `POST /update`) requires it: HTTP Basic auth with any
user name and the token as password or the header `X-Api-Token: <token>`. The pages send the token
the browser stored; a download typed into the address bar gets the browser's user-name/password
prompt; the command-line tools read it from the environment variable `TICKR_API_TOKEN`
([section 7](#7-command-line-tools-optional)). Without a token the endpoints are open to the whole
LAN – set one before relying on the device.

The image is written to the OTA slot that is *not* currently running (`next_update` in
`/api/system/info`), verified, made the boot partition, and the device restarts ~1.5 s after the
response. **This overwrites whatever was in that slot** – if the stock firmware was still there, it
is gone after the second TickrDisplay update (diagram in section 1). Back it up first (section 3);
you can always restore it later (5b). Every upload is checked for the `0xE9` image magic and the
slot size before erasing anything; a failed or interrupted upload leaves the current firmware untouched.

### 4b. LAN push-OTA (ArduinoOTA) – `tickr_dev` build only

> **Not in the release image.** The `tickr` image is built without ArduinoOTA and mDNS
> (`TICKR_ARDUINO_OTA` off): port 3232 is closed and there is no `tickrdisplay-xxxxxx.local`.
> `POST /update` (4a) is the supported update path for everyone else.

A `tickr_dev` image runs
[ArduinoOTA](https://github.com/espressif/arduino-esp32/tree/master/libraries/ArduinoOTA) on
**TCP port 3232** and announces itself over mDNS as `tickrdisplay-xxxxxx.local` (`_arduino._tcp`,
`xxxxxx` = last three bytes of the MAC) – the protocol PlatformIO's `espota` uploader speaks:

```sh
cd tickr_display
pio run -e tickr_dev -t upload --upload-port <device-ip> --upload-flag --auth=<ota_password>
# or, without PlatformIO (espota.py ships with the Arduino-ESP32 core):
python3 espota.py -i <device-ip> -p 3232 -a <ota_password> -f .pio/build/tickr_dev/firmware.bin
```

The blue LED is on while writing; the device reboots into the new slot and the previous image stays
in the other slot exactly as with `POST /update`. The stock firmware has no such listener, so the
very first flash always goes through the `TickrMeter` AP (2a) or UART (2b).

**Preconditions – all must hold, otherwise ArduinoOTA is not started (the boot log says why):**

1. The device runs a `tickr_dev` image (`/api/system/info` → `arduino_ota.built = true`).
2. **`ota_password` is set** on `/system` → *Advanced* (applied on the next boot); plain password or
   its 32-hex-digit MD5. An empty password means *no* push-OTA – the firmware refuses to expose an
   unauthenticated flash-write service.
3. **USB power** (on battery the device wakes, refreshes and sleeps within seconds).
4. Wi-Fi connected in STA mode.

`/api/system/info` → `arduino_ota.{enabled,hostname,port}` shows whether the service is up. The
challenge/response authenticates, but the image travels in clear text over the LAN – never expose
port 3232 to the internet. Only one flash writer at a time (`409` against a running `POST /update`
and vice versa); the same slot-size limit applies (`espota` reports `OTA_BEGIN_ERROR`).

## 5. Going back to the stock firmware

Three options, from least to most invasive.

### 5a. Switch the boot slot (stock still in the other partition)

If the stock image is still in the other slot (3a tells how to recognise it), press **Boot other
partition (appX)** under `/system` → *Firmware* → *Recovery* and confirm. The button names the slot
and is disabled when the other slot holds no bootable image; *Boot* in the partition table on `/dev`
does the same on `tickr_dev` builds. From a terminal:

```sh
curl -u ":<token>" -X POST http://<device-ip>/api/system/boot_partition \
     -H 'Content-Type: application/json' -d '{"label":"app0"}'
```

TickrDisplay checks that the partition is an
app slot with a valid image (`0xE9` magic, application descriptor, `esp_image_verify()`), sets it
as boot partition and reboots. The stock firmware comes up, opens its `TickrMeter` portal if it
cannot connect, and TickrDisplay stays in the other slot: you can come back the same way you came –
stock `http://192.168.4.1/update` and upload `firmware.bin` again.

**Without the API token (or without any LAN access): recovery mode.** Switch the device off and on
three times, each start within 20 s of the previous one (the e-ink counts along), join the open
access point **`TickrDisplay`**, open `http://192.168.244.1/` and press **Boot previous firmware** in
the *Recovery* section – the same validated slot switch as above, but accepted only from that access
point, and shown only when the other slot holds a valid image. Coming back from stock is still the
stock portal's `/update`. Recovery mode is described in
[README → *Recovery mode*](../README.md#lost-your-token-or-wi-fi-recovery-mode),
[`WEB_UI.md` → *Recovery mode*](WEB_UI.md#recovery-mode) and [`SECURITY.md`](../SECURITY.md).

Caveat: the slot switch ignores the `otadata` state of the target – a slot the bootloader marked
`invalid` or `aborted` earlier is selected anyway (`ota_state` in `/api/system/info` shows it). This
only matters if the stock image itself is broken.

### 5b. Upload the vendor image through TickrDisplay's `/system#firmware`

The vendor publishes recovery images (`recoveryfw.bin`, also referred to as `FW-45.bin`) at
`https://api.tickrmeter.io/recoveryfw.bin`; see "How to force firmware update locally" on
help.tickrmeter.com. It is a normal ESP32 app image and fits in an OTA slot: download it, then
upload it on `/system#firmware` → **Upload & flash** exactly like a TickrDisplay image (4a), or
from a terminal ([section 7](#7-command-line-tools-optional)).

This works even if the stock slot had already been overwritten by a TickrDisplay update. A slot
dump from section 3 (`app0_0x010000.bin`) can be uploaded the same way.

### 5c. UART: write the full backup back

Requires the internal header (2b) and the `stock_full.bin` made before the first UART flash:

```sh
esptool.py --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 stock_full.bin
```

This is the only way to recover a device that no longer boots to any web interface, and the only
way to restore the *original* bootloader/partition table/NVS after `pio run -t upload`.

## 6. Risks and gotchas

* **Power loss during a write.** Always update on USB power. An interrupted *OTA* upload is harmless
  (the running slot is untouched and the half-written slot is never selected). An interrupted *UART*
  `write_flash` of the bootloader or partition table bricks the device until re-flashed over UART.
* **Stock portal timeout.** The stock WiFiManager portal closes after a few minutes of inactivity;
  upload immediately after joining the `TickrMeter` AP, otherwise power-cycle and retry.
* **Loss of the `spiffs` partition contents.** TickrDisplay keeps `/config.json` on LittleFS in the
  partition labelled `spiffs`; the stock firmware uses SPIFFS there. On the *first boot* the mount
  fails and the partition is **formatted**, before any backup can be taken. If you care about the
  vendor's SPIFFS content, make the UART full-flash dump (2b) *before* flashing. NVS and the stock
  app slot are not touched.
* **Stock slot overwritten by the second update** (section 4) – back it up (section 3) or re-upload
  it later (5b).
* **The partition table never changes over OTA.** A device flashed over Wi-Fi keeps the stock 4 MB
  layout (two 1.25 MB app slots, 1.375 MB `spiffs`); `pio run -t upload` over UART writes the same
  layout. The chip itself is 8 MB, but with the stock bootloader `esp_flash` is limited to 4 MB, so
  `/api/system/flash` cannot dump beyond `0x400000` (`flash.image_header_size` = 4 MB vs
  `flash.chip_size_jedec` = 8 MB in `/api/system/info`): a full-flash download announces 8 MB and
  stops after 4 MB – expected. Backups and `write_flash` addresses are only valid for the table they
  were taken from.
* **Image size limit.** The OTA slot on the stock table is **1 310 720 bytes**. `/update` refuses
  larger images before erasing anything; the *Firmware* page and the command-line tools check the
  size before uploading.
* **Rollback bookkeeping.** Arduino-ESP32 is built with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a
  freshly selected slot boots as `pending_verify` and the firmware marks itself `valid` on start; an
  image that crashes before that point is rolled back to the previous slot on the next reset (so is
  a non-Arduino image that never confirms itself).
* **Credentials.** Both firmwares let the ESP32 Wi-Fi driver store the last SSID/password in the
  shared `nvs` partition (same `WiFi.begin(ssid, pass)` record), so switching slots usually keeps the
  device on your network; if the "other" firmware cannot connect it opens its own portal
  (`TickrMeter` for stock, `TickrDisplay` for this firmware). TickrDisplay never modifies the vendor
  device identity in NVS.

## 7. Command-line tools (optional)

Everything above works from the browser. The tools below do the same over HTTP for scripts,
several devices or a headless machine; none of them is needed for a normal update. They live in
`scripts/` at the repository root. If an API token is set, put it into the environment variable
`TICKR_API_TOKEN` first – the scripts send it as HTTP Basic auth (any user, password = token).

**Upload a firmware image** (TickrDisplay `POST /update`, or the stock portal's `POST /u` at
`192.168.4.1` – detected automatically, or forced with the flag):

```sh
# macOS / Linux (bash, curl)
export TICKR_API_TOKEN=<token>
scripts/flash_ota.sh <device-ip> tickrdisplay-<version>.bin            # TickrDisplay
scripts/flash_ota.sh 192.168.4.1 tickrdisplay-<version>.bin --stock    # stock portal
```

```powershell
# Windows (PowerShell 5.1 or 7, no extra tools)
$env:TICKR_API_TOKEN = "<token>"
.\scripts\flash_ota.ps1 <device-ip> .\tickrdisplay-<version>.bin        # TickrDisplay
.\scripts\flash_ota.ps1 192.168.4.1 .\tickrdisplay-<version>.bin -Stock  # stock portal
```

Both check the `0xE9` image magic and the slot size, upload the file as multipart field `update`,
print the device's answer and exit non-zero on failure. The bare request behind them, with `curl`
(`curl.exe` ships with Windows 10 and later):

```sh
curl -u ":<token>" -F "update=@tickrdisplay-<version>.bin" http://<device-ip>/update
```

> **Unverified:** `scripts/flash_ota.ps1` has not been run on a Windows machine.

**Back up every partition** (`nvs`, `otadata`, `app0`, `app1`, `spiffs`, `coredump`, …) into a
folder with `SHA256SUMS` and a table of the app slots that says which one is the stock image:

```sh
scripts/backup_device.sh <device-ip>            # macOS / Linux -> backup_<date>/
```

Single downloads (`-J -O` keeps the server-supplied file name `<label>_0x<address>.bin`):

```sh
curl -u ":<token>" -fSJO http://<device-ip>/api/system/partition/app0
curl -u ":<token>" -fSJO http://<device-ip>/api/system/partition/nvs
curl -u ":<token>" -fSJO http://<device-ip>/api/system/partition/otadata
# raw flash, e.g. the whole 4 MB (bootloader + partition table + everything):
curl -u ":<token>" -fSJO "http://<device-ip>/api/system/flash?offset=0&length=0x400000"
```

`esptool.py image_info app0_0x010000.bin` prints a valid header for a dumped app slot. The boot-slot
switch from a terminal is the `curl` in 5a.

## 8. Stock partition table

Standard Arduino-ESP32 `default.csv`, 4 MB, read from a flash dump of a TickrMeter (table at `0x8000`)
and from the vendor recovery image header (`Flash size: 4MB`); how to take such a dump yourself is in
[`HARDWARE.md` → *Stock firmware*](HARDWARE.md#stock-firmware).

| Label      | Type | Subtype   | Offset     | Size       | Contents on a stock device                          |
|------------|------|-----------|------------|------------|-----------------------------------------------------|
| (bootloader) | -  | -         | `0x001000` | -          | 2nd stage bootloader, configured for 4 MB           |
| (partition table) | - | -      | `0x008000` | `0x001000` |                                                     |
| `nvs`      | data | nvs       | `0x009000` | `0x005000` | vendor device id, Wi-Fi credentials, PHY calibration |
| `otadata`  | data | ota       | `0x00E000` | `0x002000` | which app slot boots                                |
| `app0`     | app  | ota_0     | `0x010000` | `0x140000` | stock firmware (usually)                            |
| `app1`     | app  | ota_1     | `0x150000` | `0x140000` | empty / OTA target                                  |
| `spiffs`   | data | spiffs    | `0x290000` | `0x160000` | stock SPIFFS → TickrDisplay LittleFS                |
| `coredump` | data | coredump  | `0x3F0000` | `0x010000` | (absent on board rev B)                             |

Which slot the stock firmware occupies depends on the device's update history; check
`/api/system/info` rather than assuming `app0` (how to tell the images apart: 3a).

## 9. HTTP API

The system, OTA and partition endpoints used in this document (`POST /update`, `/api/system/info`,
`update_from_url`, `boot_partition`, `partition/<label>`, `flash`, `restart`, the Wi-Fi and recovery
routes) are specified in [`API.md` → *System, OTA, partitions*](API.md#system-ota-partitions) and
[*Wi-Fi and recovery*](API.md#wi-fi-and-recovery).
