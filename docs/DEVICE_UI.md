# Device UI: what the e-ink, the LED bar and the speaker say

The panel is 296×128, landscape, drawn through a shadow canvas (4736 B)
that is blitted to the SSD1680; the canvas *is* what `/api/screen/raw` and
`/api/screen.bmp` serve, so the shelf tile in the browser always shows the
real frame. Fonts: FreeSansBold 9 / 12 / 18 pt. A **full** refresh takes
≈ 4.1 s with the visible inversion flash; a whole-frame **differential**
("partial") refresh ≈ 0.75 s without it – which one a frame gets is decided
by one pure function ([E-ink refresh rules](#e-ink-refresh-rules)). All
on-device strings live in `src/logic/ui_strings.h`.

Principles: the content is the product – everything else is a *card* that
interrupts it or a *service screen* while the user is at the device. A card
is a glyph and one 18 pt line (12 pt second line), readable from two metres
and in the shelf thumbnail. No status words or addresses on content; the
address appears only where the user has to type it (set-up, recovery,
WAITING). A *transition* is shown once for 5 s, a *condition* until it
ends. Every condition has a debounce and a rate limit; a signal that
flickers never reaches the panel.

## Screens

| # | Screen | When | What is on it |
|---|---|---|---|
| 1 | **Content** | Every payload with `title`/`value` (text look) or with `change` / `spark` (ticker look, [`TICKERS.md`](TICKERS.md)) | Text: title 9 pt top-left; value centred, cascade 18 pt ×2 → 18 → 12 → 9, truncated with `...`. Ticker: title left and change with a triangle on the 9 pt row, price centred, sparkline ≤ 200×24 bottom-left, age line 9 pt bottom-right – or, in its place, a payload's `time` string / the funding line of a Binance perpetual (`FR +0.0100%`, [`TICKERS.md`](TICKERS.md) → *Binance futures*). Badges bottom-right corner |
| 2 | **WAITING** | No payload shown on this boot: after the first connect, after a service frame ends, on a battery device without a Pull URL | Panel icon 40×28; *Ready* 18 pt, *choose content at* 12 pt, `http://<ip>/` 12 pt (9 pt for a 15-char address); badges as on content |
| 3 | **Badges** | Wi-Fi lost ≥ 60 s; bolt ↔ battery after 5 s of a stable new reading; **BATTERY LOW** at ≤ 15 % on battery with a known level | Right-aligned in rows 112–127: `[Wi-Fi lost] [NN%] [battery]` or `[Wi-Fi lost] [bolt]`; low = battery outline with an exclamation mark; outline only when the level is unknown (board `?`); **nothing** for `power: unknown` |
| 4 | **BATTERY EMPTY** card | Battery mode, cell < 3300 mV; drawn once per discharge, then 60-min sleeps until the cell is > 3450 mV or USB is found | Empty battery glyph 80×40 above *Battery empty* 18 pt / *connect USB power* 12 pt. Never served by `/api/screen.*` – the device sleeps right after |
| 5 | **Failed battery wake** | Wi-Fi or pull failed on a battery wake | 1st miss: nothing drawn, LED red 300 ms; 2nd miss and every 4th after it: the OFFLINE card (row 10) with *last update N min / h ago* = minutes slept since the last successful frame (no line before the first success of a discharge) |
| 6 | **Wi-Fi setup** / **Recovery mode** (access-point card) | Captive portal open (no credentials, or the USB connect failed) / a power-cycle series completed on USB; recovery is held ≤ 30 min | *Wi-Fi setup* or *Recovery mode* 18 pt, *Join Wi-Fi: TickrDisplay* 12 pt, *open http://192.168.244.1* 12 pt; recovery adds a 9 pt note (*Exits after 10 min without clients*, *Access point starting...*). No glyph – with one the 12 pt lines do not fit |
| 7 | **Boot splash** / **Restart N of 3** / **Recovery: USB only** | Every cold USB start / a power-cycle series in progress, or completed on battery | *TickrDisplay* 18 pt, version 9 pt, *Recovery: switch off/on now* 9 pt – no count on the first frame; frames 2/3 carry *Restart N of 3* and the hint |
| 8 | **Pairing code** / **outcome** | `POST /api/pair/start` on USB; the outcome is held 5 s | *Pairing - enter this code:* 9 pt, code 18 pt centred, check word / TTL / name 9 pt; outcome 18 pt + detail. Hidden from `/api/screen.*` (`503`) |
| 9 | **Identify** | `POST /api/screen/identify {"n","ttl_s"}`, default 30 s | Digit ≈ 100 px, device name 9 pt; whole panel, no badges |
| 10 | **OFFLINE** card | USB: 10 min of continuous Wi-Fi outage, once per outage; a reconnection counts only after it held 30 s. Battery: row 5 | Crossed-Wi-Fi glyph 49×26 on top, *No Wi-Fi* 18 pt, *last update N min ago* (→ *N h ago* from 60 min) 12 pt; no age line on a device that never showed content. New content replaces it |
| 11 | **OTA** card | Start of any flash: `/update`, `update_from_url`, ArduinoOTA | Ring-arrow glyph 45×45, *Updating* 18 pt / *keep the power on* 12 pt; LED blue steady, green 3 s on success until the deferred restart; a failed upload restores the base frame within 1 s |
| 12 | **ON BATTERY** card | Debounced USB → battery flip while awake, held 5 s, one card per 10-min window; never at boot, never for battery → USB, never on the 3rd flip inside 10 min (*flapping*: badge only) | Battery glyph 80×40 at the level (outline only on board `?`), *On battery* 18 pt and – only when the restart into the battery flow follows – *updates every N min* 12 pt (N = refresh interval, 60 when unset); LED amber flash 0.5 s, no sound |
| 13 | **Power-mode restart** | 120 s after the debounced flip (600 s when flapping or when the previous reading stood < 60 s); deferred while OTA / identify / pairing / recovery / portal hold the panel | Nothing drawn: the device restarts into the battery flow with an RTC marker; the next boot keeps the content on the panel (no clear-to-white) and the first battery pull replaces it |

`power: unknown` (unrecognised board `?`): no card, no power glyph, the
device behaves as USB; `/api/screen/state.status` says `power: "unknown"`
and `/system` offers the override. A configured override draws the glyph
of the *configured* source.

## State machine

```
                       cold USB start            3× within 20 s
  ┌──────────┐  ─────────────────────►  SPLASH ───────────────► RECOVERY (service, ≤ 30 min)
  │ power on │                            │
  └──────────┘  battery wake ────────┐    │ no credentials / USB connect failed
                                     │    ▼
                                     │  SETUP (service: AP + instructions; LED white)
                                     │    │ joined
                                     ▼    ▼
                                   ┌──────────────────────────────────────────────────┐
   USB: payload / pull / MQTT ───► │ CONTENT   fresh ──(no update for T_stale)──► stale│ ◄── back from any card
   battery: 1 pull per wake ─────► │           (ticker · text)               + badges │
                                   └───────┬──────────┬───────────┬───────────┬───────┘
      nothing to show yet ───► WAITING     │          │           │           │
      ("choose content at                  │          │           │           │
        http://<ip>/", once)               │          │           │           │
                                           ▼          ▼           ▼           ▼
                                  transitional    OFFLINE       BATTERY     service
                                  card (5 s):     (condition,   (conditions) PAIRING code → outcome
                                  ON BATTERY      after T_long) LOW badge    IDENTIFY
                                                  card + "last  EMPTY card   OTA "keep power on"
                                                  update N min" → sleep     RECOVERY
```

**Priority** when several want the panel (highest first): RECOVERY / SETUP
→ OTA → PAIRING (already `409`s Identify) → IDENTIFY → BATTERY EMPTY →
transitional POWER card → OFFLINE card → CONTENT with badges. A
lower-priority card that becomes due while a higher one is up is *dropped*,
not queued; a condition re-evaluates when the higher state ends and is
drawn by the restore.

**Implementation.** `logic/device_state` is a pure machine polled once a
second by `main.cpp` with the raw link state, the *debounced* power reading
of `logic/status_policy`, the quantised battery level, a content counter,
the OTA flag and the service frame on the panel. It returns the card the
base frame should be (`none | offline | ota | battery_empty |
power_to_battery`), the LED facts (`batt_low`, `led_offline`,
`power_flapping`) and the switch verdict `power_switch` with
`power_grace_s`. Condition cards are set with `display_set_card()`: the
card *becomes the base frame* until `none` or new content, so a restore
after a service frame and a badge refresh draw it too. One refresh per card
change, never a queue; a badge refresh due in the same second is folded
into it; the restore after an outage carries the current badges; a content
refresh while a card is up replaces the card for the rest of that
condition. Wi-Fi flaps shorter than the 30 s hold neither end an outage nor
restart its timer. The battery flow (wake → fetch → sleep) does not run the
machine; it uses two helpers (`device_offline_card_due()`,
`device_offline_age_line()`).

## Card texts and layout notes

* **CONTENT.** Battery badge: outline with a fill proportional to the
  level and the percentage in 9 pt; on USB the bolt is the only power mark.
  Ticker: price cascade 18 pt ×2 → 12 pt ×2 → 18 → 12 → 9 – a bare number
  of 1 000 or more that misses 18 pt ×2 is retried there without its
  fraction before the cascade steps down ([`TICKERS.md`](TICKERS.md) →
  *What the screen shows*); the sparkline
  is shortened to keep clear of a long age line; the age line reads *just
  now* under 60 s, `N min / h / d ago`, `stale N …` past `T_stale`, or a
  `time` string verbatim. A title/value payload keeps the text look.
* **WAITING.** The address is the last one the device had; when Wi-Fi is
  down the card keeps it and the crossed badge says why it will not answer.
* **ON BATTERY.** The second line states the promise and is drawn only
  when the restart follows. **No ON-USB card**: battery → USB shows the
  content with the bolt through the badge refresh. A flip to or from
  `unknown` is never a transition.
* **BATTERY LOW** is a *badge*, not a card: at ≤ 15 % (≈ 3620 mV) the
  battery outline gets an exclamation mark and the LED goes off;
  hysteresis = the 5 % display step; no refresh of its own – it appears
  with the next content or badge refresh. `status.batt_low` in the API.
* **BATTERY EMPTY.** At < 3300 mV, once per discharge (RTC flag), then
  60-min sleeps; the panel keeps the image without power. Reachable on
  both board revisions; board `?` gets neither this nor the LOW badge.
* **OFFLINE.** Staged: after 60 s the crossed-Wi-Fi *badge*, after 10 min
  the *card* – a 10-minute-old quote with a marker is still information, a
  2-hour-old quote presented as live is misinformation.
* **OTA.** Drawn for the whole flash from `/update`, `update_from_url` and
  ArduinoOTA; a failed or aborted upload restores the base frame at once;
  the reboot splash ends it otherwise.
* **Splash.** The only place the brand and the version appear. Its third
  line is one action, no count; the count belongs to frames 2/3 of a
  power-cycle series.

## Wi-Fi link supervision

The OFFLINE card says what the panel knows; getting the link back is the
job of a supervisor that runs on the awake (USB) device next to the
state machine (`logic/wifi_supervisor`, `managers/wifi_link`). The Wi-Fi
core's own auto-reconnect is one attempt per disconnect event and stops
for good on several reason codes (a router that rejects the
authentication while it boots, a band-steering disassociation), so the
firmware does not rely on it alone:

* **Link down** (`WiFi.status()` not connected) for **25 s** → the device
  disconnects and calls `WiFi.begin()` with the saved network itself, then
  again after **15 s, 30 s, 60 s, 60 s, …**; the back-off resets when the
  link is back.
* **The core gave up** (`WL_CONNECT_FAILED`, `WL_NO_SSID_AVAIL`) → the
  first own attempt after **10 s**.
* **Associated without an address** (the DHCP lease lost and not renewed):
  a fresh association gets **2 min** before it is torn down and re-made,
  which also restarts the DHCP client.
* **30 min** without a connection → `ESP.restart()`. It is a software
  reset: it does not count towards recovery mode, and the RTC copy of the
  frame lets the panel resume with a partial refresh.
* Nothing is done while the set-up portal or the recovery access point is
  up (they drive the station themselves) or while a firmware image is
  being written; the timers keep running. The battery flow does not run
  the supervisor – every wake-up is a fresh `WiFi.begin()`.

Each counted loss (a link that had an address and lost it, the core's own
retries are not counted) prints one serial line with the driver's reason
code, as do the supervisor's attempts and the restart. `GET /api/status`
reports `wifi_disconnects`, `wifi_last_reason`, `wifi_down_s`,
`wifi_reconnects` and `wifi_restarts` (see [`API.md`](API.md) → *Status
and configuration*); the counters live in RTC memory, so they survive the
supervisor's restart, an OTA restart and deep sleep, and start from zero
after a power-on.

The device also announces a **hostname** to the DHCP server:
`<device-name>-XXXXXX`, the user's device name reduced to an RFC 1123
label (letters, digits, `-`; space and `_` become `-`, everything else is
dropped, lower-case, `tickr` when nothing is left) plus the last three
bytes of the MAC address, at most 32 characters – `Shelf Left` on the
device `02:00:00:A1:B2:C3` is `shelf-left-A1B2C3`. A renamed device announces the
new name from its next boot; `/api/system/info` → `wifi.hostname` shows
the one in use.

## Power-mode switch

The power mode is decided in `setup()`: a device that finds USB runs awake
with Wi-Fi, a device on battery wakes, fetches and sleeps. Two runtime
transitions matter:

* **USB → battery while awake** – detectable on both revisions
  ([`HARDWARE.md`](HARDWARE.md) → *Power sensing*). The ON BATTERY card
  states the fact; the restart into the battery flow makes the promise
  true. The switch lives in `logic/device_state` next to the card (they
  share the flip / flapping bookkeeping); `main.cpp` acts on `restart`.

  | Event | Card | Switch |
  |---|---|---|
  | Boot reading (first poll) | none – not a transition | `idle` (or `off`) |
  | USB → battery, the USB reading stood ≥ 60 s, ≤ 2 flips in the 10-min window | **ON BATTERY** 5 s, LED amber 0.5 s | `grace` **120 s** (`DS_POWER_GRACE_MS`) |
  | USB → battery within 60 s of the previous change (`DS_POWER_STABLE_MS` – the boot detector settling) | card | `grace` **600 s** (`DS_POWER_GRACE_LONG_MS`) |
  | 3rd flip inside 10 min (`DS_POWER_FLAPPING_FLIPS`) | none – badge only, log *flapping* | `grace` 600 s of uninterrupted battery |
  | Battery → USB (any time) | none (content with the bolt via the badge refresh) | cancelled silently → `idle` |
  | Reading → `unknown` | none | cancelled → `idle` |
  | Grace over while OTA writes / identify / pairing / recovery card or AP / portal | – | `blocked` – deferred, not dropped; restart on the first free poll while still on battery |
  | Grace over, nothing blocks | – | `restart` → restart into the battery flow |
  | `switch_allowed` false (override ≠ `auto`, no Pull URL, locked) or board `?` | card **without the second line** | `off`, never a restart |
  | `switch_allowed` withdrawn during the grace (URL removed, override set) | – | cancelled → `off` |
  | Content refresh / OFFLINE card during the grace | as usual | unaffected |

  **Restart mechanics.** RTC marker, `display_prepare_sleep()`,
  `ESP.restart()`; the next boot sees the marker, keeps the "content shown"
  flag (so a later failed wake's age line counts from the content on the
  panel) and initialises the display without a clear-to-white; the first
  battery pull replaces the content – exactly one refresh for the switch
  plus the card's two. **Guards against a restart loop:** override ≠
  `auto` → `off`; no Pull URL → `off` (the battery flow would only show
  WAITING); board `?` → `off`; a flip within 60 s of the previous change or
  a flapping source → the long grace; a boot after a power-mode restart
  that reads USB **locks** the switch `off` until a power cycle – a misread
  source costs one restart. A boot on battery is never a transition, so a
  battery device awake for configuration never restarts. A device that
  *booted* on battery without a URL and receives one while awake keeps
  running as USB until a power cycle.

* **Battery → USB while asleep.** The device wakes on the RTC timer only –
  there is no GPIO wake on USB – so USB is noticed at the next scheduled
  wake at the earliest (up to the refresh interval later). That wake draws
  nothing until the scheduled pull (3 s) draws the content with the bolt;
  the panel keeps the last battery frame meanwhile.

* **Weak adapters behind the stacking pads** (≈ 4.3 V → "battery"): the
  hysteresis holds the state once entered, but a unit that boots on the
  pads may boot as battery; the `power_source` override exists for that. A
  stack whose pads do not make contact is a genuine "upper unit on battery"
  – the detector says `battery`, the unit deep-sleeps between refreshes and
  becomes a sleeper in the panel.

## E-ink refresh rules

Every frame request renders into the shadow canvas and asks
`logic/refresh_policy` (`refresh_decide()`, host-tested) once; the single
decision point is `present()` in `hal_display.cpp`. The policy is
**partial-first**: a differential refresh is the default, a full refresh is
reserved for frames where a differential would be wrong or ghosting must
be cleaned.

| Verdict | Meaning | Panel |
|---|---|---|
| `none` | the rendered frame is byte-identical to the shown one | nothing; `refreshes_skipped`++ |
| `defer` | a content / badge partial less than 30 s after the previous partial | drawn by `display_loop()` when the spacing has passed – never dropped |
| `partial` | whole-frame differential refresh: the controller drives only the pixels that differ between the shown frame (rewritten from the RTC copy) and the new one | ≈ 0.75 s, no inversion flash |
| `full` | OTP waveform, cleans ghosting; optionally preceded by a clear to white (`RP_DOUBLE_FULL_AFTER_CARD`, default 0) | ≈ 4.1 s |

**Decision order** (`refresh_policy.cpp`):

| Condition | Kind |
|---|---|
| The copy of the shown frame is not trustworthy – first frame of a boot, CRC mismatch, reset by power-on / EN / brownout / crash, or the driver is about to clear the panel | **FULL** |
| 24 h hygiene: `FULL_REFRESH_MAX_AGE_MS` without any refresh (`display_loop()` redraws the base frame) | **FULL**, never skipped |
| Rendered frame equals the shown frame (any other event) | **NONE** |
| Condition card in (OFFLINE / OTA / BATTERY EMPTY); boot frames (splash, *Restart N of 3*, *USB only*), SETUP and RECOVERY cards | **FULL** |
| Layout kind changed (text ↔ ticker ↔ WAITING) or the ticker's source changed against the last *base* frame | **FULL** |
| Forced full: on USB `partials_since_full ≥ RP_FORCE_FULL_EVERY` (8) **or** `ms_since_full ≥ RP_FULL_MAX_AGE_MS` (60 min); on battery `partials_since_full ≥ RP_FORCE_FULL_BATT` (6) – a wake counter, there is no clock across sleeps | **FULL** |
| Content / badge partial less than `RP_PARTIAL_MIN_MS` (30 s) after the previous partial – USB only; the first partial after a full is never held | **DEFER** |
| Everything else: content of the same layout kind, badge change / stale crossing, identify, pairing code / outcome, the transitional ON BATTERY card, the base frame back after any card or service frame, a battery wake with a valid copy | **PARTIAL** |

With `RP_FORCE_FULL_EVERY 8` the USB period is 9 frames: at a 1-min ticker
one inversion flash per 9 min, at 5 min one per 45 min. The constants are
tuned values in `refresh_policy.h`, not settings.

**Event classes** (`RefreshEvent`): `CONTENT` (payload / MQTT / pull /
ticker, also the WAITING card), `BADGE` (status_policy's minute rule
already applied), `SERVICE` (identify, pairing, ON BATTERY card), `RESTORE`
(base frame back), `CONDITION`, `BOOT`, `HYGIENE`. The layout kind is
tracked for base content frames only, so the content back after OFFLINE
compares against its own kind and comes back partial.

**Refreshes per event:** new content of the same layout kind – 1 partial,
every 9th a full, 0 when identical; layout or source change – 1 full;
badge change alone – 0 (drawn with the next content), an urgent badge
(Wi-Fi lost, power flip, stale crossing) ≤ 1 partial per 60 s; ON BATTERY
card – 2 partials; condition card – 1 full in, the way back a partial (OTA
→ splash, EMPTY → sleep); identify – 2 partials; pairing – 3 partials;
SETUP / RECOVERY – 1 full + 1 partial back; USB cold boot – 2 fulls (splash
+ first content / WAITING); battery wake – exactly 1 partial or 1 full, or
0.

**The copy of the shown frame** lives in RTC slow memory (`RTC_NOINIT_ATTR`,
4748 B: the pixels, `partials_since_full`, layout kind, condition-card flag,
title CRC, CRC-32), written after every blit. It is trusted at boot only
when the reset kind is a deep-sleep wake or a software restart **and** the
CRC matches **and** the driver is not about to clear the panel; otherwise
the counters restart and the first frame is full. Consequences: deep-sleep
wake → partial; the OTA restart and the power-mode restart → the first
frame may be partial; power cycle / crash → full. Never a differential
against an untrusted image.

**Transitions – hysteresis, debounce, rate limit:**

| Transition | Detector hysteresis | Debounce (1 s polls) | Rate limit on the panel |
|---|---|---|---|
| USB ↔ battery | 4500 / 4350 mV rail (rev A), +80 / −30 mV difference (rev B) | 5 polls | one card per 10 min; a 3rd flip inside the window changes the badge only. Mode switch: restart 2 min after the debounced flip, 10 min when flapping or when the previous reading stood < 60 s |
| Wi-Fi up → down | `WiFi.isConnected()` | 60 polls (`T_short`) → crossed badge | ≤ 1 badge-only refresh per 60 s |
| Wi-Fi still down | – | 10 min (`T_long`, USB) → OFFLINE card; own reconnects from 25 s on, restart at 30 min (*Wi-Fi link supervision*) | once per outage; the reconnect must hold 30 s before the content comes back |
| Battery ≤ 15 % | 5 % display step | reading at wake (battery) / 60 s spacing (USB) | badge only |
| Battery < 3300 mV | 3300 / 3450 mV | one reading per wake | once per discharge |
| Content stale | – | `T_stale` = 3 × refresh interval, min 10 min (the payload's own `age_s` counts) | age line only; one refresh at the crossing. A *source* failure (HTTP 500, extract error) is an age line, never a red LED |

**Battery flow.** Every wake is one refresh – partial when the RTC copy is
valid and fewer than 6 wakes were drawn partial, else full – and the rules
decide *which* frame: USB found → the USB flow (content with the bolt after
the scheduled pull, no card); cell < 3300 mV → EMPTY card once, sleep
60 min; Wi-Fi or pull fails → nothing on the 1st miss, the OFFLINE card on
the 2nd and every 4th after it; success → content with the level read this
wake (no Wi-Fi bars on battery – the radio is off within seconds). A
USB → battery card never happens on battery: a device that boots on battery
simply draws content.

**Ghosting.** Differential refreshes leave faint ghosts of what changed;
the forced fulls (8 partials / 60 min on USB, 6 wakes on battery, the 24 h
floor) bound them. The partial LUT has no temperature compensation – in a
cold room partials look weaker until the next full. A full that replaces a
long-lived condition card can be preceded by a clear to white
(`RP_DOUBLE_FULL_AFTER_CARD`, default off). After deep sleep the display is
initialised without a clear-to-white.

`/api/screen/state` exposes the counters: `refreshes_full`,
`refreshes_partial`, `refreshes_skipped` (folded badges and identical
frames), `last_full_s`, `partials_since_full`; the serial log has one line
per verdict (`Refresh: partial (event 0, 3 partials since full)`).

## LED and sound

`hal_indication` keeps **one base colour** (the payload's `alert.led`, or
red / green by direction when `led_rule = sign`) and an **overlay layer**
with a priority mask and a one-shot flash; `indication_loop()` drives
blink / breathe / pulse from the main loop. The lowest number wins; the
base colour returns after every overlay. `GET /api/status.led` reports the
colour actually driven, overlay included.

| Priority | State | LED | Sound |
|---|---|---|---|
| 1 | OTA flashing | blue steady; success: green 3 s until the deferred restart | none |
| 2 | RECOVERY active | white, slow breathe (1 per 2 s) | none |
| 3 | SETUP portal | white steady | none |
| 4 | PAIRING code shown | amber steady; outcome: green 1 s / red 1 s | none |
| 5 | IDENTIFY | white blink 1 Hz for the TTL | none |
| 6 | BATTERY EMPTY | off | none |
| 7 | BATTERY LOW badge | off | none |
| 8 | OFFLINE (after `T_long`) | base colour off; one amber pulse every 10 s on USB | none |
| 9 | ON BATTERY card | amber flash 0.5 s when the card is drawn | none |
| 10 | Battery wake end | green / red 300 ms (direct write, no loop on the battery flow) | none |
| base | CONTENT | `alert.led`, or red / green by `dir`; off when the rule is `off` | payload `alert.sound` |

A fetch / extract error never turns the LED red (red means "down");
overlays run on USB only except 6 and 10; sound is never used for
conditions – it stays a payload matter.

## The panel shows the same frame

The shelf tile draws `/api/screen/raw` at 200 px wide (0.68 ×): 18 pt →
17 px and 12 pt → 12 px stay readable, 9 pt does not – state cards use
18 / 12 pt only. Thick strokes (4 px) survive the scaling; hairlines
vanish. `GET /api/screen/state` names what is on the panel: `state`
(`boot · setup · waiting · content · pairing · recovery · ota · identify ·
offline · battery_empty · power_to_battery`), `card`, `stale_s`,
`power_switch`, `power_grace_s`, the refresh counters and the badge
snapshot `status {wifi, bars, usb, power, batt_pct, batt_low, ip}`; the
tile keys its badge on `state` and hides the "live" pulse when `state ≠
content`. Field list: [`API.md`](API.md) → *Screen state fields*. A sleeper
uploads the frame it drew to a relay; a card frame is as valid as a content
frame, so the shelf shows the sleeper's last card.

## Unverified

> **Unverified:** the BATTERY LOW badge and the BATTERY EMPTY card have not
> been seen on hardware (they need a cell at ≤ 15 % / < 3.30 V or a lowered
> threshold in a dev build).

> **Unverified:** the OFFLINE card sequence on USB (10 min without the
> router, card, restore after the 30 s hold) has not been exercised on
> hardware as a staged test; likewise the link supervisor's own reconnects
> and its 30-min restart (host-tested only – a router outage on the bench
> is still owed).

> **Unverified:** the runtime USB → battery switch (cable pulled from an
> awake unit: card, grace, restart into the battery flow) and the
> partial-refresh battery wake are host-tested only; the awake-time saving
> of a partial wake (≈ 3.3 s) is an estimate from the driver constants.

> **Unverified:** the LED breathe period (2 s) and the amber tint
> (255, 120, 0) have not been judged on the bar; the double full after a
> long-lived card (`RP_DOUBLE_FULL_AFTER_CARD`) has not been compared against
> the default on a dense ticker frame.
