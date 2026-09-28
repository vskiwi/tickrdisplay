# Flashing, updating and recovering a TickrMeter

How to get TickrDisplay onto a TickrMeter, back up the stock firmware, update TickrDisplay later
and go back to the vendor firmware. Pin-outs and header locations are in [`HARDWARE.md`](HARDWARE.md);
the endpoints used below are specified in [`API.md`](API.md).

**Read the whole document before flashing.** The device has no USB-serial bridge (USB-C is power
only), so the *only* way to flash without opening the case is over Wi-Fi, and the only way to fix
a device that no longer boots to a web page is the internal UART header.

## 1. How the two firmwares coexist

The stock firmware is a plain Arduino-ESP32 application built with the standard Arduino
`default.csv` partition layout (two 1.25 MB OTA slots `app0`/`app1`, [section 7](#7-stock-partition-table)).
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
`curl` or a browser, and a way to make your normal Wi-Fi *unavailable* to the device.

1. Make the device's known Wi-Fi network unreachable (turn off the access point, change its
   password temporarily, or move the device out of range). The stock firmware only opens its
   setup portal when it cannot connect.
2. Power-cycle the device with the switch on the back (off, wait 2 s, on).
3. After ~10–30 s an open access point named **`TickrMeter`** appears. Join it.
4. Open **`http://192.168.4.1/update`** in a *regular browser tab*. Do **not** use the captive
   portal pop-up that your OS may show – file uploads do not work from there (WiFiManager itself
   prints this warning). If the pop-up opens, close it and type the URL manually.
5. Choose `firmware.bin` and press **Update**. The page shows no progress; the upload takes
   1–3 minutes. The stock portal answers "Update successful. Device rebooting now..." and the
   device restarts. Same thing from the command line (field name `update`, URL `/u`):

   ```sh
   scripts/flash_ota.sh 192.168.4.1 tickr_display/.pio/build/tickr/firmware.bin --stock
   ```

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

### 3a. One command

```sh
scripts/backup_device.sh <device-ip>            # -> backup_<date>/
```

The script fetches `/api/system/info`, downloads every partition (`nvs`, `otadata`, `app0`, `app1`,
`spiffs`, `coredump`, ...) as `<label>_0x<address>.bin`, writes `SHA256SUMS`, and prints a table of
the app slots, e.g.

```
    LABEL    ADDRESS    SIZE      RUNNING  BOOT  OTA_STATE       IMAGE
    app0     0x010000   1310720   -        -     valid           arduino-lib-builder v4.4.5 Jun 12 2023
    app1     0x150000   1310720   RUNNING  BOOT  valid           arduino-lib-builder v4.4.7-dirty Mar  5 2024
```

The slot that is *not* RUNNING and reports IDF **v4.4.5 / Jun 12 2023** is the stock firmware (both
images call themselves `arduino-lib-builder`; TickrDisplay is IDF 4.4.7 and the descriptor date is
the Arduino core's build date, not the sketch's). If an API token is set, `export TICKR_API_TOKEN=<token>` first.

### 3b. Manually

On a **`tickr_dev` build** `http://<device-ip>/dev` (Diagnostics) has a **Download** link per
partition (the page opens read-only without a token, the downloads need it). The release image
has no `/dev` page, so there use curl (`-J -O` keeps the server-supplied file name):

```sh
curl -fSJO http://<device-ip>/api/system/partition/app0
curl -fSJO http://<device-ip>/api/system/partition/nvs
curl -fSJO http://<device-ip>/api/system/partition/otadata
# raw flash, e.g. the whole 4 MB (bootloader + partition table + everything):
curl -fSJO "http://<device-ip>/api/system/flash?offset=0&length=0x400000"
```

A dumped app slot is a plain ESP32 application image: `esptool.py image_info app0_0x010000.bin`
prints a valid header; the file can later be uploaded through `/system#firmware` or written back
with `esptool.py write_flash 0x10000 app0_0x010000.bin` (address = the one in the file name). The
dump is the full 1.25 MB slot; the trailing `0xFF` padding is harmless.

## 4. Updating TickrDisplay

Two paths, both over the normal home network (no AP mode needed): HTTP upload to `POST /update`
from the *Firmware* section of `/system` or from the shell (4a, every build), and ArduinoOTA push
from PlatformIO / `espota.py` (4b, **developer build `tickr_dev` only**).

### 4a. HTTP upload (`/system#firmware`, `POST /update`)

Browser: `http://<device-ip>/system#firmware` → choose the new `firmware.bin` → **Upload & flash**.
The page shows upload progress, the result, and reloads after the reboot. The e-ink shows
*Updating / keep the power on* and the LED is blue while flash is being written; an aborted upload
restores the previous screen. (`GET /update` answers `301` to `/system#firmware`.)

```sh
scripts/flash_ota.sh <device-ip> tickr_display/.pio/build/tickr/firmware.bin
# or plain curl:
curl -F "update=@tickr_display/.pio/build/tickr/firmware.bin" http://<device-ip>/update
```

**From a URL:** `POST /api/system/update_from_url` with `{"url":"http://host/firmware.bin"}` makes
the device fetch and flash the image itself (plain `http://` only).

**Several devices at once:** on the shelf (`/`) the *...* drawer has an **Updates** card – *Update all
members* uploads one `.bin` to every online USB member of the group in turn, this device last (its
reboot ends the page, which reloads after 15 s), with per-device progress. Sleeping battery members
get the URL you enter there parked on a relay and fetch it on their next wake-up (`update_from_url`).

> **Unverified:** Group OTA via the Updates card has not been exercised on hardware.

**API token.** If an *API Token* is set (`/system` → *Security*, or the shelf's *Protect this device*
card), every OTA / system endpoint (including `POST /update`) requires it: HTTP Basic auth with any
user name and the token as password (`curl -u :<token> ...`) or the header `X-Api-Token: <token>`.
The pages send the token the browser stored; `scripts/flash_ota.sh` and `scripts/backup_device.sh`
read it from the environment variable `TICKR_API_TOKEN`. Without a token the endpoints are open to
the whole LAN – set one before relying on the device.

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

If `/api/system/info` (or the partition table on `/dev` of a `tickr_dev` build) shows the stock image
in the other slot:

```sh
curl -X POST http://<device-ip>/api/system/boot_partition \
     -H 'Content-Type: application/json' -d '{"label":"app0"}'
```

or press **Boot other partition** under `/system` → *Firmware* → *Recovery* (or *Boot* in the
partition table on `/dev`, `tickr_dev` builds only). TickrDisplay checks that the partition is an
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
help.tickrmeter.com. It is a normal 1 268 912-byte ESP32 app image and fits in an OTA slot:

```sh
curl -fLO https://api.tickrmeter.io/recoveryfw.bin
scripts/flash_ota.sh <device-ip> recoveryfw.bin
```

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
  larger images before erasing anything; `flash_ota.sh` warns as well.
* **Rollback bookkeeping.** Arduino-ESP32 is built with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a
  freshly selected slot boots as `pending_verify` and the firmware marks itself `valid` on start; an
  image that crashes before that point is rolled back to the previous slot on the next reset (so is
  a non-Arduino image that never confirms itself).
* **Credentials.** Both firmwares let the ESP32 Wi-Fi driver store the last SSID/password in the
  shared `nvs` partition (same `WiFi.begin(ssid, pass)` record), so switching slots usually keeps the
  device on your network; if the "other" firmware cannot connect it opens its own portal
  (`TickrMeter` for stock, `TickrDisplay` for this firmware). TickrDisplay never modifies the vendor
  device identity in NVS.

## 7. Stock partition table

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

## 8. HTTP API

The system, OTA and partition endpoints used in this document (`POST /update`, `/api/system/info`,
`update_from_url`, `boot_partition`, `partition/<label>`, `flash`, `restart`, the Wi-Fi and recovery
routes) are specified in [`API.md` → *System, OTA, partitions*](API.md#system-ota-partitions) and
[*Wi-Fi and recovery*](API.md#wi-fi-and-recovery).
