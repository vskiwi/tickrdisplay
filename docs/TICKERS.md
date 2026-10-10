# Tickers: crypto quotes fetched by the device

The **Ticker** source turns a TickrDisplay into what the stock product was:
pick a market and a symbol, and the device shows the price, the 24 h change
with an up/down triangle, a sparkline and how old the quote is – no cloud,
no server of your own. The payload fields a proxy can send (`change`, `dir`,
`age_s`, `time`, `spark[]`) and the `POST /api/source/test` route are in
[`API.md`](API.md); the e-ink layout in [`DEVICE_UI.md`](DEVICE_UI.md).

## 1. What the Ticker source does

* The device itself fetches **one symbol** over HTTPS every *refresh
  interval* (1–1440 min; the editor pre-fills 5), extracts price and change
  from the JSON answer, formats them and draws the ticker frame. One symbol
  per device; several symbols = several devices.
* **Presets** – CoinGecko, Kraken, Binance – know the URL and the JSON
  fields. **Custom JSON** takes any URL that answers JSON, with the fields
  named as small JSON paths – a public API or a proxy on your LAN.
* The source produces the same payload a proxy would send, so a Node-RED or
  Home Assistant flow (Appendices A, B) renders identically – the route for
  stocks, indices or anything that needs an API key.

## 2. Presets

`{s}` is the symbol you type, `{m}` the market.

| Preset | Request | Symbol · market examples | Price · change fields | Root CA |
|---|---|---|---|---|
| **CoinGecko** | `api.coingecko.com/api/v3/simple/price?ids={s}&vs_currencies={m}&include_24hr_change=true` – both folded to lower case | coin **id** (`bitcoin`, `ethereum`, `solana`) · `usd`, `eur` | `$.*.{m}` · `$.*.{m}_24h_change` (a percentage) | GTS Root R4, cross-signed by GlobalSign Root CA |
| **Kraken** | `api.kraken.com/0/public/Ticker?pair={s}{m}` – case as typed | `XBT`, `ETH` · `USD`, `EUR` | `$.result.*.c[0]` (last) · `$.result.*.o` (open – change = `(last − open) / open`) | GTS Root R4, cross-signed by GlobalSign Root CA |
| **Binance** | `api.binance.com/api/v3/ticker/24hr?symbol={s}{m}` – folded to upper case | `BTC`, `ETH` · `USDT`, `BTC` | `$.lastPrice` · `$.priceChangePercent` | DigiCert Global Root G2 |
| **Binance USDⓈ-M futures** | `fapi.binance.com/fapi/v1/ticker/24hr?symbol={s}{m}` – upper case; plus `fapi/v1/premiumIndex?symbol={s}{m}` on a perpetual | `BTC`, `ETH` · `USDT` (perpetual), `USDT_261225` (quarterly, expiry as YYMMDD) | `$.lastPrice` · `$.priceChangePercent`; funding `$.lastFundingRate` | DigiCert Global Root G2 |
| **Binance COIN-M futures** | `dapi.binance.com/dapi/v1/ticker/24hr?symbol={s}{m}` – upper case; plus `dapi/v1/premiumIndex?symbol={s}{m}` on a perpetual | `BTC`, `ETH` · `USD_PERP` (perpetual), `USD_261225` (quarterly) | `$[0].lastPrice` · `$[0].priceChangePercent` (one-element arrays); funding `$[0].lastFundingRate` | DigiCert Global Root G2 |

### Binance futures

The two futures presets are the spot preset on the futures hosts, with two
differences:

* **The funding rate replaces the age line.** After the price the device
  makes a second request (`premiumIndex`) and draws `lastFundingRate` as a
  percentage with four fraction digits and an explicit sign in the age
  line's place: **`FR +0.0100%`** (`0.0001` from the API = 0.01 %; clamped to
  ±9.9999 %). If the second request fails the price still counts as a
  successful fetch and the line keeps the last funding text of the same
  contract – empty before the first success, in which case the age line is
  drawn as usual. With the funding line up there is no *stale N min*
  marker on the frame; `stale_s` in `/api/screen/state` still counts, and
  the OFFLINE card keeps its *last update N min ago* line.
* **Quarterly (delivery) contracts have no funding.** A market whose tail
  after `_` is a date (`USDT_261225`, `USD_261225`) skips the second request
  and keeps the age line. Contract codes come from Binance's exchange
  information; an expired one answers `{}` (fapi) or an error object, shown
  as `price path`.
* **Label**: `<symbol>/<market> PERP - Binance` or `<symbol>/<market>
  <date> - Binance` on USDⓈ-M (`BTC/USDT PERP - Binance`, `BTC/USDT 261225 -
  Binance`); COIN-M writes the pair as one word (`BTCUSD PERP - Binance`,
  `ETHUSD 261225 - Binance`). Your own label under *Advanced* wins.
* Two requests per refresh (Binance weight 1 each, limit 2 400 per minute
  on the futures hosts); on battery the second TLS handshake adds one to
  two seconds of awake time. The same `http 451` geo-block as spot applies.

Caveats:

* **CoinGecko** wants the coin *id*, not the ticker symbol (`bitcoin`, not
  `BTC`); the public tier allows roughly 10 requests per minute per IP. Its
  prices are JSON numbers, kept to about seven significant digits – a quote
  above 100 000 with cents may show a rounded last digit. The default label
  for a long id is truncated on the panel; set your own under *Advanced*.
* **Kraken** delivers the price as a string with full precision; the result
  key differs from the pair name (`XXBTZUSD` for `XBTUSD`), which the `*`
  wildcard absorbs. Public limit about one request per second.
* **Binance** answers **`http 451`** from regions it does not serve –
  whether it works depends on your region. An unknown symbol answers an
  error object without `lastPrice`, shown as `price path`.
* No preset needs an API key; all use the roots baked into the firmware
  (§7). The interval floor is 1 min for every preset.

## 3. Custom JSON

For any API that answers a JSON document of at most **4096 bytes**.

* **URL** (≤ 127 chars, `http://` or `https://`): `{s}` and `{m}` are
  replaced by the symbol and market fields, without case folding –
  `https://proxy.lan/quote/{s}?vs={m}`. Both placeholders are optional.
* **Price path** (required) and **change path** (optional), ≤ 31 chars
  each, in a mini-JSONPath:

  | Syntax | Meaning | Example |
  |---|---|---|
  | `$` | the root – optional | `$.lastPrice` = `lastPrice` |
  | `.key` / `key` | object member (≤ 23 bytes; no `.`, `[`, `]` inside) | `data.last` |
  | `[n]` | array index, 0-based | `marketdata.data[0][1]` |
  | `*` | first member of an object / first element of an array | `result.*.c[0]` |

  No recursion, filters or slices; allowed characters are letters, digits
  and `. _ - * [ ] $`. A path that matches nothing – or has a syntax error –
  reads as *nothing there* (`price path` / `change path`).
* **Change mode**: *percentage* (the value at the change path is already
  `%`) or *open price* (change = `(price − open) / open`).
* **History array path** (optional): an array of numbers there (≥ 2 points,
  the first 48 kept) is drawn as the sparkline instead of the device's own
  history (§5).
* A value may be a JSON string (`"84000.06"` – kept exactly) or a JSON
  number (integer, or a float with about seven significant digits);
  anything else is `price not a number` / `change not a number`. Symbol
  ≤ 31 and market ≤ 11 characters – letters, digits, `. _ -`.

## 4. What the screen shows

Label and change with the triangle on the top row, the price centred and as
large as it fits, the sparkline bottom-left, the age line bottom-right,
badges in the corner ([`DEVICE_UI.md`](DEVICE_UI.md) → *Card texts and
layout notes*).

* **Label**: `<symbol>/<market> - <Preset>` (`BTC/USDT - Binance`,
  `bitcoin/usd - CoinGecko`; the futures shapes are under *Binance
  futures*), or your own text (≤ 31 chars).
* **Price**: rounded half-up to *auto* decimals – **0** from 1 000 000,
  **2** from 1, **4** from 0.01, else **6** – or a fixed 0–6; a thousands
  separator every three digits: **space** (`84 000.06`, the default – the
  e-ink fonts are ASCII only), **comma** or none. This applies to what the
  device fetches; a proxy's `value` is drawn as sent. On the panel a price
  with an integer part of 1 000 or more is shown **without its fraction**
  (`84 014.90` → `84 015`, rounded half-up) when the full string does not
  fit the largest size of the price cascade; a price that fits keeps its
  decimals, and the *Decimals* / *Separator* settings still shape the
  string itself (what *Test* returns as `price`).
* **Change**: `+0.07%` / `-1.23%` / `0.00%` (no sign when it rounds to
  zero) with a filled triangle up or down, a dash for flat. Without a change
  path the top row shows the label only.
* **LED**: with the device setting *LED rule* = `sign`, green on an up-move,
  red on a down-move, **unchanged** on flat; `off` (default) leaves the LED
  to the payload's `alert.led`. A fetch failure never touches the LED – red
  means "down", not "broken".
* **Age line**: *just now* under a minute, then `5 min ago` / `2 h ago` /
  `3 d ago`, counted from the fetch. Past **T_stale** = 3 × the refresh
  interval (at least 10 min) it reads **`stale 40 min`** / `stale 3 h` and
  earns one refresh of its own, so a dead source is visible without waiting
  for other content. A proxy's `time` string is shown verbatim instead, and
  so is the funding line of a Binance perpetual (`FR +0.0100%`).

## 5. Sparkline history

* The device keeps the last **48** prices of the ticker source, one per
  successful fetch: 4 h at a 5-min interval, 2 days at 1 h. Drawn min–max
  scaled into a 200 × 24 px box (shortened to clear a long age line); fewer
  than 2 points draw nothing, a flat series is a mid line.
* The ring lives in **RTC memory**, so a battery device keeps it across deep
  sleep; on USB it is ordinary static memory.
* **Reset** by a power-off or reset (RTC memory is lost) and by a change of
  the fetch URL – symbol, market, preset or custom URL. *Test* does not
  touch it. A custom source's own history array wins for that frame; the
  ring is still fed.

## 6. Fetch schedule and errors

**USB power.** The first fetch runs 3 s after Wi-Fi is up, then every
refresh interval ± 10 % jitter (a shelf of devices does not hit one API in
the same second). After a miss the wait grows **1×, 2×, 4×, 8×** the
interval (capped at 8×) until the next success. Saving a changed source or
interval fetches **at once**. A due fetch without a link is retried after
30 s, one during a pairing screen after 10 s – neither counts as a miss.
MQTT may stay active alongside. An identical frame is not redrawn; a
changed one is a differential refresh, with a forced full every 9th frame
or 60 min ([`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh rules*).

**Battery power.** One fetch per wake-up, then deep sleep for the refresh
interval; the LED blinks green after a success, red after a miss. A miss
doubles the sleep (2×, 4× …, capped at 60 min, never below the interval);
the first miss draws nothing, the second and every fourth after it draw the
OFFLINE card with the age of the shown quote ([`DEVICE_UI.md`](DEVICE_UI.md)
→ *E-ink refresh rules*). Every fetch is a TLS handshake (≈ 2 s awake);
15 min or more is kind to the cell.

**What a failure looks like.** The previous frame stays, no card, the LED
is untouched; the age line grows old and eventually reads `stale N min`.
`GET /api/status` → `last_error` carries `pull: <reason>` until a later
fetch succeeds (*System* shows it as *Last error*); *Test* shows the reason
without the prefix:

| Reason | Meaning |
|---|---|
| `http 451` | Binance geo-block – the region is not served |
| `http 4xx` / `5xx` | any other status: rate limit `429`, wrong URL `404` |
| `connect/tls failed` | no connection, or the certificate chain does not verify (§7) |
| `invalid url` · `empty body` · `incomplete body` | the URL does not parse · nothing came back · the connection dropped mid-answer |
| `response too large` · `not json` | over 4096 bytes (filter at the source or through a proxy) · not a JSON document |
| `price path` / `change path` | nothing at that path – usually a wrong symbol (Binance's error object, CoinGecko's `{}`) |
| `price not a number` / `change not a number` | the value there is not numeric, or the open price is 0 |
| `ticker not configured` · `no wifi` | the saved source does not resolve (no symbol / no price path) · *Test* only, the device is not connected |

## 7. TLS

The transport is chosen by the kind of source:

| Source | Roots used | Can it go unverified? |
|---|---|---|
| **Preset** (CoinGecko, Kraken, Binance) | the roots baked into the firmware: DigiCert Global Root G2, GTS Root R4, GlobalSign Root CA, ISRG Root X1 (Let's Encrypt) | **never** – a chain that does not verify is `connect/tls failed` |
| **Custom JSON** ticker URL | your uploaded root CA (*System* → *TLS root CA*, stored as `/ca.pem`) when present, else the baked-in roots | **never** – a self-signed proxy needs its CA uploaded |
| **Custom JSON URL** (the Pull URL that returns a ready payload) | your uploaded root CA | **yes** – without a CA it is fetched unverified; `/api/status` reports `tls_insecure_used` and *System* shows an INSECURE warning |

Plain `http://` needs none of it and is the cheapest choice for a proxy on
the LAN. The bundle is generated at build time from
`tickr_display/certs/*.pem` (`scripts/build_ca_bundle.py`) and is not
checked for expiry on the device. Fingerprints: [`SECURITY.md`](../SECURITY.md).

## 8. Setting it up in the web UI

Shelf page (`/`) → click the device → **Content…** → *Source* = **Ticker**:

1. **Market**: CoinGecko · Kraken · Binance · Binance USDⓈ-M futures ·
   Binance COIN-M futures · Custom JSON – the fields show that preset's
   placeholders and a one-line hint with the API's limits.
2. **Symbol** and **market**; the futures presets take the contract in the
   market field (`USDT` / `USD_PERP` for the perpetual, `USDT_261225` /
   `USD_261225` for a quarterly); *Custom JSON* adds URL, price path, change
   path, change mode and the optional history array path.
3. **Every N minutes** – the refresh interval (pre-filled 5 for a device not
   yet on a ticker; a battery device shows the handshake hint).
4. **Test** – one fetch on *that* device, nothing saved: `BTC/USDT -
   Binance: 84 000.06  +0.07%  (812 ms)` or `Error: http 451`; a futures
   perpetual adds the funding line (`FR +0.0048%`, or *funding: n/a* when
   only the second request failed). The browser cannot call the exchanges
   itself (CORS), the device can.
5. **Advanced** – label, decimals, thousands separator, **LED rule**.
6. **Save** – the device fetches at once and the tile preview follows.

**Text → Send** or **Custom JSON URL → Save** on a ticker device afterwards
switches the source, so the ticker stops fetching. *For all…* offers Ticker
for awake devices; sleeping ones are skipped. The same fields can be posted
by a script: [`API.md`](API.md) → *Status and configuration*.

## 9. Unverified

> **Unverified:** the ticker on a **battery** device – one fetch per wake
> and the sparkline history surviving a real deep sleep – is host-tested
> only and has not been exercised on hardware.

> **Unverified:** the back-off sequence and the `stale N min` line on a
> **failing source** (a dead proxy, a rate-limited API) have not been
> watched through on hardware.

> **Unverified:** the Binance **`http 451`** geo-block path has not been
> reproduced from a blocked region.

## Appendix A – Node-RED proxy flow

Import via *Menu → Import*. Two paths in one flow: **(1)** a pull endpoint
`GET http://<node-red>:1880/tickr/<SYMBOL>` that fetches the Binance 24 h
ticker on demand and answers with a TickrDisplay payload – a battery
device's *Custom JSON URL*, plain HTTP on the LAN, no CA needed; **(2)** an
inject every 5 minutes that publishes the same payload retained to
`tickr/display` for USB devices on MQTT. Replace `<BROKER_HOST>`.

```json
[
  {"id":"tickr_tab","type":"tab","label":"TickrDisplay tickers","disabled":false,"info":"Quote proxy for TickrDisplay (docs/TICKERS.md, Appendix A)."},
  {"id":"mq_broker","type":"mqtt-broker","name":"home broker","broker":"<BROKER_HOST>","port":"1883","clientid":"nodered-tickr","autoConnect":true,"usetls":false,"protocolVersion":"4","keepalive":"60","cleansession":true},
  {"id":"n_in","type":"http in","z":"tickr_tab","name":"GET /tickr/:symbol","url":"/tickr/:symbol","method":"get","upload":false,"swaggerDoc":"","x":150,"y":100,"wires":[["n_url"]]},
  {"id":"n_tick","type":"inject","z":"tickr_tab","name":"every 5 min: BTCUSDT","props":[{"p":"payload"}],"repeat":"300","crontab":"","once":true,"onceDelay":"5","topic":"","payload":"BTCUSDT","payloadType":"str","x":150,"y":180,"wires":[["n_url"]]},
  {"id":"n_url","type":"function","z":"tickr_tab","name":"build quote URL","func":"// symbol from the HTTP path (pull) or from the inject payload (MQTT)\nmsg.symbol = (msg.req && msg.req.params && msg.req.params.symbol) ? msg.req.params.symbol.toUpperCase() : String(msg.payload);\n// Binance 24h ticker (no key). Any other JSON API: change the URL here and the field names in the next node.\nmsg.url = 'https://api.binance.com/api/v3/ticker/24hr?symbol=' + msg.symbol;\nmsg.headers = {};\nmsg.payload = undefined;\nreturn msg;","outputs":1,"noerr":0,"initialize":"","finalize":"","libs":[],"x":360,"y":140,"wires":[["n_req"]]},
  {"id":"n_req","type":"http request","z":"tickr_tab","name":"fetch quote","method":"GET","ret":"obj","paytoqs":"ignore","url":"","tls":"","persist":false,"proxy":"","insecureHTTPParser":false,"authType":"","senderr":true,"headers":[],"x":540,"y":140,"wires":[["n_fmt"]]},
  {"id":"n_fmt","type":"function","z":"tickr_tab","name":"TickrDisplay payload","func":"var d = msg.payload, sym = msg.symbol || 'ticker';\nvar out;\nif (msg.statusCode !== 200 || !d || d.lastPrice === undefined) {\n    out = { title: sym, value: 'no data', alert: { led: '000000' } };\n} else {\n    var price = parseFloat(d.lastPrice), pct = parseFloat(d.priceChangePercent);\n    var dec = price >= 1 ? 2 : 4;\n    var txt = price.toLocaleString('en-US', { minimumFractionDigits: dec, maximumFractionDigits: dec }).replace(/,/g, ' ');\n    var chg = (pct >= 0 ? '+' : '') + pct.toFixed(2) + '%';\n    // optional alert: flow variables alert_above / alert_below (set from another inject)\n    var above = flow.get('alert_above'), below = flow.get('alert_below'), last = flow.get('last_' + sym);\n    var sound = 'none';\n    if (above && last !== undefined && last < above && price >= above) sound = 'double_beep';\n    if (below && last !== undefined && last > below && price <= below) sound = 'double_beep';\n    flow.set('last_' + sym, price);\n    out = {\n        title: sym.replace(/USDT$/, '/USDT'),   // the change is drawn from change / dir, not from the title\n        value: txt,\n        change: chg, dir: pct > 0 ? 1 : (pct < 0 ? -1 : 0), age_s: 0,\n        alert: { led: pct >= 0 ? '00FF00' : 'FF0000', sound: sound }\n    };\n}\nmsg.payload = out;\nmsg.headers = { 'Content-Type': 'application/json' };\nmsg.statusCode = 200;\nmsg.topic = 'tickr/display';\nmsg.retain = true;\nreturn msg;","outputs":1,"noerr":0,"initialize":"","finalize":"","libs":[],"x":740,"y":140,"wires":[["n_res","n_mqtt"]]},
  {"id":"n_res","type":"http response","z":"tickr_tab","name":"answer pull","statusCode":"","headers":{},"x":940,"y":100,"wires":[]},
  {"id":"n_mqtt","type":"mqtt out","z":"tickr_tab","name":"tickr/display (retained)","topic":"tickr/display","qos":"0","retain":"true","respTopic":"","contentType":"","userProps":"","correl":"","expiry":"","broker":"mq_broker","x":960,"y":180,"wires":[]}
]
```

Notes: the `http response` node only fires for messages that came through
`http in` (the inject path has no `msg.res`; a `switch` on `msg.res`
silences Node-RED's warning). The MQTT message is retained, so a device that
(re)connects gets the last quote at once. A `spark` array of ≤ 48 numbers
adds the chart; keep the device's LED rule `off` when the proxy sets
`alert.led`.

## Appendix B – Home Assistant

REST sensor + automation for USB devices over MQTT:

```yaml
# configuration.yaml
sensor:
  - platform: rest
    name: btc_usdt
    resource: https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT
    scan_interval: 300
    value_template: "{{ value_json.lastPrice | float | round(2) }}"
    json_attributes:
      - priceChangePercent
    unit_of_measurement: USDT

automation:
  - alias: TickrDisplay – BTC ticker
    trigger:
      - platform: state
        entity_id: sensor.btc_usdt
    action:
      - service: mqtt.publish
        data:
          topic: tickr/display
          retain: true
          payload: >-
            {% set pct = state_attr('sensor.btc_usdt', 'priceChangePercent') | float(0) %}
            {% set chg = ('+' if pct >= 0 else '') ~ (pct | round(2)) ~ '%' %}
            {"title": "BTC/USDT",
             "value": "{{ '{:,.2f}'.format(states('sensor.btc_usdt') | float(0)) | replace(',', ' ') }}",
             "change": "{{ chg }}", "dir": {{ 1 if pct > 0 else (-1 if pct < 0 else 0) }},
             "alert": {"led": "{{ '00FF00' if pct >= 0 else 'FF0000' }}"}}
```

Battery devices need a URL to pull. Home Assistant has no built-in "serve
this JSON on GET"; the cheapest trick is a *File* notification that writes
the same payload to `/config/www/tickr/btc.json` (truncate first with a
`shell_command` – `notify.file` appends) and a *Custom JSON URL* of
`http://<ha>:8123/local/tickr/btc.json` on the device (`/local/` is served
without authentication – acceptable for a quote). Appendix A's Node-RED
endpoint is cleaner if Node-RED is available anyway.
