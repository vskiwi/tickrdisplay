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
A rejected document names its reason (`root must be a JSON object`, `no
recognised fields (…)`, `value: too long`) in the `400` body of `POST
/api/screen`, and in `last_error` of `GET /api/status` for every path: a
rejected Pull URL body is a failed pull (`pull: root must be a JSON object`)
and backs off like an HTTP error – [`TICKERS.md`](TICKERS.md) → *Fetch
schedule and errors*. `POST /api/pull/test` shows the verdict for a URL
before it is saved.

```json
{"title":"Bitcoin","value":"$95,240","change":"+0.07%","dir":1,"age_s":120,
 "spark":[83950,83990,84001],
 "alert":{"led":"00FF00","brightness":128,"sound":"double_beep","volume":128}}
```

| Field | Type | Description |
|-------|------|-------------|
| `title` | string or number | Heading line, ≤ 63 bytes |
| `value` | string or number | Main text, ≤ 127 bytes. The screen is redrawn when `title` and/or `value` is present (a missing one is drawn empty) |
| `alert.led` | string | LED colour `RRGGBB`, optional leading `#` (`000000` = off). Kept until the next payload changes it |
| `alert.brightness` | integer 0–255 | Scales the LED colour – `alert.led` or the colour of the device's LED rule – for this and later payloads; each channel is `value × brightness / 255`. Default 255; `0` keeps the colour but drives the LED dark, so a later non-zero value restores it without resending `led`. Not stored across a restart. Overlays and blinks (OTA, pairing, identify, the end-of-wake blink) are not scaled; in deep sleep `led_battery = hold` holds the full colour regardless of the brightness, and holds nothing at `0` ([`DEVICE_UI.md`](DEVICE_UI.md) → *LED and sound*) |
| `alert.sound` | string | Preset `beep` (2 kHz, 100 ms), `double_beep`, `long_beep` (500 ms), `none`, or an **RTTTL** melody: ≤ 1023 bytes, ≤ 512 notes, `d` ∈ {1,2,4,8,16,32}, `o` 4–7, `b` 25–900, total ≤ 15 s. An unknown preset or invalid melody is ignored with a `warning` in the response; the rest of the payload is applied |
| `alert.volume` | integer 0–255 | DAC amplitude for this and later sounds. Default 255 |
| `change` | string (≤ 15 bytes) or number | Printed as given (`"+0.07%"`); **selects the ticker layout**. `""` is recognised but draws nothing |
| `dir` | `-1` · `0` · `1` or `"down"` · `"flat"` · `"up"` | Triangle direction and LED rule; derived from the sign of `change` when absent |
| `age_s` | number ≥ 0 | Age of the quote when sent; the device adds the time since and shows *just now* / `N min ago` / `N h ago`, or *stale* once older than 3 × the refresh interval (min 10 min) |
| `time` | string (≤ 15 bytes) or number | Shown verbatim instead of the relative age |
| `spark` | array of numbers | Sparkline; the first 48 numeric elements are kept, non-numbers skipped. **Selects the ticker layout** |
| `short` | string ≤ 7 printable ASCII chars (or number) | Name on the badge of a ticker frame (`"BTC"`), white on black top left; absent = no badge, the frame as without it. Does not select the layout by itself |
| `tiles` | array of up to 4 | **Selects a tile layout** – see [Tile layouts](#tile-layouts) |
| `layout` | `single` · `1x1` · `1x2` · `2x1` · `2x2` | The tile preset; declarative (`single` is the default and the only value without `tiles`) |
| `style`, `status` | object, boolean | Tile defaults and the status band – [Tile layouts](#tile-layouts) |

Fields are applied in the order volume → brightness → LED (or the device's
LED rule) → sound → screen. Payloads are queued (depth 8) and executed from the main
loop; an identical rendered frame is not redrawn (see
[`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules*).

### Tile layouts

A payload with `tiles` is drawn as a frame of cells instead of one text or
ticker frame. The sender decides **which** cells are filled and with what;
the device decides **how** – where the elements of a cell go and how large
they are drawn. Worked examples for every preset, a sketch of every look
and the sender guidance are in
[`INTEGRATION_EXAMPLE.md`](INTEGRATION_EXAMPLE.md) → *Several coins at
once: tile layouts*; this section is the reference.

```json
{"layout": "2x2",
 "style": {"look": "stack", "size": "auto", "invert": false},
 "tiles": [
   {"title": "BTC", "value": "80000",  "change": "+15%",   "style": {"invert": true}},
   {"title": "ETH", "value": "4100",   "change": "+12.3%", "dir": 1},
   {"title": "SOL", "value": "185.20", "change": "+10%",   "style": {"look": "row"}},
   null],
 "status": true, "age_s": 20, "title": "Movers", "alert": {"led": "00FF00"}}
```

**Precedence.** `tiles` selects the tile frame and wins over the ticker
look (`change` / `spark`) and the text look (`title` / `value`). Inside a
cell the tile's own `style` overrides the root `style`, which overrides
the defaults (`stack`, `auto`, not inverted); a key left out at both
levels keeps its default. Without `layout` the preset follows the number
of elements in `tiles`: one → `1x1`, two → `1x2`, anything else (none,
three, four) → `2x2`. `layout: "single"` means *no tiles* – it is the
default of every payload without `tiles` and is rejected next to them;
a preset name without `tiles` is rejected too. With `tiles` the root
fields mean:

| Root field next to `tiles` | Effect |
|---|---|
| `title` | The web UI's *Showing* line only (nothing on the panel); absent → the titles of the filled cells joined with `, ` |
| `age_s` | The age line of the status band, as for a ticker (absent → the frame's own age) |
| `alert` | LED, brightness, sound, volume as for any payload; the LED rule `sign` follows the **first** tile's `dir` (explicit, or the sign of its `change`); a first tile without a direction leaves the LED as it was |
| `status`, `style`, `layout` | The status band, the style defaults and the preset – below |
| `value`, `change`, `dir`, `time`, `spark`, `short` | **Ignored.** The payload is applied and the answer carries `"warning": "<field> ignored: tiles present"` naming the first such field (in that order); an `alert.sound` warning takes precedence |

#### Presets

Cells are numbered row-major from the top left; `tiles[i]` fills cell `i`.
A 1 px separator runs at x = 148 (two columns) and / or y = 64 (two rows),
so the right column is 147 px wide and the lower row 63 px high. The
**status band** – the Wi-Fi / power badges and the age line
([`DEVICE_UI.md`](DEVICE_UI.md) → *Screens*) – takes the lowest 16 rows of
the **bottom-right cell**; `"status": false` drops the band and gives that
cell its full height. A cell whose usable height is under 60 px is
**cramped**: the `stack` look draws its bottom row at 9 pt instead of
12 pt. The only cramped cells are the bottom-right cell of a `2x2` and the
lower row of a `2x1`, and only with the band on.

| `layout` | Cells (w × h in px, cell order) | Cell with the band, its usable size | Cramped |
|---|---|---|---|
| `1x1` | 1: 296×128 | the cell, 296×112 | no |
| `1x2` | 2: 148×128 left, 147×128 right | the right one, 147×112 | no |
| `2x1` | 2: 296×64 top, 296×63 bottom | the bottom one, 296×47 | the bottom row |
| `2x2` | 4: 148×64, 147×64, 148×63, 147×63 (TL, TR, BL, BR) | the bottom-right one, 147×47 | the bottom-right cell |

`tiles` may hold at most the preset's cells (`tiles: more than 2 for
layout 1x2`) and never more than four; the cells the array leaves out,
`null` and `{}` are **empty** – white, in the root style (an inverted root
style does not blacken an empty cell: nothing is drawn there). A frame
whose tiles are all empty is a blank frame with its separators and the
band. An element of `tiles` is an object or `null`; any other type is
rejected.

#### Tile fields

| Field | Type | Limit | Default | Effect |
|---|---|---|---|---|
| `title` | string or number | ≤ 15 printable ASCII chars | none | The big text of the cell (`"BTC"`); the badge text in the `ticker` look. Non-ASCII is rejected |
| `value` | string or number | ≤ 23 bytes | none | The second text (`"80000"`), drawn as sent |
| `change` | string or number | ≤ 15 bytes | none | Drawn with its direction triangle; `""` = no change |
| `dir` | `-1` · `0` · `1` or `"down"` · `"flat"` · `"up"` | – | the sign of `change` | The triangle (down / dash / up) and, in the first tile, the LED rule |
| `style` | object | – | the root `style` | Overrides for this cell – below. Unknown keys are ignored |

A tile counts as **filled** when it has a `title`, a `value` or a non-empty
`change`; `{"dir": 1}` or `{"style": {...}}` alone is an empty cell. Any
other key in a tile is ignored. Numbers are rendered as for the root
fields (integers as they are, floats with 6 significant digits).

#### `style`

Root `style` = defaults for every cell; a tile's `style` = overrides for
that cell. Each key is optional at both levels.

| Key | Values | Default | Effect |
|---|---|---|---|
| `look` | `stack` · `row` · `ticker` | `stack` | The placement template of the cell. **`stack`**: title in the upper band, value bottom-left, change with its triangle bottom-right; in a cell of 96 px or more (`1x1`, `1x2`) three bands – title, value, change – one under the other. **`row`**: one line – title left, value and change right-aligned; made for the 296 px rows of a `2x1` and a `1x1`; in a 148 px cell the three share 140 px and the value is the one cut. **`ticker`**: the cell of the built-in 2×2 grid ([`TICKERS.md`](TICKERS.md) → *Several tickers on one panel: the 2×2 grid*) – `title` white on a black badge top-left (no badge without a title), change 9 pt with its triangle right-aligned on the badge row, `value` right-aligned below at 18 pt (whole, else without its fraction) → 12 → 9; always in the top 46 rows of the cell, whatever its height |
| `size` | `auto` · `l` · `m` · `s` | `auto` | The cap of the **title's** font ladder; value and change start one step below it. `auto` and `l`: from 18 pt ×2 (`row`: from 12 pt ×2). `m`: title 18 pt at most, value / change 12 pt at most. `s`: title 12 pt at most, value / change 9 pt |
| `invert` | `true` · `false` | `false` | The cell white on black: the cell is filled black and every element – text, triangle, badge – drawn in white (the badge field becomes white with black text). The status band stays white. Toggling it is a full refresh |

#### Fonts and fitting

The steps are FreeSansBold **18 pt ×2** (52 px capitals), **12 pt ×2**
(36), **18 pt** (26), **12 pt** (18) and **9 pt** (13). Every element gets
a ladder of steps – from its cap down to 9 pt – keeping only the steps
whose capitals fit its band; 9 pt is always the last. The element is drawn
at the **largest step of its ladder whose text fits the width**; a text
that does not fit even at 9 pt is cut with `...`. When elements share one
row (the bottom row of a two-row cell, the whole line of the `row` look),
they are measured together with 8 px gaps and 15 px for the triangle and
shrink in the order **change first, then value, then title**; if the three
of a `row` cell still do not fit at 9 pt, the title keeps up to 3/5 of the
room left of the change and the value is the one cut.

The bands of the `stack` look (`status` on unless said otherwise):

| Cell | Bands | Title ladder | Value / change ladder |
|---|---|---|---|
| 148-px cell of 64 px (`2x2` top cells and bottom-left; every `2x2` cell with `status: false`) | title 38 px; bottom row 20 px (value left, change right) | 12 pt ×2 → 18 → 12 → 9 | 12 → 9 |
| Cramped cell, 47 px (`2x2` bottom-right, `2x1` lower row) | title 26 px; bottom row 13 px, 2 px clear of the band | 18 → 12 → 9 | 9 only |
| `2x1` upper row, 296×64 | as the 148-px cell of 64 px, 288 px wide | 12 pt ×2 → 18 → 12 → 9 | 12 → 9 |
| Tall cell with the band (`1x2` both columns, `1x1`) | three bands from the frame's smallest cell (112 px): title 40 %, value and change the rest, 4 px gaps, 4 px clear of the band – the two columns of a `1x2` align row for row | 12 pt ×2 → 18 → 12 → 9 | 18 → 12 → 9 |
| Tall cell, `status: false` (`1x2`, `1x1`) | title 45 % of 124 px, value and change the rest | **18 pt ×2** → 12 pt ×2 → 18 → 12 → 9 | 18 → 12 → 9 |

The `row` look has one band: the title from 12 pt ×2 (never 18 pt ×2),
value and change from 18 pt. In words, for a sender: in a 148 px cell a
title of **up to 4 capitals** (`DOGE`) stays at 12 pt ×2 and a 5-letter
one drops to 18 pt; on the bottom row of such a cell **5 digits of value
stay at 12 pt next to a 4-character change** (`80000` · `+15%`), a
6-character change (`+12.3%`) pulls both to 9 pt, and **6–7 characters
of value fit at 9 pt** next to `+15%`. In the cramped bottom-right cell of
a `2x2` the row is 9 pt from the start: 6 digits next to `+15%`, 5 next to
`+12.3%`, and a 7-character change (`-12.34%`) leaves room for 5
characters of value. In a `1x2` column the value stands alone: **7 digits
at 18 pt**; a change of up to 6 characters stays at 18 pt, `-12.34%` drops
to 12 pt. A 296 px row (`2x1`, `1x1`) holds a title of **8 capitals** at
12 pt ×2 (5 at 18 pt ×2 with `status: false`) and, in the `row` look,
`BTC 80000 +15%` as 12 pt ×2 / 18 / 12.

#### Refresh

A tile frame follows the e-ink refresh rules
([`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules*): its layout
identity is the **preset, the `status` flag and, per cell, whether it is
filled, its `title`, `look`, `size` and `invert`**. New values, changes or
directions in the same cells are a **partial** refresh (the usual spacing
and forced-full counters apply); another preset, a toggled `status` or
`invert`, another `look` or `size`, a cell appearing, emptying or renamed,
and the switch from or to a text / ticker / grid frame are a **full** one.

#### Rejections and warnings

A rejected payload is dropped as a whole and the panel does not change;
the text is the `error` of `POST /api/screen` (`400`), `last_error` of a
Pull URL (prefixed `pull: `) and the `error` of `POST /api/pull/test`.
`N` is the index in `tiles`.

| Text | Cause | Fix |
|---|---|---|
| `layout: expected single\|1x1\|1x2\|2x1\|2x2` | Another string (`"grid"`, `"2X2"`) or a non-string | One of the five names, lower case |
| `layout: 2x2 needs tiles` | A preset named without `tiles` | Add `tiles`, or drop `layout` |
| `layout: single cannot carry tiles` | `"single"` next to `tiles` | A preset name, or no `layout` |
| `tiles: must be an array of up to 4 tiles` | `tiles` is an object, string or number | An array |
| `tiles: more than 4` | Five or more elements | At most four |
| `tiles: more than 2 for layout 1x2` (`1 for layout 1x1`, `2 for layout 2x1`) | More elements than the preset has cells | Fewer tiles or a larger preset |
| `tiles[N]: must be an object or null` | A string, number, boolean or array as a tile | An object, `null` or `{}` |
| `tiles[N].title: too long` · `tiles[N].title: printable ASCII only` · `tiles[N].title: must be a string` | Over 15 chars; non-ASCII (`"БТК"`, `€`); a boolean / object / array | ≤ 15 printable ASCII characters, or a number |
| `tiles[N].value: too long` · `tiles[N].value: must be a string` | Over 23 bytes; a wrong type | ≤ 23 bytes, or a number |
| `tiles[N].change: too long` · `tiles[N].change: must be a string` | Over 15 bytes; a wrong type | ≤ 15 bytes, or a number |
| `tiles[N].dir: expected -1\|0\|1 or up\|down\|flat` | A float, another word, a boolean | An integer −1 / 0 / 1 or one of the three words |
| `style: must be an object` · `tiles[N].style: must be an object` | A string or number as a style | An object with `look` / `size` / `invert` |
| `style.look: expected stack\|row\|ticker` · `tiles[N].style.look: …` | Another look name or a non-string | One of the three |
| `style.size: expected auto\|l\|m\|s` · `tiles[N].style.size: …` | Another size name or a non-string | One of the four |
| `style.invert: must be true or false` · `tiles[N].style.invert: …` | `"yes"`, `1` | A JSON boolean |
| `status: must be true or false` | `"off"`, `0` | A JSON boolean |
| `no recognised fields (title, value, alert, change, dir, age_s, time, spark, short, layout, tiles, style, status)` | None of the root keys present | At least one of them |
| *warning* `value ignored: tiles present` (also `change`, `dir`, `time`, `spark`, `short`) | A single-frame field next to `tiles` – the payload **is** applied | Drop the root field, or ignore the warning |

#### What the API reports for a tile frame

`POST /api/screen` answers `202 {"status":"queued","queue":1}` – with
`"warning":"value ignored: tiles present"` when a root field was ignored.
`POST /api/pull/test` ([HTTP routes](#http-routes)) answers `layout` =
the preset name, `tiles` = the number of filled cells and `title` = the
*Showing* line (the root title, else the joined tile titles):

```json
{"ok":true,"title":"BTC, ETH, SOL","value":"","layout":"2x2","bytes":214,"tiles":3,
 "url":"http://192.0.2.10:8000/tickr?token=<token>","ms":61}
```

`GET /api/screen/state` ([Screen state fields](#screen-state-fields))
names the preset in `layout` and adds `tiles` (filled cells),
`status_band` (the payload's `status`) and `symbols[]` (the titles in cell
order, `""` for an empty cell); `title` is the *Showing* line, `value` is
empty, `age_s` the age shown in the band:

```json
{"title":"BTC, ETH, SOL","value":"","state":"content","card":"none","stale_s":12,
 "render_seq":9,"rendered_at_s":812,"refreshes_full":3,"refreshes_partial":5,"refreshes_skipped":0,
 "last_full_s":640,"partials_since_full":2,
 "layout":"2x2","tiles":3,"status_band":true,"symbols":["BTC","ETH","SOL",""],"age_s":32,
 "power_switch":"off","power_grace_s":0,"source":"url","symbol":"",
 "width":296,"height":128,
 "status":{"wifi":true,"bars":3,"usb":true,"power":"usb","batt_pct":100,"batt_low":false,"ip":"192.0.2.5"}}
```

`GET /api/screen.bmp` is the frame itself – the way to preview a layout.

> **Unverified:** tile payloads over MQTT and through a relay, and on a
> battery device (one frame per wake-up), go through the same parser and
> drawer but have been exercised on the host and over `POST /api/screen` /
> the Pull URL on a USB device only; the Content editor's *Test* button
> reporting a tile frame (`OK (2x2, 3 tiles, …)`) has not been watched in a
> browser.

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
| `GET` | `/api/status` | public | `uptime_s`, `heap_free`, `heap_min_free`, `rssi`, `ip`, `power` (`usb`·`battery`·`unknown`), `power_mode` (override `auto`·`usb`·`battery`), `board` (`A`·`B`·`?`), `vsys_v` (cell) and `vin_v` (rail) – `null` on board `?`, `cell_divider` (the cell divider in effect, `"189/100"` on rev A, `"200/100"` on rev B, or the configured one – [`HARDWARE.md`](HARDWARE.md) → *Power sensing*), `pull_https`, `tls_ca`, `mqtt_enabled`, `mqtt_connected`, `mqtt_state`, `queue_depth`, `queue_dropped`, `auth_enabled`, `tls_insecure_used`, `reset_reason` (`esp_reset_reason_t` of this boot: `1` power-on, `3` software, `4` panic, `5`/`6`/`7` watchdogs, `8` deep-sleep wake-up, `9` brownout), `boot_count` (boots since power-on, deep-sleep wake-ups not counted), `crash_count` (of those: panic, watchdog or brownout) and `crash_last_reason` (`reset_reason` code of the last crash, `0` = none) – kept across every reset but a power-on, so a reboot loop stays visible after it ended, `loop_stack_min_free` (bytes of the main task's stack never used since boot), `last_error`, `wifi_disconnects` (losses of a link that had an address), `wifi_last_reason` (driver reason code of the last one, `wifi_err_reason_t`; `0` = the address was lost while still associated, or none yet), `wifi_down_s` (seconds since the current outage began, `0` when up), `wifi_reconnects` (outages ended after the firmware's own reconnect), `wifi_restarts` (restarts after 30 min without a link) – counters kept across software restarts and deep sleep, zero after a power-on ([`DEVICE_UI.md`](DEVICE_UI.md) → *Wi-Fi link supervision*), `led {r,g,b}` (the colour actually driven, overlays included), `peers {count, fresh, members, sent, send_failed, received, verified, dropped, period_s, broadcast, running}` |
| `GET` | `/api/config` | token | Settings without secrets: `mqtt_server`, `mqtt_port`, `mqtt_topic`, `mqtt_user`, `pull_url`, `refresh_interval` (min, 1–1440), `adc_cell_num`, `adc_cell_den` (cell divider override; `0`/`0` = auto, the board's divider), `led_rule` (`off`·`sign`), `led_battery` (`off`·`hold` – the LED through deep sleep, [`DEVICE_UI.md`](DEVICE_UI.md) → *LED and sound*), `source_kind`, `tk_preset` (`coingecko`·`kraken`·`binance`·`binance_usdm`·`binance_coinm`·`custom`), `tk_symbol`, `tk_market`, `tk_url`, `tk_price`, `tk_change`, `tk_spark`, `tk_mode`, `tk_decimals` (255 = auto), `tk_sep`, `tk_label`, `tk_short` (badge name ≤ 7 chars, `""` = derived from the symbol); the 2×2 grid ([`TICKERS.md`](TICKERS.md) → *Several tickers on one panel: the 2×2 grid*): `tk_view` (`single` · `grid`), `tk_n` (1–4 ticker rows) and the rows 1–3 as `tk1_preset` … `tk1_short`, `tk2_*`, `tk3_*` – the same twelve fields as `tk_*`, always present; flags `mqtt_pass_set`, `tls_ca_set`, `api_token_set`, `ota_password_set`, `group_set`, `tk_api_key_set`, `arduino_ota_build` |
| `POST` | `/config` | token | Save settings – form fields named as above plus the secrets `mqtt_pass`, `api_token`, `ota_password`, `tls_ca_pem`, `tk_api_key` (an empty field keeps the stored value; `<field>_clear=1` erases it). Validated as a whole; `400` with the reasons, else `200`. A changed Pull URL, ticker source, grid view / rows or interval fetches at once on USB. Rows the grid shows (`tk_view=grid`, index below `tk_n`) must resolve like `tk_*` (`ticker 2: symbol required`); other rows are stored as posted. The power-source override is `POST /api/power/source` |
| `POST` | `/api/pull/test` | token | Form field `pull_url` (`http://` or `https://`, ≤ 127 chars) – one GET of that URL through the payload parser, nothing saved or drawn (≤ ≈ 10 s; the same transport as the scheduled pull – [`TICKERS.md`](TICKERS.md) → *TLS*): `{"ok":true,"title":"…","value":"…","layout":"text"·"ticker"·a tile preset,"bytes":73,"url":"…","ms":412}` (`layout` = what the payload would select; a tile payload adds `tiles` = the filled cells and reports the *Showing* title – [Tile layouts](#tile-layouts); `warning` added for an ignored `alert.sound` or an ignored root field) or `{"ok":false,"error":"root must be a JSON object","url":"…","ms":398}` – the error is the parser's text ([Payload format](#payload-format)) or the fetch reason without the `pull:` prefix ([`TICKERS.md`](TICKERS.md) → *Fetch schedule and errors*). `400` missing / malformed URL, `409` while another test runs |
| `POST` | `/api/source/test` | token | Form fields of a ticker source (`tk_preset`, `tk_symbol`, `tk_market`, `tk_label`, `tk_short`, `tk_decimals`, `tk_sep`; `tk_url`, `tk_price`, `tk_change`, `tk_mode`, `tk_spark` for `custom`) – one fetch now, nothing saved (≤ ≈ 20 s; two requests for a futures perpetual): `{"ok":true,"price":"…","change":"…","dir":1,"label":"…","short":"BTC","url":"…","ms":812}` (`short` = the badge name the device would draw: the posted one or the derived default, [`TICKERS.md`](TICKERS.md) → *What the screen shows*) – the futures presets add `"funding":"FR +0.0048%"` (`""` when only the funding request failed) – or `{"ok":false,"error":"http 451","url":"…","ms":…}`. `400` invalid source, `409` while a test runs |
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
| `layout` | `text` · `ticker` · `grid` · a tile preset (`1x1` · `1x2` · `2x1` · `2x2`); a ticker frame adds `change`, `short` (the badge name, `""` = no badge), `dir` (−1/0/1), `age_s` (total age of the quote); a grid frame adds `symbols[]` and `shorts[]` in cell order and `age_s` of its oldest quote ([`TICKERS.md`](TICKERS.md) → *Several tickers on one panel: the 2×2 grid*); its `title` is the comma-separated symbols. A tile frame ([Tile layouts](#tile-layouts)) adds `tiles` (filled cells), `status_band` (the payload's `status`), `symbols[]` (the titles in cell order, `""` for an empty cell) and `age_s` |
| `source`, `symbol` | What the device fetches on its own (`ticker` · `url` · `none`) and the first ticker's symbol (`""` otherwise) |
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
