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
  change the look.
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
| `{}` or `{"text": "...", "val": "..."}` – only unknown keys | `no recognised fields (title, value, alert, change, dir, age_s, time, spark, short)` | Use the field names from the table; at least one |
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
```

```python
"""TickrDisplay Pull URL example server (docs/INTEGRATION_EXAMPLE.md).

Serves ONE JSON object the device understands at  GET /tickr?token=<token>
Built from the Binance USDT-margined futures 24 h tickers: every pair whose
24 h change is at or above THRESHOLD_PCT, strongest first.

    pip install fastapi uvicorn requests
    TICKR_TOKEN=<token> THRESHOLD_PCT=10 uvicorn pull_server:app --host 0.0.0.0 --port 8000

Device: Source = Custom JSON URL, URL = http://<server-host>:8000/tickr?token=<token>
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

# Firmware limits (tickr_display/src/logic/payload.h): bytes, not characters.
TITLE_MAX, VALUE_MAX, CHANGE_MAX = 63, 127, 15

app = FastAPI()
_lock = threading.Lock()
_payload = {"title": "Binance movers", "value": "starting...", "alert": {"led": "000000"}}


def cut(text: str, limit: int) -> str:
    """Trim to the firmware limit; the panel fonts draw printable ASCII only."""
    text = text.encode("ascii", "replace").decode("ascii")
    return text if len(text) <= limit else text[: limit - 3] + "..."


def fetch_rows():
    """[(symbol, pct), ...] above the threshold, strongest first."""
    r = requests.get(SOURCE_URL, timeout=10, headers={"Accept": "application/json"})
    r.raise_for_status()
    rows = []
    for t in r.json():
        sym = str(t.get("symbol", ""))
        if not sym.endswith("USDT") or "_" in sym:   # EDIT: perpetuals quoted in USDT only
            continue
        try:
            pct = float(t["priceChangePercent"])
        except (KeyError, TypeError, ValueError):
            continue
        if pct >= THRESHOLD_PCT:
            rows.append((sym[:-4], pct))
    rows.sort(key=lambda x: x[1], reverse=True)
    return rows


def build_payload(rows) -> dict:
    """Always ONE object - never a list, never {} - even when there is nothing to show."""
    if not rows:
        # EDIT: what the panel shows while nothing passes the threshold.
        return {"title": "Binance movers",
                "value": cut(f"nothing above +{THRESHOLD_PCT:g}%", VALUE_MAX),
                "alert": {"led": "000000"}}
    sym, pct = rows[0]
    # EDIT: the text lines. `change` + `dir` switch the panel to the ticker look.
    return {
        "title": cut(f"{sym} +{pct:.1f}%", TITLE_MAX),
        "value": cut(" | ".join(f"{s} +{p:.1f}%" for s, p in rows), VALUE_MAX),
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
  `value`, `layout`, `source`).
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
> data in a background thread so the request is answered from memory. My
> data source is: … The text I want on the panel is: …
