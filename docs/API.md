# TickrDisplay API reference

Everything the device accepts over the network: the JSON payload, the HTTP
routes on port 80, MQTT and the UDP discovery beacon. Behaviour is described
as implemented in `tickr_display/src`; the web pages (`/`, `/system`, `/dev`)
are clients of this API and need nothing else.

## Authentication

* Without a configured API token the API is **open**. Set one on `/system` →
  *Security* (or with *Protect this device* on the shelf).
* With a token, a protected route answers `401 {"error":"unauthorized"}`
  unless the request carries **HTTP Basic** credentials (any user name,
  password = token – `curl -u :<token>`) or the header
  **`X-Api-Token: <token>`**. The comparison is constant-time.
* `WWW-Authenticate: Basic` is sent only to *navigations* (`Accept:
  text/html` – address bar, link, plain form), never to `fetch`/XHR or
  `curl`, so the pages never trigger the browser's Basic prompt.
* **Group routes** (pairing, group, layout writes, peers writes, relay
  parking) additionally refuse to work on a device without a token:
  `403 {"error":"set an API token on /system first"}`.
* **Public** routes answer without a token; they expose no secrets. Routes
  marked **captive** are open only to clients of the device's own access
  point (set-up portal or recovery mode) and answer `403` to everybody else,
  token or not. **Signed** routes are device-to-device and authenticate with
  the group secret (see [Relay signature](#relay-signature)).
* Every `/api/*` route sends `Access-Control-Allow-Origin: *`; `OPTIONS`
  preflights on `/api/*`, `/update` and `/config` answer `204`, so a page
  served by one device can call every device on the LAN from the browser.

## Payload format

The same JSON document is accepted from `POST /api/screen`, from the MQTT
topic, from the Pull URL and from a relay. ≤ **4096 bytes**, standard JSON
(no comments), must be an object with at least one recognised field;
unknown fields are ignored. The document is validated completely before
anything is drawn or played – an invalid one is rejected and nothing changes.

```json
{"title":"Bitcoin","value":"$95,240","change":"+0.07%","dir":1,"age_s":120,
 "spark":[83950,83990,84001],"alert":{"led":"00FF00","sound":"double_beep","volume":128}}
```

| Field | Type | Description |
|-------|------|-------------|
| `title` | string or number | Heading line, ≤ 63 chars |
| `value` | string or number | Main text, ≤ 127 chars. The screen is redrawn when `title` and/or `value` is present (a missing one is drawn empty) |
| `alert.led` | string | LED colour `RRGGBB`, optional leading `#` (`000000` = off). Kept until the next payload changes it |
| `alert.sound` | string | Preset `beep` (2 kHz, 100 ms), `double_beep`, `long_beep` (500 ms), `none`, or an **RTTTL** melody: ≤ 1024 chars, ≤ 512 notes, `d` ∈ {1,2,4,8,16,32}, `o` 4–7, `b` 25–900, total ≤ 15 s. An unknown preset or invalid melody is ignored with a `warning` in the response; the rest of the payload is applied |
| `alert.volume` | integer 0–255 | DAC amplitude for this and later sounds. Default 255 |
| `change` | string (≤ 15 chars) or number | Printed as given (`"+0.07%"`); **selects the ticker layout**. `""` is recognised but draws nothing |
| `dir` | `-1` · `0` · `1` or `"down"` · `"flat"` · `"up"` | Triangle direction and LED rule; derived from the sign of `change` when absent |
| `age_s` | integer ≥ 0 | Age of the quote when sent; the device adds the time since and shows *just now* / `N min ago` / `N h ago`, or *stale* once older than 3 × the refresh interval (min 10 min) |
| `time` | string ≤ 15 chars | Shown verbatim instead of the relative age |
| `spark` | array of numbers | Sparkline; the first 48 numeric elements are kept, non-numbers skipped. **Selects the ticker layout** |

Fields are applied in the order volume → LED (or the device's LED rule) →
sound → screen. Payloads are queued (depth 8) and executed from the main
loop; an identical rendered frame is not redrawn (see
[`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules*).

## HTTP routes

Commands that go through the queue answer `202 {"status":"queued","queue":N}`,
or `503` when the queue is full. Error bodies are `{"error":"…"}`.

### Pages

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/` | public | The shelf (`panel.html`, gzip). Serves the Wi-Fi page instead to captive clients |
| `GET` | `/system` | public | Settings, firmware, power, security, about (`system.html`); also served in set-up mode |
| `GET` | `/wifi` | public | Wi-Fi set-up page with the *Recovery* section |
| `GET` | `/dev` | public | Diagnostics page – **`tickr_dev` builds only** (`/api/system/info.dev_page`); the release image answers `404` |
| `GET` | `/panel`, `/group`, `/config`, `/update` | – | `301` to `/`, `/#group`, `/system`, `/system#firmware` |

Pages are read-only without a token and show an inline token field; the
browser keeps the token in `localStorage`.

### Screen

| Method | Path | Auth | Description |
|---|---|---|---|
| `POST` | `/api/screen` | token | Body = payload (`Content-Length` required; `411` without, `413` above 4096 B, `400` invalid, `202` queued) |
| `GET` | `/api/screen/raw` | public | The frame on the e-ink: 4736 bytes, 296×128, 1 bit/pixel, rows top-down, MSB first, 1 = black. Headers `X-Screen-Width`, `X-Screen-Height`, `X-Screen-Format: 1bpp-msb`, `ETag` (+ `If-None-Match` → `304`). `503` + `Retry-After` while a pairing code is on the screen or heap is low |
| `GET` | `/api/screen.bmp` | public | The same frame as a 1-bit BMP (`Content-Disposition: inline`); same headers and `503` rules |
| `GET` | `/api/screen/state` | public | What is shown – see [Screen state fields](#screen-state-fields) |
| `POST` | `/api/screen/identify` | token | `{"n":2,"ttl_s":30}` (≤ 96 B): show `n` (1–99) large with the device name for `ttl_s` s (1–600), then return to the content; `{"n":0}` returns at once. `409` while a pairing code is shown |

`POST /api/screen/raw` (push a ready bitmap) is *not implemented*.

### Status and configuration

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/api/status` | public | `uptime_s`, `heap_free`, `heap_min_free`, `rssi`, `ip`, `power` (`usb`·`battery`·`unknown`), `power_mode` (override `auto`·`usb`·`battery`), `board` (`A`·`B`·`?`), `vsys_v` (cell) and `vin_v` (rail) – `null` on board `?`, `pull_https`, `tls_ca`, `mqtt_enabled`, `mqtt_connected`, `mqtt_state`, `queue_depth`, `queue_dropped`, `auth_enabled`, `tls_insecure_used`, `reset_reason`, `last_error`, `wifi_disconnects` (losses of a link that had an address), `wifi_last_reason` (driver reason code of the last one, `wifi_err_reason_t`; `0` = the address was lost while still associated, or none yet), `wifi_down_s` (seconds since the current outage began, `0` when up), `wifi_reconnects` (outages ended after the firmware's own reconnect), `wifi_restarts` (restarts after 30 min without a link) – counters kept across software restarts and deep sleep, zero after a power-on ([`DEVICE_UI.md`](DEVICE_UI.md) → *Wi-Fi link supervision*), `led {r,g,b}` (the colour actually driven, overlays included), `peers {count, fresh, members, sent, send_failed, received, verified, dropped, period_s, broadcast, running}` |
| `GET` | `/api/config` | token | Settings without secrets: `mqtt_server`, `mqtt_port`, `mqtt_topic`, `mqtt_user`, `pull_url`, `refresh_interval` (min, 1–1440), `adc_cell_num`, `adc_cell_den`, `led_rule` (`off`·`sign`), `source_kind`, `tk_preset` (`coingecko`·`kraken`·`binance`·`binance_usdm`·`binance_coinm`·`custom`), `tk_symbol`, `tk_market`, `tk_url`, `tk_price`, `tk_change`, `tk_spark`, `tk_mode`, `tk_decimals` (255 = auto), `tk_sep`, `tk_label`; flags `mqtt_pass_set`, `tls_ca_set`, `api_token_set`, `ota_password_set`, `group_set`, `tk_api_key_set`, `arduino_ota_build` |
| `POST` | `/config` | token | Save settings – form fields named as above plus the secrets `mqtt_pass`, `api_token`, `ota_password`, `tls_ca_pem`, `tk_api_key` (an empty field keeps the stored value; `<field>_clear=1` erases it). Validated as a whole; `400` with the reasons, else `200`. A changed Pull URL, ticker source or interval fetches at once on USB. The power-source override is `POST /api/power/source` |
| `POST` | `/api/source/test` | token | Form fields of a ticker source (`tk_preset`, `tk_symbol`, `tk_market`, `tk_label`, `tk_decimals`, `tk_sep`; `tk_url`, `tk_price`, `tk_change`, `tk_mode`, `tk_spark` for `custom`) – one fetch now, nothing saved (≤ ≈ 20 s; two requests for a futures perpetual): `{"ok":true,"price":"…","change":"…","dir":1,"label":"…","url":"…","ms":812}` – the futures presets add `"funding":"FR +0.0048%"` (`""` when only the funding request failed) – or `{"ok":false,"error":"http 451","url":"…","ms":…}`. `400` invalid source, `409` while a test runs |
| `GET` | `/api/identity` | public | `id` (`tickr-XXXXXX` from the MAC), `name`, `mac`, `ip`, `version`, `power`, `board`, `sleep_s`, `relay` (USB group member), `pairable` (USB power), `group` (`null` or `{id,name,epoch}` – never the secret), `layout {x,y,group}`, `screen {width,height,format}`, `caps` (`screen_raw`, `screen_bmp`, `peers_v1`, `pair_v1`, `layout_v1`, `relay_v1`, `update_url_v1`), `max_peers`, `uptime_s` |
| `POST` | `/api/identity` | token | `{"name":"Kitchen left","layout":{"x":0,"y":0,"group":"shelf"}}` (JSON or form fields `name`, `layout_x`, `layout_y`, `layout_group`): rename / set the shelf slot (`x`,`y` 0–15). The name also becomes the DHCP hostname `<name>-XXXXXX` from the next boot ([`DEVICE_UI.md`](DEVICE_UI.md) → *Wi-Fi link supervision*) |

### Power

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/api/power/raw` | public | Detector diagnostics: `board`, `rule` (`rail`·`diff`·`none`), `sense_en`, raw counts (mean/min/max), calibrated pin mV and divided mV of GPIO 32 (`vsys`, cell) and 33 (`vin`, rail), `diff_mv`, ADC calibration kind, both dividers, thresholds, boot window, `aux[]` (GPIO 34–39). See [`HARDWARE.md`](HARDWARE.md) → *Power sensing* |
| `POST` | `/api/power/source` | token | Form `mode=auto|usb|battery` – override, applied at once and saved |
| `GET`/`POST` | `/api/power/probe`, `/api/power/probe/gpio4` | token | GPIO pull probe – only in builds with `-DTICKR_POWER_PROBE` (off in every environment) |

### Peers, layout, beacon

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/api/peers` | public | Devices heard on the LAN: `[{id, mac, name, ip, version, power, member, stale, manual, last_seen_s, next_wake_s, rssi, epoch}]` |
| `POST` | `/api/peers` | group | `{"ip":"a.b.c.d"}` – probe a device by address (other subnet) → `202 {"status":"probing"}`; `503` when discovery is not running (battery / AP mode) |
| `DELETE` | `/api/peers/<id>` | group | Forget a peer; `404` unknown id |
| `GET` | `/api/layout` | public | The shelf layout document (`/layout.json`) or `404`: `{"v":1,"updated_at":<ms>,"by":"tickr-…","devices":{"tickr-XXXXXX":{"x":0,"y":0,"name":"…"}},"groups":{"<name>":[ids]}}` |
| `PUT`/`POST` | `/api/layout` | group | Store the document: ≤ 4096 B, `v` = 1, `x`/`y` 0–15, ids `tickr-XXXXXX`, names ≤ 31 ASCII chars, ≤ 64 devices, ≤ 8 groups |
| `DELETE` | `/api/layout` | group | Forget the document |
| `GET` | `/api/peers/<id>/screen` | public | Last frame a sleeping member uploaded (same format as `/api/screen/raw`, `X-Screen-Age-S` when known); `404` none |
| `PUT` | `/api/peers/<id>/screen` | signed | A battery member uploads its 4736-byte frame after a refresh (at intervals ≥ 10 min); `400` wrong length |

UDP discovery is described under [UDP beacon](#udp-beacon).

### Relay (content for sleeping members)

A USB-powered group member is a *relay*: it parks a payload or a firmware URL
per sleeping member, which fetches it on its next wake-up.

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/api/relay` | group | `{"payload":[ids],"ota":[ids]}` – members with something parked |
| `PUT` | `/api/relay/<id>` | group | Park a payload (JSON object ≤ 4096 B) → `{"ok":true,"bytes":N}`. `403 "not a relay"` on battery or without a group, `404` not a member, `413` too large |
| `PUT` | `/api/relay/<id>/ota` | group | Park `{"url":"http://host/firmware.bin"}` – flashed on the member's next wake (plain `http://` only) |
| `DELETE` | `/api/relay/<id>`, `/api/relay/<id>/ota` | group | Clear; `404` nothing pending |
| `GET` | `/api/relay/<id>[?consume=1]` | signed | The sleeper's fetch: `200` + payload or `204`; header `X-Tickr-OTA-URL` when an update waits, `X-Tickr-Nonce` for the next request; `consume=1` deletes after delivery |

#### Relay signature

Signed requests carry one header `X-Tickr-Group:
<group_id 16 hex>:<epoch>:<nonce 16 hex>:<64 hex HMAC-SHA-256(group_secret, msg)>`
with `msg = "tickr-relay-v1\n" METHOD "\n" path "\n" nonce "\n" SHA-256(body) as 64 hex`
(path without query string). The nonce is issued by the relay – as `rn` in
its probe reply or in the `X-Tickr-Nonce` header of the previous verified
response – is valid for 30 s and usable once. `401 "bad signature"`, `403
"no group"`.

### Pairing and group

Pairing works on USB power only; the code must be read off the device's
screen. All routes are **group** (403 without a token) unless noted.

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/pair/start` | Form `pk` (base64 X25519 public key), optional `mode=rekey` → `202 {"sid","state":"starting","mode"}`; the screen shows a 6-digit code and a 4-letter check word for 90 s. `403` on battery, `409` session open, `429` + `Retry-After` in cooldown, `503` low heap |
| `GET` | `/api/pair/status` | **public.** `{"state":"idle","pairable":…}` · `{"state":"starting"}` · `{"state":"pairing","sid","pk_d","n_d","chk","expires_s","attempts_left","mode"}` · `{"state":"cooldown","retry_after_s"}` |
| `POST` | `/api/pair/confirm` | Form `sid`, `ci` (HMAC proof of the code), and `box` (wrapped credentials to join / re-key) or `name` (create) → `200 {"c_d","group"}`; `401 {"error":"wrong code","attempts_left"}`, `429` after the third |
| `POST` / `DELETE` | `/api/pair/cancel` / `/api/pair` | Cancel; the screen shows *Pairing cancelled* for 5 s |
| `GET` | `/api/group` | `{id, name, epoch, members}` or `404` |
| `GET` | `/api/group/secret?pk=…` | Credentials wrapped for the given public key: `202 {"status":"computing","retry_after_ms":300}` first, then `{"pk_a","n_a","box"}` |
| `POST` | `/api/group/leave` | Leave the group |
| `POST` | `/api/group/rekey` | New secret, `epoch + 1` on this device; the panel re-keys the other USB members |

Protocol details: [`MULTI_DEVICE.md`](MULTI_DEVICE.md).

### Wi-Fi and recovery

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/api/wifi/status` | public | `portal` (set-up AP up), `captive` (this client may connect/forget), `state` (`idle`·`connecting`·`connected`·`failed`), `ssid`, `ip`, `rssi`, `saved`, `error` |
| `GET` | `/api/wifi/scan` | captive or token | `202 {"status":"scanning"}` while running, then `{"networks":[{ssid,rssi,secure}]}` strongest first; `?rescan=1` |
| `POST` | `/api/wifi/connect` | captive | Form `ssid`, `pass` → `202`; credentials stored in NVS |
| `POST` | `/api/wifi/forget` | captive | Erase the saved credentials and disconnect |
| `GET` | `/api/recovery/status` | captive or token | `{active, via_ap, ap_ip, clients, remaining_s, token_set, group, other_slot}` |
| `POST` | `/api/recovery/token` | captive, recovery on | `mode=new` → `{"ok":true,"token":"<32 hex>"}` (shown once) or `mode=clear` |
| `POST` | `/api/recovery/group/leave` | captive, recovery on | Leave the group without the token; `404` no group |
| `POST` | `/api/recovery/factory` | captive, recovery on | Delete `config.json`, `layout.json`, `peers.bin`, `ca.pem` and restart; form `wifi=1` also erases the Wi-Fi credentials |
| `POST` | `/api/recovery/boot/previous` | captive, recovery on | Boot the other app slot if it holds a valid image → `{"ok":true,"boot":"app0"}`; `409` otherwise |

Recovery `POST`s answer `403 "recovery mode: only through the TickrDisplay
access point"` from the LAN, token or not. The device's access point is
`192.168.244.1`. How to enter recovery mode: README → *Recovery mode*.

### System, OTA, partitions

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/api/system/info` | public | `firmware {name, version, build, sdk, sketch_size, …}`, `chip {model, revision, cores, mac}`, `board {revision, battery_measurable}`, `flash {chip_size, jedec_id, chip_size_jedec, image_header_size}`, `memory`, `reset_reason`, `uptime_ms`, `update_in_progress`, `dev_page`, `arduino_ota {built, enabled, hostname, port}`, `wifi {ip, rssi, hostname}` (the DHCP hostname in use, `<device-name>-XXXXXX`), `running` / `boot` / `next_update` and `partitions[]` (`label`, `type`, `subtype_str`, `address`, `size`; app slots add `running`, `boot`, `bootable`, `ota_state`, `description {project_name, version, idf_ver, date}`) |
| `POST` | `/update` (alias `/u`) | token | Multipart firmware upload, field `update` → inactive OTA slot, `{"ok":true,"target":"app1","written":N}`, restart after 1.5 s. `409` another upload running / no file |
| `POST` | `/api/system/update_from_url` | token | `{"url":"http://host/firmware.bin"}` → `202 {"status":"scheduled"}`; the device fetches and flashes the image itself. Plain `http://` only (`400` for `https://`), `409` while another update runs |
| `POST` | `/api/system/boot_partition` | token | `{"label":"app0","restart":true}` or `?label=app0` – validated slot switch (return to stock); restart after 1.5 s unless `restart:false` |
| `GET` | `/api/system/partition/<label>` | token | Stream a raw partition (`Content-Disposition: attachment`, `X-Partition-Address/Type/Subtype`) |
| `GET` | `/api/system/flash?offset=&length=` | token | Stream raw flash (decimal or `0x` hex; `length` defaults to the end of the bootloader-configured size) |
| `POST` | `/api/system/restart` | token | `{"ok":true}`, restart after 0.5 s |

While an upload is being written, downloads, slot changes and restarts
answer `409`. Flashing procedures: [`FLASHING.md`](FLASHING.md).

### Test and development routes

`POST`, token: `/api/test/led_red_pwm?val=0..255` (also `led_green_pwm`,
`led_blue_pwm`), `/api/test/led_red` (flash 500 ms; also `led_green`,
`led_blue`), `/api/test/volume?val=0..255`, `/api/test/play_rtttl?melody=…`
(URL-encoded, validated), `/api/test/beep` (demo melody),
`/api/test/screen_refresh` ("Screen Test"), `/api/test/screen_pattern`.
Compiled into every build. `POST /api/recovery/enter` exists only with
`-DTICKR_RECOVERY_TEST` (`tickr_dev`). ArduinoOTA (`espota`, TCP 3232,
mDNS `tickrdisplay-xxxxxx.local`) is compiled into `tickr_dev` only and needs
`ota_password` and USB power.

## Screen state fields

`GET /api/screen/state` (public; `ETag` follows `render_seq`):

| Field | Meaning |
|---|---|
| `title`, `value` | The text of the shown content frame |
| `state` | `boot` · `setup` · `waiting` · `content` · `pairing` · `recovery` · `ota` · `identify` · `offline` · `battery_empty` · `power_to_battery` – what is on the panel now |
| `card` | `none` · `offline` · `ota` · `battery_empty` · `power_to_battery` – the condition card that is the base frame, also while a service frame is on top |
| `stale_s` | Seconds since the content was shown; `null` before the first payload |
| `layout` | `text` · `ticker`; a ticker frame adds `change`, `dir` (−1/0/1), `age_s` (total age of the quote) |
| `source`, `symbol` | What the device fetches on its own (`ticker` · `url` · `none`) and the ticker's symbol (`""` otherwise) |
| `power_switch`, `power_grace_s` | Runtime USB → battery switch: `off` · `idle` · `grace` · `blocked` · `restart`, seconds left in the grace |
| `render_seq`, `rendered_at_s` | Frame counter and uptime of the last draw – poll these, fetch `raw` only on a change |
| `refreshes_full`, `refreshes_partial`, `refreshes_skipped` | Full refreshes, differential (partial) refreshes and frames not drawn (folded badge changes, frames identical to the shown one) this boot |
| `last_full_s`, `partials_since_full` | Uptime of the last full refresh (0 = none this boot) and the forced-full counter – see [`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules* |
| `width`, `height` | 296, 128 |
| `status` | `{wifi, bars (0–3), usb, power: usb·battery·unknown, batt_pct, batt_low, ip}` – the badge snapshot |

## MQTT

* Client id `tickrdisplay-XXXXXX` (last three bytes of the MAC), optional
  user name / password, keep-alive 30 s, socket timeout 5 s.
* Subscribes to the configured topic (default `tickr/display`); every message
  is parsed as a payload (≤ 4096 B).
* Status topic `<topic>/status` (wildcard tail stripped, e.g.
  `tickr/display/status`): retained `online` after connecting, retained
  `offline` as Last Will and on clean shutdown.
* Reconnect back-off 5 s → 30 s (doubling). MQTT runs in USB mode only.

## UDP beacon

Every USB-powered device broadcasts one JSON object per datagram on **UDP
port 47000** (≤ 255 bytes); a battery device sends one per wake-up with its
planned sleep. Nothing is sent in access-point mode, and a device only
*records* beacons (`GET /api/peers`) – it never acts on one. The period is
adaptive: 45 s × ⌈N/8⌉, clamped to 45–180 s, ± jitter.

```json
{"t":"tickr","mac":"020000d4e5f6","n":"Kitchen left","ip":"192.0.2.5","v":"0.3.1",
 "pw":"usb","sl":0,"seq":12,"rs":-62,"g":"","ep":0,"tag":""}
```

| Key | Meaning |
|---|---|
| `t` | `tickr` (announce) or `tickr?` (probe) |
| `mac` | 12 hex digits – the identity; `tickr-XXXXXX` is derived from its tail |
| `n`, `ip`, `v` | Name (≤ 15 chars), dotted IPv4, firmware version |
| `pw`, `sl` | `usb` or `bat`; planned sleep in seconds (0 on USB) |
| `seq`, `rs` | Sender's monotonic counter; RSSI (cosmetic) |
| `g`, `ep`, `tag` | Group id (16 hex), epoch and the first 8 bytes of HMAC-SHA-256(group_secret, `"tickr-beacon-v1\n" mac "\n" n "\n" ip "\n" pw "\n" sl "\n" seq "\n" g "\n" ep`) as 16 hex digits; `""`/`0`/`""` without a group |
| `nc` | Nonce echoed in a unicast reply to a probe `{"t":"tickr?","mac":"…","nc":"…"}` |
| `rn` | Relay nonce (16 hex) in a probe reply from a USB group member – one signed relay request |

A beacon with a valid `tag` for this device's group and epoch marks the
sender `member: true` in `/api/peers`. Peers without a beacon for four
periods (or loaded from `/peers.bin` after a reboot) are `stale: true`.
