# Building your own data source

How to feed a TickrDisplay from your own script or server, with a complete
working example. This page is self-contained: a person – or a language
model given only this file – has everything needed to produce a response
the device accepts. Routes, status codes and the authoritative field list
are in [`API.md`](API.md); this page explains how to use them.

## Three ways in

| | **Pull URL** (*Custom JSON URL*) | **Push** (`POST /api/screen`, MQTT) | **Ticker → Custom JSON** |
|---|---|---|---|
| Who talks to whom | The device GETs your URL every *N minutes* | Your script sends to the device | The device GETs any JSON API and extracts **one number** |
| What you return / send | One **ready payload object** (title, value, …) | The same payload object | Any JSON; you give the *path* to the price (and change) |
| Formatting | Yours – the text is drawn as sent | Yours | The device's: thousands separator, decimals, `+0.07%`, sparkline |
| Works on battery | **Yes** – fetched on every wake-up | Only while the device is awake (a few seconds per wake-up; MQTT runs on USB only), or parked on a relay for a group member – [`MULTI_DEVICE.md`](MULTI_DEVICE.md) | **Yes** |
| Survives a reboot | Yes (it is a setting) | No – the *Ready* card waits for the next push | Yes |
| Transport | Plain `http://` or `https://`; HTTPS is checked only against a root CA you upload | HTTP to the device on your LAN | HTTPS always checked (uploaded CA or built-in roots) |
| Use it for | Anything you compute yourself: screeners, alerts, several values in one line | Home Assistant / Node-RED events, scripts, one-off messages | A plain price feed – [`TICKERS.md`](TICKERS.md) → *Custom JSON* |

The rest of this page is about the first two: they share the payload. The
third one has its own rules (a numeric value, a mini-JSONPath) and is
documented in [`TICKERS.md`](TICKERS.md).

## The contract

**The device expects exactly one JSON object.** Not an array of objects,
not a bare string or number, not an empty array. The object must contain at
least one of the recognised fields below; every other key is ignored. The
whole body must be **4096 bytes or less**. Limits are **bytes** of UTF-8;
the panel fonts draw **printable ASCII** (`0x20`–`0x7E`) only – other
characters are not drawn, so keep the text ASCII.

```json
{"title": "BTC +12.4%", "value": "BTC +12.4% | SOL +10.1%", "change": "+12.4%", "dir": 1,
 "alert": {"led": "00FF00", "brightness": 128, "sound": "double_beep"}}
```

The smallest valid document is `{"value": "hello"}`.

| Field | Type | Limit | Meaning |
|---|---|---|---|
| `title` | string or number | ≤ 63 bytes | Heading line, top left. Together with `value` it redraws the screen (text look) |
| `value` | string or number | ≤ 127 bytes | Main text, centred, as large as it fits, truncated with `...` on the panel |
| `change` | string or number | ≤ 15 bytes | Printed as given, e.g. `"+0.07%"`. A non-empty `change` **switches to the ticker look** (triangle, change on the top row, `value` drawn as the price). `""` is accepted and draws nothing |
| `dir` | `-1` · `0` · `1`, or `"down"` · `"flat"` · `"up"` | integer or one of the three words | Triangle direction and the LED rule. Absent: derived from the sign of `change`. A float or any other word is rejected |
| `age_s` | number ≥ 0 | – | Age of the quote when you sent it, in seconds; the device adds the time since and draws *just now* / `N min ago`. Strings and negatives are rejected |
| `time` | string or number | ≤ 15 bytes | Drawn verbatim in place of the relative age (`"19:00"`) |
| `spark` | array of numbers | first 48 numbers kept | Sparkline, min–max scaled; non-numbers are skipped, fewer than 2 points draw nothing. A non-empty array **switches to the ticker look** |
| `short` | string or number | ≤ 7 printable ASCII | Name on the black badge of a ticker frame (`"BTC"`). Does not switch the look by itself |
| `alert` | object | – | Container for the three fields below; any other type is rejected |
| `alert.led` | string | exactly 6 hex digits, optional leading `#` | LED colour `RRGGBB` (`"00FF00"`, `"#ff0000"`; `"000000"` = off and forgets the colour). Kept until the next payload changes it. Numbers and colour names are rejected |
| `alert.brightness` | integer | 0–255 | Scales the LED colour per channel (`c × brightness / 255`) – an explicit `alert.led` as well as the automatic red/green by sign. Default 255. `0` keeps the colour dark but remembered (a later non-zero value restores it). A float or an out-of-range value is rejected |
| `alert.sound` | string | preset or RTTTL ≤ 1023 bytes | `beep` · `double_beep` · `long_beep` · `none` · `""`, or an RTTTL melody. An unknown string is *ignored with a warning* – the rest of the payload is still applied |
| `alert.volume` | integer | 0–255 | Amplitude for this and later sounds. A float or an out-of-range value is rejected |
| `tiles` | array of ≤ 4 | object · `null` · `{}` per cell | Several coins in one frame – see [Several coins at once: tile layouts](#several-coins-at-once-tile-layouts). A tile = `title` (≤ 15 printable ASCII), `value` (≤ 23 bytes), `change` (≤ 15 bytes), `dir`, optional `style` |
| `layout` | string | `single` · `1x1` · `1x2` · `2x1` · `2x2` | The tile preset; `single` (the default) is the only value without `tiles` |
| `style`, `status` | object, boolean | `{"look","size","invert"}`, `true`/`false` | Tile defaults and the status band ([`API.md`](API.md) → *Tile layouts*) |

Notes on types:

* A **number** in `title`, `value`, `change`, `time` or `short` is rendered
  by the device: integers as they are, floats with **6 significant digits**
  (`95240.55` becomes `95240.6`). Format numbers yourself and send strings.
* `dir`, `alert.volume` and `alert.brightness` must be JSON **integers**
  (`1`, not `1.0`); `age_s` may be a float.
* `alert.volume` and `alert.brightness` are **sticky**: they stay in effect
  for later payloads until changed, and go back to 255 on a boot – every
  battery wake-up is a boot, so send them in every payload there. Brightness
  dims the colour the payload sets, not the overlays (update, pairing,
  identify, offline pulse) or the wake-up blinks; in deep sleep with *LED on
  battery* = `hold` the held colour is full-on per channel regardless of
  brightness, and `0` holds nothing. Thinking in per cent? Send
  `round(pct * 255 / 100)` – 50 % is `128`.
* Only `change` and `spark` select the ticker layout. A payload with just
  `title` / `value` keeps the text look; `short` and `dir` alone do not
  change the look. `tiles` selects a tile layout and wins over both.
* An explicit `alert.led` always wins. Without one, the device's own *LED
  rule* setting (`sign`) colours the LED red/green by `dir`; with the
  default `off` the LED is left as it was.

### What is rejected, and the exact reason

Validation is all-or-nothing: an invalid document is dropped and the screen
does not change. The reason text comes from the parser; it is the text you
see in `last_error` (`GET /api/status`) and in the error body of
`POST /api/screen`.

| You sent | Reason text | Fix |
|---|---|---|
| `[{"text": "BTC", "val": "+31%"}]` – an array, even with one element | `root must be a JSON object` | Return the object itself, not a list around it |
| `[]` – "nothing to show" | `root must be a JSON object` | Return an *idle* object: `{"title":"Movers","value":"nothing above +30%"}` |
| `{}` or `{"text": "...", "val": "..."}` – only unknown keys | `no recognised fields (title, value, alert, change, dir, age_s, time, spark, short, layout, tiles, style, status)` | Use the field names from the table; at least one |
| `"hello"`, `42`, `null` | `root must be a JSON object` | Wrap it: `{"value": "hello"}` |
| Not JSON (HTML error page, trailing comma, single quotes) | `invalid JSON: <parser detail>` | Send `application/json` with double quotes and no comments |
| A body over 4096 bytes | `payload too large (max 4096 bytes)` (push, `413`) · `response too large` (pull) | Shorten `value`, trim `spark` to 48 points |
| An empty body (`200 OK` with nothing in it) | `empty body` | Always return an object |
| `"title": true` / `{"a":1}` / `[1,2]` | `title: must be a string` | Strings or numbers only |
| `"value"` longer than 127 bytes | `value: too long` | Truncate on your side |
| `"alert": "beep"` | `alert: must be an object` | `"alert": {"sound": "beep"}` |
| `"led": 65280` or `"led": "green"` | `alert.led: must be a string RRGGBB` · `alert.led: expected 6 hex digits RRGGBB` · `alert.led: invalid hex digit` | `"led": "00FF00"` |
| `"volume": 128.0` / `300` | `alert.volume: must be an integer 0..255` · `alert.volume: out of range 0..255` | An integer 0–255 |
| `"brightness": 50.0` / `"50%"` / `300` | `alert.brightness: must be an integer 0..255` · `alert.brightness: out of range 0..255` | An integer 0–255 (`round(pct * 255 / 100)`) |
| `"dir": 1.0` / `"dir": "rising"` | `dir: expected -1\|0\|1 or up\|down\|flat` | `1` or `"up"` |
| `"age_s": "120"` / `-5` | `age_s: must be a non-negative number` | `120` |
| `"spark": "1,2,3"` | `spark: must be an array of numbers` | `[1, 2, 3]` |
| `"short": "Bitcoin!"` / `"short": "БТК"` | `short: printable ASCII, up to 7 chars` | `"BTC"` |
| `"sound": "ding"` | *not rejected* – `alert.sound ignored: unknown preset or invalid RTTTL` | One of the presets or valid RTTTL |
| `"tiles": {"title": "BTC"}` / five elements | `tiles: must be an array of up to 4 tiles` · `tiles: more than 4` | An array of at most four cells; `tiles: more than 2 for layout 1x2` when the preset is smaller |
| `"tiles": [{"title":"A"}, "B"]` | `tiles[1]: must be an object or null` | Objects, `null` or `{}` only |
| A tile `title` of 16+ chars, `"БТК"` / a `value` of 24+ bytes | `tiles[2].title: too long` · `tiles[0].title: printable ASCII only` · `tiles[0].value: too long` | Shorten on your side |
| `"layout": "grid"` / `"layout": "2x2"` without `tiles` / `"layout": "single"` with `tiles` | `layout: expected single\|1x1\|1x2\|2x1\|2x2` · `layout: 2x2 needs tiles` · `layout: single cannot carry tiles` | A preset name next to `tiles`, or leave `layout` out |
| `"style": "bold"` / `{"look": "big"}` / `{"size": "xl"}` / `{"invert": "yes"}` | `style: must be an object` · `style.look: expected stack\|row\|ticker` · `style.size: expected auto\|l\|m\|s` · `style.invert: must be true or false` (prefixed `tiles[N].` for a tile's own style) | The listed values; booleans as `true` / `false` |
| `"value": "…"` next to `tiles` | *not rejected* – `value ignored: tiles present` | Drop the root `value` / `change` / `dir` / `spark` / `short` / `time`, or ignore the warning |

Mistakes on the **Ticker → Custom JSON** path are different: the value at
the price path must be a plain number – `"31.2"`, `31.2`, `"-0.5"` – and
`"+31.2%"` reads as *price not a number*; a path of more than 31 characters
is not accepted. Details: [`TICKERS.md`](TICKERS.md) → *Custom JSON* and
*Fetch schedule and errors*.

## How the device fetches a Pull URL

* The URL is at most **127 characters** and must start with `http://` or
  `https://`. A query string is fine – `?token=<token>` is the simplest way
  to protect your endpoint.
* The request is **`GET`, HTTP/1.0**: answer with a `Content-Length` (or
  close the connection after the body); do not use chunked transfer
  encoding. Every stage – connect, TLS handshake, headers, body – has a
  **10 s** timeout. Up to **3 redirects** are followed.
* Only a **`200`** answer is read; anything else is a failed fetch and the
  previous frame stays. A declared or actual body over 4096 bytes, or an
  empty body, is a failed fetch too.
* **HTTPS**: the certificate is checked against a root CA you upload on
  `/system` (*TLS root CA*); without one the connection is made anyway and
  the UI marks it INSECURE. Plain `http://` on the LAN needs nothing.
* The interval (*Every N minutes*, 1–1440) is the device's poll period: on
  USB a timer, on battery the deep-sleep period – one fetch per wake-up.
  Your server may refresh its own data as often as it likes; the device
  only sees what the URL returns at that moment.
* The response is parsed exactly like a `POST /api/screen` body, so you
  can test a payload with `curl` against the device before you write a
  line of server code.

## Complete example: a Pull URL server in Python

One file, three packages. It asks Binance for the USDT-margined perpetual
futures, keeps every pair whose 24 h change is at or above `THRESHOLD_PCT`,
and serves **one** payload object at `GET /tickr?token=<token>` – an *idle*
object when nothing qualifies, so the device always gets something valid.
The data is refreshed in a background thread; the device only reads the
cache. The same file is in [`examples/pull_server.py`](examples/pull_server.py).
Lines you are meant to change are marked `# EDIT:`.

```bash
pip install fastapi uvicorn requests
TICKR_TOKEN=<token> THRESHOLD_PCT=10 LED_BRIGHTNESS=128 uvicorn pull_server:app --host 0.0.0.0 --port 8000
curl -s "http://localhost:8000/tickr?token=<token>"
# {"title":"XYZ +14.2%","value":"XYZ +14.2% | ABC +11.0%","change":"+14.2%","dir":1,"alert":{"led":"00FF00","sound":"double_beep","brightness":128}}
# with GRID=1 in the environment: the top four as tiles (see "Several coins at once: tile layouts")
# {"layout":"2x2","style":{"look":"stack","size":"auto"},"tiles":[{"title":"XYZ","value":"0.293","change":"+14%"},{"title":"ABC","value":"1.52","change":"+11%"},null,null],"title":"Binance movers","alert":{"led":"00FF00","brightness":128}}
```

```python
"""TickrDisplay Pull URL example server (docs/INTEGRATION_EXAMPLE.md).

Serves ONE JSON object the device understands at  GET /tickr?token=<token>
Built from the Binance USDT-margined futures 24 h tickers: every pair whose
24 h change is at or above THRESHOLD_PCT, strongest first.

    pip install fastapi uvicorn requests
    TICKR_TOKEN=<token> THRESHOLD_PCT=10 uvicorn pull_server:app --host 0.0.0.0 --port 8000

    Device: Source = Custom JSON URL, URL = http://<server-host>:8000/tickr?token=<token>

GRID=1 sends the top four movers as a 2x2 tile frame instead of one text /
ticker frame (docs/API.md "Tile layouts"): one tile per coin, the cells of
coins below the threshold left empty (null). The server picks the tiles,
the device draws them.
"""
import os
import threading
import time

import requests
from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse

# EDIT: the shared secret; the device sends it as ?token=... in the URL.
TOKEN = os.environ.get("TICKR_TOKEN", "")
# EDIT: a pair must have moved at least this much (per cent, 24 h) to be listed.
THRESHOLD_PCT = float(os.environ.get("THRESHOLD_PCT", "10"))
# EDIT: how often the exchange is asked (seconds). The device polls on its own interval.
REFRESH_S = int(os.environ.get("REFRESH_S", "60"))
# EDIT: LED brightness 0-255 (255 = full). Thinking in per cent? round(pct * 255 / 100).
LED_BRIGHTNESS = max(0, min(255, int(os.environ.get("LED_BRIGHTNESS", "255"))))
# EDIT: any JSON API; change fetch_rows() to match its shape.
SOURCE_URL = "https://fapi.binance.com/fapi/v1/ticker/24hr"
# EDIT: GRID=1 -> a 2x2 of tiles (up to four coins), else one frame with the strongest mover.
GRID = os.environ.get("GRID", "0") == "1"
# EDIT: the tile style (GRID=1): look stack|row|ticker, size auto|l|m|s, invert true|false.
TILE_STYLE = {"look": "stack", "size": "auto"}

# Firmware limits (tickr_display/src/logic/payload.h, tiles.h): bytes, not characters.
TITLE_MAX, VALUE_MAX, CHANGE_MAX = 63, 127, 15
TILE_TITLE_MAX, TILE_VALUE_MAX = 15, 23

app = FastAPI()
_lock = threading.Lock()
_payload = {"title": "Binance movers", "value": "starting...", "alert": {"led": "000000"}}


def cut(text: str, limit: int) -> str:
    """Trim to the firmware limit; the panel fonts draw printable ASCII only."""
    text = text.encode("ascii", "replace").decode("ascii")
    return text if len(text) <= limit else text[: limit - 3] + "..."


def fetch_rows():
    """[(symbol, pct, last_price), ...] above the threshold, strongest first."""
    r = requests.get(SOURCE_URL, timeout=10, headers={"Accept": "application/json"})
    r.raise_for_status()
    rows = []
    for t in r.json():
        sym = str(t.get("symbol", ""))
        if not sym.endswith("USDT") or "_" in sym:   # EDIT: perpetuals quoted in USDT only
            continue
        try:
            pct = float(t["priceChangePercent"])
            last = str(t.get("lastPrice", ""))
        except (KeyError, TypeError, ValueError):
            continue
        if pct >= THRESHOLD_PCT:
            rows.append((sym[:-4], pct, last))
    rows.sort(key=lambda x: x[1], reverse=True)
    return rows


def tile_price(last: str) -> str:
    """A short price for a 148 px cell: the bottom row of a tile holds about
    7 characters of value next to a 4-5 character change (docs/API.md "Tile layouts")."""
    try:
        p = float(last)
    except ValueError:
        return last
    if p >= 1000:
        return f"{p:.0f}"           # 80000
    if p >= 1:
        return f"{p:.2f}"           # 185.20
    return f"{p:.3g}"               # 0.00237


def build_tiles(rows) -> dict:
    """GRID=1: the top four as a 2x2; cells without a mover stay empty (null).
    A tile is title / value / change (+ dir, derived from the sign); the device
    picks the font sizes. All-empty tiles draw a blank grid - still one object."""
    tiles = [None, None, None, None]
    for i, (sym, pct, last) in enumerate(rows[:4]):
        tiles[i] = {"title": cut(sym, TILE_TITLE_MAX), "value": cut(tile_price(last), TILE_VALUE_MAX),
                    "change": cut(f"+{pct:.0f}%", CHANGE_MAX)}
    return {
        "layout": "2x2",
        "style": TILE_STYLE,
        "tiles": tiles,
        "title": "Binance movers",            # the shelf's Showing line only
        "alert": {"led": "00FF00" if rows else "000000", "brightness": LED_BRIGHTNESS},
    }


def build_payload(rows) -> dict:
    """Always ONE object - never a list, never {} - even when there is nothing to show."""
    if GRID:
        return build_tiles(rows)
    if not rows:
        # EDIT: what the panel shows while nothing passes the threshold.
        return {"title": "Binance movers",
                "value": cut(f"nothing above +{THRESHOLD_PCT:g}%", VALUE_MAX),
                "alert": {"led": "000000"}}
    sym, pct, _ = rows[0]
    # EDIT: the text lines. `change` + `dir` switch the panel to the ticker look.
    return {
        "title": cut(f"{sym} +{pct:.1f}%", TITLE_MAX),
        "value": cut(" | ".join(f"{s} +{p:.1f}%" for s, p, _ in rows), VALUE_MAX),
        "change": cut(f"+{pct:.1f}%", CHANGE_MAX),
        "dir": 1,
        # EDIT: RRGGBB, beep|double_beep|long_beep|none. brightness is sticky on the device but
        # reset on boot (every battery wake-up is a boot), so it is sent in every payload.
        "alert": {"led": "00FF00", "sound": "double_beep", "brightness": LED_BRIGHTNESS},
    }


def refresh_loop():
    global _payload
    while True:
        try:
            p = build_payload(fetch_rows())
        except Exception as e:                      # the device must still get an object
            p = {"title": "Binance movers", "value": cut(f"source error: {e}", VALUE_MAX),
                 "alert": {"led": "FF0000"}}
        with _lock:
            _payload = p
        time.sleep(REFRESH_S)


threading.Thread(target=refresh_loop, daemon=True).start()


@app.get("/tickr")
def tickr(token: str = Query(default="")):
    if not TOKEN or token != TOKEN:
        return JSONResponse({"error": "unauthorized"}, status_code=401,
                            headers={"Cache-Control": "no-store"})
    with _lock:
        body = dict(_payload)
    return JSONResponse(body, headers={"Cache-Control": "no-store"})
```

Things the example gets right that a first attempt usually misses:

* It returns **one object**, also when the list is empty and also when the
  exchange is down – the panel then says so instead of staying blank.
* It **truncates** `title`, `value` and `change` to the firmware limits and
  strips non-ASCII characters.
* `Cache-Control: no-store`, so a proxy between the device and the server
  does not serve a stale object.
* The exchange is asked in the background; the device's request is answered
  from memory within milliseconds – well inside the 10 s budget, also on a
  battery device that is awake for a few seconds.
* `alert.sound` is sent only when there is something to announce; the idle
  object turns the LED off.

### The same payload pushed with curl

Without a server of your own, send the object to the device directly. The
token here is the **device's** API token, not the one of the example server.

```bash
curl -X POST "http://<device-ip>/api/screen" -u ":<token>" \
     -H 'Content-Type: application/json' \
     -d '{"title":"BTC +12.4%","value":"BTC +12.4% | SOL +10.1%","change":"+12.4%","dir":1,
          "alert":{"led":"00FF00","brightness":128,"sound":"double_beep"}}'
# -> 202 {"status":"queued","queue":1}
# A rejected body answers 400 {"error":"<reason text from the table above>"}
```

The same object published to the MQTT topic (default `tickr/display`) has
the same effect on a USB-powered device.

## Several coins at once: tile layouts

One `value` with every mover joined by `|` is cut to what fits one line.
For more than one coin send **tiles**: the panel is split into cells and
every cell shows one item – a big name, the price under it, the change
with its triangle at the right. The rule of the split: **you choose the
tiles, the device draws them.** Your script decides which cells are filled
and in what order; the firmware owns the placement and the font sizes.
The field-by-field reference, the exact limits, the font ladders and the
rejection texts are in [`API.md`](API.md) → *Tile layouts*; this section
shows what to send and what you will see.

### The shape of a tile payload

```json
{"layout": "2x2",
 "style": {"look": "stack", "size": "auto", "invert": false},
 "tiles": [
   {"title": "BTC", "value": "80000", "change": "+15%", "dir": 1, "style": {"invert": true}},
   {"title": "ETH", "value": "4100",  "change": "+12.3%"},
   null,
   {}],
 "status": true,
 "title": "Movers",
 "age_s": 20,
 "alert": {"led": "00FF00", "brightness": 128, "sound": "beep"}}
```

* **`tiles`** (required for a tile frame): an array of **up to four**
  elements, one per cell, in reading order – top-left, top-right,
  bottom-left, bottom-right. An element is a **tile object**, or `null` /
  `{}` for an **empty cell**; a shorter array leaves the remaining cells
  empty. `[null, null, null, null]` draws a blank frame – still one object,
  never `[]`.
* A **tile** has the field names you already know: `title` (≤ 15
  printable ASCII characters – the big text), `value` (≤ 23 bytes),
  `change` (≤ 15 bytes, drawn with its triangle), `dir` (`1` / `0` / `-1`
  or `"up"` / `"flat"` / `"down"`; derived from the sign of `change` when
  absent) and an optional `style`. A tile with none of `title`, `value`,
  `change` is an empty cell.
* **`layout`**: `2x2` (four cells of 148×64), `1x2` (two tall columns of
  148×128), `2x1` (two wide rows of 296×64), `1x1` (one cell over the
  whole 296×128 panel). Optional: without it one tile → `1x1`, two →
  `1x2`, anything else → `2x2`. `single` means "no tiles" and is only
  valid without them.
* **`style`** at the root = defaults for every cell; `style` inside a tile
  = overrides for that cell. Keys: `look` (`stack` · `row` · `ticker`),
  `size` (`auto` · `l` · `m` · `s`), `invert` (`true` · `false`). Every key
  is optional at both levels; the defaults are `stack`, `auto`, `false`.
* **`status`**: `true` (default) keeps the Wi-Fi / power badges and the
  age line in the lowest 16 rows of the bottom-right cell; `false` drops
  them and gives that cell its full height.
* The root **`title`** is only what the web UI's *Showing* line says
  (without it: the tile titles joined); **`age_s`** feeds the age line;
  **`alert`** works as always – the LED rule `sign` follows the **first**
  tile. The root `value`, `change`, `dir`, `time`, `spark` and `short` are
  **ignored** next to `tiles` (the answer carries
  `"warning": "value ignored: tiles present"`); the frame is still drawn.
* Everything else about a payload still holds: one JSON object, ≤ 4096
  bytes, printable ASCII, numbers rendered with 6 significant digits –
  format on your side and send strings.

### The three looks

What goes where in a cell. Sizes are what the device picks for a 148 px
cell with short texts; it goes one step smaller whenever a text does not
fit.

**`stack`** (default) – the title large in the upper band; the value
bottom-left and the change with its triangle bottom-right. In a 64 px cell
(`2x2`, the rows of a `2x1`) two bands; in a tall cell (`1x2`, `1x1`)
three – title, value, change – one under the other, the value and the
change at 18 pt:

```
148 x 64 (2x2 cell)              148 x 128 (1x2 column)
+----------------------+         +----------------------+
| BTC                  |  title  | BTC                  |  title   12 pt x2
| 81250      ^ +16.2%  |  row    | 80000                |  value   18 pt
+----------------------+         |             ^ +15%   |  change  18 pt
  value 12 pt   change 12 pt     |                      |
                                 +----------------------+
```

**`row`** – one line: the title at the left, the value and the change
right-aligned. Made for the 296 px rows of a `2x1` (and a `1x1`); in a
148 px cell the three texts share 140 px and the value is the one cut:

```
296 x 64 (2x1 row)
+--------------------------------------------------+
| BTC                           80000   ^ +15%     |   12 pt x2 / 18 pt / 12 pt
+--------------------------------------------------+
```

**`ticker`** – the cell of the device's own 2×2 ticker grid: the title
white on a black badge top-left, the change with its triangle at the right
of the badge row, the value right-aligned below. Always drawn in the top
46 rows of the cell, so it is made for the `2x2`:

```
148 x 64
+----------------------+
|[BTC]        ^ +0.9%  |   badge 12 pt, change 9 pt
|            2 530.43  |   value 18 pt, right-aligned
+----------------------+
```

**The status band** sits in the bottom-right cell of every preset unless
`"status": false`. That cell is 16 rows shorter: in a `2x2` or the lower
row of a `2x1` its `stack` bottom row is 9 pt (the title stays 18 pt), in
a `1x2` or `1x1` the bands are laid out from this shorter height – the two
columns of a `1x2` therefore align:

```
147 x 63 bottom-right cell of a 2x2, status band on
+----------------------+
| DOGE                 |   title 18 pt
| 0.2531     ^ +9.8%   |   value / change 9 pt
| just now          /| |   the band: age line left, badges right
+----------------------+
```

**`invert: true`** fills the cell black and draws every element white
(the badge of the `ticker` look becomes a white field with black text);
the status band stays white. Use it to single out one cell – the top
mover, a breached threshold.

### Keeping the text short

The device never overflows a cell: a text that does not fit at the largest
size is drawn smaller, and a text that does not fit at 9 pt is cut with
`...`. What stays large ([`API.md`](API.md) → *Fonts and fitting* has the
full table):

| Where | Title stays at the largest size with | On the value / change row |
|---|---|---|
| A 148 px cell of a `2x2` (top cells, bottom-left; every cell with `status: false`) | **≤ 4 capitals** (`DOGE`), 5 digits; `MATIC` drops one step | **5 digits of value at 12 pt next to a 4-character change** (`80000` · `+15%`); a 6-character change (`+12.3%`) pulls both to 9 pt; 6–7 characters of value fit at 9 pt |
| The bottom-right cell of a `2x2` with the band (147×47) | ≤ 5 capitals at 18 pt | 9 pt from the start: 6 digits next to `+15%`, 5 next to `+12.3%`; a 7-character change (`-12.34%`) leaves room for 5 characters of value |
| A `1x2` column | ≤ 4 capitals (≤ 2 at 18 pt ×2 with `status: false`) | the value alone: **7 digits at 18 pt**; a change of up to 6 characters stays at 18 pt, `-12.34%` drops to 12 pt |
| A 296 px row (`2x1`, `1x1`) | ≤ 8 capitals (≤ 5 at 18 pt ×2 with `status: false` in a `1x1`) | `row` look: `BTC 80000 +15%` as 12 pt ×2 / 18 / 12; a 6-character change drops to 9 pt, a 7-character one pulls the value to 12 pt. `stack` look: 13 digits next to `-12.34%` at 12 pt |

Rules of thumb for a sender:

* **Round the change to what matters**: `+15%` or `+9.8%` rather than
  `+14.62%`. The change shrinks first when a row is tight, and in the
  bottom-right cell of a `2x2` (the smallest one) every character of
  `change` is a character less of `value` – `"-12.34%"` next to a 7-digit
  price shows `0.25...`. Put the item whose price matters least in that
  cell, or send `"status": false` to make the four cells equal (and lose
  the badges and the age line).
* **Shorten the price to the cell**: ≥ 1000 → no decimals (`80000`),
  ≥ 1 → two decimals (`185.20`), below → three significant digits
  (`0.00237`). The example server's `tile_price()` does exactly this.
* **Titles of 3–4 capitals** stay at the largest size in a 148 px cell;
  `MATIC` and `PEPEUSDT` are drawn one and two steps smaller, a 15-character
  title ends up at 9 pt with `...`.
* Values and changes **can change freely** between payloads – the device
  redraws them with a partial refresh. Adding, removing or renaming a
  tile, or toggling `invert` / `status` / the preset, is a full refresh
  (an inversion flash), so keep the set of cells stable while the numbers
  move ([`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules*).

### Worked examples

Each one is a complete body for `POST /api/screen` or a complete Pull URL
response. The line under it says what the panel shows.

**The pump case – the top movers above a threshold fill a `2x2` from the
top left, the rest stays empty:**

```json
{"layout": "2x2",
 "tiles": [
   {"title": "BTC", "value": "80000",  "change": "+15%"},
   {"title": "ETH", "value": "4100",   "change": "+12.3%"},
   {"title": "SOL", "value": "185.20", "change": "+10%"},
   null],
 "title": "Movers", "age_s": 5, "alert": {"led": "00FF00", "sound": "beep"}}
```

Three cells with a big name, the price and an up-triangle with the change
on the bottom row; the bottom-right cell is blank but for the age line
(*just now*) and the power badge; the LED green; the *Showing* line of the
shelf says *Movers*.

**A full `2x2` with the top mover inverted:**

```json
{"layout": "2x2",
 "tiles": [
   {"title": "BTC",  "value": "81250",  "change": "+16.2%", "style": {"invert": true}},
   {"title": "ETH",  "value": "4180",   "change": "+13%"},
   {"title": "SOL",  "value": "190.05", "change": "+11%"},
   {"title": "DOGE", "value": "0.2531", "change": "+9.8%"}]}
```

Four cells; the top-left one black with white text and a white triangle,
the others black on white. `DOGE` in the bottom-right cell is drawn at
18 pt over a 9 pt row because the band takes its lowest rows; the other
three titles at 12 pt ×2.

**`1x2` – two coins large:**

```json
{"layout": "1x2",
 "tiles": [
   {"title": "BTC", "value": "80000",   "change": "+15%"},
   {"title": "ETH", "value": "4100.55", "change": "-2.3%"}]}
```

Two columns with a 1 px line between them; in each, the name at 12 pt ×2,
the price at 18 pt under it and the change at 18 pt with its triangle
right-aligned under that – the rows of both columns at the same heights;
the age line and the badge at the foot of the right column.

**`2x1` – two rows in the `row` look:**

```json
{"layout": "2x1", "style": {"look": "row"},
 "tiles": [
   {"title": "BTC", "value": "80000",   "change": "+15%"},
   {"title": "ETH", "value": "4100.55", "change": "-2.3%"}]}
```

Two lines over the full width – `BTC` left, `80000 ▲+15%` right on the
upper one, `ETH … 4100.55 ▼-2.3%` on the lower one, each title at 12 pt
×2, the price at 18 pt; the lower row ends above the age line and the
badge.

**`1x1` – one hero, no status band:**

```json
{"layout": "1x1", "status": false,
 "tiles": [{"title": "BTC", "value": "80000.12", "change": "+15.3%", "style": {"invert": true}}]}
```

The whole panel black: `BTC` at 18 pt ×2 (52 px capitals), the price and
the change at 18 pt under it, no badges, no age line. With `"status":
true` the title would be 12 pt ×2 and the band white at the bottom.

**The `ticker` look – the device's own grid, fed by you:**

```json
{"layout": "2x2", "style": {"look": "ticker"},
 "tiles": [
   {"title": "BTC", "value": "82 722",   "change": "-0.50%"},
   {"title": "ETH", "value": "2 530.43", "change": "+0.90%"},
   {"title": "SOL", "value": "109.07",   "change": "-0.63%"},
   {"title": "BNB", "value": "746.64",   "change": "-0.46%"}]}
```

Pixel for pixel the built-in 2×2 ticker grid: a black name badge and the
change at the top of every cell, the price right-aligned below – but with
your prices and your formatting (the spaces are yours).

**A blank frame – nothing qualifies right now:**

```json
{"layout": "2x2", "tiles": [null, null, null, null], "title": "Movers", "alert": {"led": "000000"}}
```

An empty grid: the two separators, the age line and the badge in the
bottom-right cell, the LED off. Still one object – `[]` would be rejected
with `root must be a JSON object`.

**Root defaults with per-tile overrides:**

```json
{"layout": "2x2",
 "style": {"look": "stack", "size": "m"},
 "tiles": [
   {"title": "BTC",  "value": "80000",  "change": "+15%", "style": {"invert": true, "size": "auto"}},
   {"title": "ETH",  "value": "4100",   "change": "+12.3%"},
   {"title": "SOL",  "value": "185.20", "change": "+10%", "style": {"look": "ticker"}},
   {"title": "DOGE", "value": "0.2531", "change": "+9.8%", "style": {"invert": true}}]}
```

The root says *stack look, title at most 18 pt*; the first tile keeps the
stack look but takes `size` back to `auto` (12 pt ×2) and inverts; the
second is plain – 18 pt title, 12 pt row; the third is a ticker cell; the
fourth inverts in the cramped bottom-right cell.

**The same tile frame pushed with `curl`** (the device's API token, a
`202` with the queue depth; a rejected body answers `400` with the reason
text):

```bash
curl -X POST "http://<device-ip>/api/screen" -u ":<token>" \
     -H 'Content-Type: application/json' \
     -d '{"layout":"2x2","tiles":[{"title":"BTC","value":"80000","change":"+15%"},
          {"title":"ETH","value":"4100","change":"+12.3%"},{"title":"SOL","value":"185.20","change":"+10%"},null],
          "title":"Movers","alert":{"led":"00FF00"}}'
# -> 202 {"status":"queued","queue":1}
```

**The same frame over MQTT** (the device's topic, default `tickr/display`;
a USB-powered device):

```bash
mosquitto_pub -h <BROKER_HOST> -t tickr/display -m \
  '{"layout":"2x2","tiles":[{"title":"BTC","value":"80000","change":"+15%"},
    {"title":"ETH","value":"4100","change":"+12.3%"},{"title":"SOL","value":"185.20","change":"+10%"},null],
    "title":"Movers","alert":{"led":"00FF00"}}'
```

The body is parsed exactly as over HTTP; a rejection is visible in
`last_error` of `GET /api/status` only (there is no reply on MQTT).

### Previewing a layout

The device is the renderer: `POST` the candidate object to `/api/screen`
and look at the device's card on the shelf page, or fetch
`http://<device-ip>/api/screen.bmp` – that is exactly the frame on the
e-ink. Iterate there before you change the server. `GET /api/screen/state`
reports `layout` (the preset), `tiles` (filled cells), `status_band` and
`symbols[]` (the titles in cell order); the Content editor's **Test**
button shows `OK (2x2, 3 tiles, …)` for a URL that returns tiles. The
example server above builds a tile frame with `GRID=1`.

### Machine-readable contract

The payload as a JSON Schema (draft-07). It follows the parser: limits are
bytes (`maxLength`), `tiles` has at most four elements, each an object or
`null`; a preset needs `tiles`, `single` cannot carry them, and an array
may not have more tiles than the preset has cells. Three things the schema
cannot say: unknown keys are ignored everywhere (so `additionalProperties`
is left open); a root `value` / `change` / `dir` / `time` / `spark` /
`short` next to `tiles` is accepted with a warning; and `integer` means an
integer **literal** – the device rejects `1.0` for `dir`, `brightness` and
`volume`, although JSON Schema counts it as an integer.

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "TickrDisplay payload",
  "description": "One JSON object for POST /api/screen, the MQTT topic or a Pull URL response. At least one recognised field. Text limits are bytes of printable ASCII.",
  "type": "object",
  "properties": {
    "title":  {"$ref": "#/definitions/text63"},
    "value":  {"$ref": "#/definitions/text127"},
    "change": {"$ref": "#/definitions/text15"},
    "dir":    {"$ref": "#/definitions/dir"},
    "age_s":  {"type": "number", "minimum": 0},
    "time":   {"$ref": "#/definitions/text15"},
    "spark":  {"type": "array", "items": {"type": "number"}, "description": "the first 48 numbers are kept"},
    "short":  {"anyOf": [{"type": "string", "maxLength": 7, "pattern": "^[\\x20-\\x7E]*$"}, {"type": "number"}]},
    "alert": {
      "type": "object",
      "properties": {
        "led":        {"type": "string", "pattern": "^#?[0-9A-Fa-f]{6}$"},
        "brightness": {"type": "integer", "minimum": 0, "maximum": 255},
        "volume":     {"type": "integer", "minimum": 0, "maximum": 255},
        "sound":      {"type": "string", "maxLength": 1023, "description": "beep | double_beep | long_beep | none | \"\" | RTTTL; anything else is ignored with a warning"}
      }
    },
    "layout": {"enum": ["single", "1x1", "1x2", "2x1", "2x2"]},
    "tiles": {
      "type": "array",
      "maxItems": 4,
      "items": {"oneOf": [{"$ref": "#/definitions/tile"}, {"type": "null"}]}
    },
    "style":  {"$ref": "#/definitions/style"},
    "status": {"type": "boolean"}
  },
  "anyOf": [
    {"required": ["title"]}, {"required": ["value"]}, {"required": ["change"]}, {"required": ["dir"]},
    {"required": ["age_s"]}, {"required": ["time"]}, {"required": ["spark"]}, {"required": ["short"]},
    {"required": ["alert"]}, {"required": ["layout"]}, {"required": ["tiles"]}, {"required": ["style"]},
    {"required": ["status"]}
  ],
  "allOf": [
    {"if": {"properties": {"layout": {"enum": ["1x1", "1x2", "2x1", "2x2"]}}, "required": ["layout"]},
     "then": {"required": ["tiles"]}},
    {"if": {"properties": {"layout": {"const": "single"}}, "required": ["layout"]},
     "then": {"not": {"required": ["tiles"]}}},
    {"if": {"properties": {"layout": {"const": "1x1"}}, "required": ["layout"]},
     "then": {"properties": {"tiles": {"maxItems": 1}}}},
    {"if": {"properties": {"layout": {"enum": ["1x2", "2x1"]}}, "required": ["layout"]},
     "then": {"properties": {"tiles": {"maxItems": 2}}}}
  ],
  "definitions": {
    "text15":  {"anyOf": [{"type": "string", "maxLength": 15},  {"type": "number"}]},
    "text23":  {"anyOf": [{"type": "string", "maxLength": 23},  {"type": "number"}]},
    "text63":  {"anyOf": [{"type": "string", "maxLength": 63},  {"type": "number"}]},
    "text127": {"anyOf": [{"type": "string", "maxLength": 127}, {"type": "number"}]},
    "dir": {
      "anyOf": [{"type": "integer"}, {"enum": ["up", "down", "flat"]}],
      "description": "-1, 0 or 1 (another integer is read by its sign); floats are rejected"
    },
    "style": {
      "type": "object",
      "properties": {
        "look":   {"enum": ["stack", "row", "ticker"]},
        "size":   {"enum": ["auto", "l", "m", "s"]},
        "invert": {"type": "boolean"}
      }
    },
    "tile": {
      "type": "object",
      "description": "a cell; without title, value and a non-empty change it is an empty cell",
      "properties": {
        "title":  {"anyOf": [{"type": "string", "maxLength": 15, "pattern": "^[\\x20-\\x7E]*$"}, {"type": "number"}]},
        "value":  {"$ref": "#/definitions/text23"},
        "change": {"$ref": "#/definitions/text15"},
        "dir":    {"$ref": "#/definitions/dir"},
        "style":  {"$ref": "#/definitions/style"}
      }
    }
  }
}
```

## Setting up the device

1. Run your server where the device can reach it – the device is on your
   Wi-Fi, so `<server-host>` must be an address or name that resolves and
   routes **from that network** (a LAN address, or a public host).
2. Open **`http://<device-ip>/`** (the shelf), click the device's card,
   then **Change…**.
3. **Source** → **Custom JSON URL**.
4. **URL returning a payload** → `http://<server-host>:8000/tickr?token=<token>`
   (≤ 127 characters, `http://` or `https://`).
5. **Every N minutes** → how often the device fetches (1–1440; on battery
   this is the sleep period – 15 or more is kind to the cell).
6. **Save**. On USB the device fetches at once; on battery at the next
   wake-up.

Checking the result:

* **The panel.** A valid object is drawn within a few seconds on USB. If the
  *Ready* card stays, or the previous content stays, the fetch or the
  parse failed.
* **`last_error`** in `GET /api/status` (also the *Error* row of the device
  sheet and *Last error* on `/system`) carries the reason of the last failed
  pull; it is cleared by the next successful one. See [`API.md`](API.md).
* **Test** next to the URL field in the Content editor fetches the URL on
  the device once, without saving, and shows the outcome – the fastest way
  to see a `root must be a JSON object` while you are still editing the
  server.
* **`GET /api/screen/state`** tells you what is on the panel (`title`,
  `value`, `layout`, `source`; for tiles also `tiles` and `symbols[]`).
* **From your computer:** `curl -s "http://<server-host>:8000/tickr?token=<token>" | python3 -m json.tool`
  must print an object (`{ … }`), not a list (`[ … ]`).

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Panel shows *Ready* / the old content; `last_error` says `root must be a JSON object` | Your endpoint returns a list (`[…]`), or `[]` when there is nothing to show | Return the object itself; return an idle object instead of `[]` |
| `last_error`: `no recognised fields (...)` | Keys like `text` / `val` / `data` – none of the recognised names | Rename to `title`, `value`, `change` … |
| `last_error`: `http 401` / `http 403` | Your server rejected the token – wrong or missing `?token=` in the device URL, or the server's `TICKR_TOKEN` differs | Paste the exact URL your server prints; mind URL-encoding of special characters |
| `last_error`: `http 404` / `http 405` | Wrong path, or the route is `POST` only | The device sends `GET`; check the path |
| `last_error`: `http -1` / `connect/tls failed` / `http -11` | Server unreachable from the device's network (firewall, wrong interface, `127.0.0.1` instead of the LAN address, DNS name the device cannot resolve), or an HTTPS handshake failure | Bind the server to `0.0.0.0`; test from another device on the same Wi-Fi; use the address, not a name, if in doubt |
| `last_error`: `response too large` | Body over 4096 bytes (long lists, pretty-printed JSON with extras, HTML) | Truncate `value`; send only the recognised fields; no pretty-printing needed |
| `last_error`: `empty body` / `incomplete body` | The server answered `200` with nothing, or dropped the connection mid-body; a server that answers an HTTP/1.0 request with chunked encoding and no `Content-Length` | Always return an object; let the framework set `Content-Length` (FastAPI/uvicorn, Flask, Express all do) |
| `last_error`: `invalid JSON: …` | An HTML error page, a comment, a trailing comma, `NaN` | Return `application/json` from your JSON library, not hand-built strings |
| HTTPS works in the browser but not on the device | Private or self-signed CA | Upload the root CA on `/system` → *TLS root CA*, or use plain `http://` on the LAN |
| Works on USB, nothing on battery | On battery the device fetches **once per wake-up** and must get an answer within 10 s | Keep the server answering from cache; check the interval; a battery device cannot be configured while asleep – switch it off and on to get its 3-minute settings window |
| The text is drawn with gaps or boxes | Non-ASCII characters (Cyrillic, `·`, `€`, emoji) | ASCII only; `|`, `-`, `+`, `%` and digits are safe |
| Numbers look rounded (`95240.6`) | A float sent as a JSON number is rendered with 6 significant digits | Format on the server and send a string |
| Price shows but no triangle / LED colour | `change` missing, or the device's *LED rule* is `off` while you expected red/green by sign | Send `change` (and `dir`), or set `alert.led`; or set the LED rule to `sign` |
| LED dark or dim although `alert.led` is set | A sticky `alert.brightness` of `0` (or a low value) from an earlier payload; `GET /api/status` → `led` shows the scaled colour | Send `"brightness": 255` (it is kept until changed and reset on boot) |
| Tiles come out as one text / ticker frame, or the answer says `layout: 2x2 needs tiles` | `tiles` missing or not an array (`"tiles": {...}`), or only `layout` was sent | `"tiles": [ {...}, {...} ]` – an array of objects / `null`; `layout` alone selects nothing |
| The bottom-right cell is smaller than the others / its price is cut | The status band takes its lowest 16 rows; a long `change` eats the value on a 9 pt row | Round `change` (`+15%`), put the least important item there, or `"status": false` |
| A `POST /api/screen` works but the Pull URL does not | The URL answers something else than what you tested – a redirect to a login page, a different route, a cached old body | `curl -v` the exact URL the device uses; check `Cache-Control: no-store` |

## Prompt for an LLM

Paste this together with this page when you ask a language model to write
the server for you:

> Write me a small HTTP server (language: …) that TickrDisplay can poll as
> its *Custom JSON URL*. On `GET /tickr?token=<token>` it must return
> exactly **one JSON object** – never an array, never an empty array,
> never an empty body – with the fields `title` (≤ 63 bytes), `value`
> (≤ 127 bytes), optionally `change` (≤ 15 bytes, like `"+1.2%"`), `dir`
> (`1`, `0` or `-1` as an integer) and `alert` (`{"led": "RRGGBB",
> "brightness": 0–255 as an integer, "sound": "beep" | "double_beep" |
> "long_beep" | "none"}`). Printable ASCII only; truncate to the limits;
> send formatted numbers as strings; send `brightness` in every payload
> (it is reset when the device boots).
> When there is nothing to show, return an idle object such as
> `{"title": "…", "value": "nothing to show", "alert": {"led": "000000"}}`.
> Answer HTTP/1.0 requests with a `Content-Length` header (no chunked
> encoding), within 10 s, body under 4096 bytes, `Cache-Control:
> no-store`; reject a wrong token with `401`. Fetch and cache the upstream
> data in a background thread so the request is answered from memory.
> To show up to four items at once, return a **tile frame** instead:
> `{"layout": "2x2", "tiles": [t1, t2, t3, t4], "title": "<showing line>",
> "alert": {...}}`. `layout` is one of `"1x1"` (one cell, the whole
> panel), `"1x2"` (two columns), `"2x1"` (two rows), `"2x2"` (four cells);
> `tiles` is an array of **at most four** elements, at most the preset's
> cells, in reading order (top-left, top-right, bottom-left, bottom-right).
> Every element is a tile object `{"title": ≤ 15 printable ASCII chars,
> "value": ≤ 23 bytes, "change": ≤ 15 bytes, "dir": 1 | 0 | -1 (optional,
> derived from the sign of change), "style": {...} (optional)}` or `null`
> for an empty cell – fill from the first cell, send `[null, null, null,
> null]` when nothing qualifies, never `[]` and never an array at the root.
> Optional `"style"` at the root (defaults for all cells) and inside a
> tile (overrides): `"look": "stack" | "row" | "ticker"` (stack = big
> title over a value / change row; row = one line for the wide rows of a
> 2x1; ticker = a name badge with the price), `"size": "auto" | "l" | "m"
> | "s"` (a cap on the title size), `"invert": true | false` (white on
> black). Optional `"status": false` hides the Wi-Fi / power badges and
> the age line that otherwise take the lowest 16 rows of the bottom-right
> cell. With `tiles`, the root `value`, `change`, `dir`, `time`, `spark`
> and `short` are ignored – do not send them; the root `title` is only a
> label for the web UI; `age_s` and `alert` work as usual. Keep the texts
> short: a title of 3–4 capitals, a price of ≤ 5 digits (`80000`,
> `185.20`, `0.00237`) and a change of 4–5 characters (`+15%`, `-9.8%`)
> stay large; the bottom-right cell of a `2x2` is the smallest, so a long
> change there costs the price its digits – round the change, or put the
> least important item there. The device chooses the font sizes and never
> overflows a cell. Keep the set of cells and titles stable between
> responses and change only the numbers – that is a quiet partial refresh;
> a changed title, preset, `invert` or `status` is a full refresh with an
> inversion flash. The complete JSON Schema of the payload is in the
> section *Machine-readable contract* of this page – validate against it.
> My data source is: … The text I want on the panel is: …
