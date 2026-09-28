# Multi-device: several TickrDisplays as one shelf

Several TickrDisplays on one LAN find each other, one browser page shows
every screen, the user arranges the devices on a virtual shelf and assigns
content to single devices or to the whole group – including members that
sleep on battery. This document explains how it works and what the trust
model is. Endpoint tables, JSON fields and the beacon format live in
[`API.md`](API.md); the texts on the e-ink in [`DEVICE_UI.md`](DEVICE_UI.md).
Where a document disagrees with the firmware, `tickr_display/src` wins.

1. [What multi-device gives you](#1-what-multi-device-gives-you)
2. [How it works](#2-how-it-works)
3. [Groups and pairing](#3-groups-and-pairing)
4. [Relay for sleeping members](#4-relay-for-sleeping-members)
5. [Limits](#5-limits)
6. [Unverified](#6-unverified)

---

## 1. What multi-device gives you

* **Several tickers on one shelf** – each device shows its own content; the
  shelf page tells you which is which (*Identify* puts a large number on
  every screen) and lets you set the content of each one.
* **Group content** – *For all…* sends one payload or one source to every
  member: online devices at once, sleeping ones on their next wake-up.
  Ticked selections can be saved as named groups.
* **Preview of every screen** – each card is the device's real framebuffer,
  fetched only when it changed; a sleeping member shows the last frame it
  uploaded and its planned wake-up time.
* **A shelf that remembers** – positions and names are stored on the
  devices, so the shelf looks the same from any device's page.
* **Group OTA** – one `.bin` to every online USB member, a firmware URL
  parked for the sleeping ones.
* **Pairing** – a device joins a group only after someone reads a code off
  its screen; nothing on the LAN can (re)pair a device silently.

Two hardware facts shape the design: an e-ink change is a refresh that takes
seconds and wears the panel ([`DEVICE_UI.md`](DEVICE_UI.md) → *E-ink refresh
rules*), and a battery device is on the network for only a few seconds per
refresh interval.

**Not supported:** scrolling or marquee text across screens (e-ink cannot do
it); synchronised refresh (USB devices flip within a fraction of a second of
each other, but nothing enforces it and sleepers wake at different times);
content spanning several screens; waking a sleeping device from the page
(its radio is off); pairing on battery power.

---

## 2. How it works

No hub, no cloud, no coordinator: every device runs the same firmware and
serves the same page; the browser does the work that involves more than one
device.

### Discovery: UDP beacons

Every USB-powered device broadcasts a small JSON beacon on **UDP port
47000**; a battery device sends one per wake-up with the seconds until its
next wake. A beacon carries the sender's MAC (the id `tickr-XXXXXX` derives
from it), name, address, firmware version, power source and planned sleep,
plus – for group members – the group id, epoch and an HMAC tag (§3). Fields:
[`API.md` → UDP beacon](API.md#udp-beacon).

* The period adapts to the number of fresh peers the device sees:
  `T = 45 s × ⌈N/8⌉`, clamped to 45–180 s, ±20 % jitter. With group
  credentials `N` counts *members* only, so strangers' devices or spoofed
  beacons cannot stretch a member's period.
* A device sends one **probe** before its boot beacon; running peers answer
  by unicast after a short random delay, so a newcomer sees them within
  well under a second. A waking battery device uses the same probe to find
  a relay (§4).
* Receivers only *record* beacons – a beacon never triggers an action – and
  trust the UDP source address over the `ip` field. The parser
  (`logic/beacon.cpp`) is a bounded scanner that fails closed.
* `POST /api/peers {"ip":…}` probes a device on another subnet by unicast;
  the answer creates the entry with the `manual` flag. Guest-network
  isolation is not crossed in either direction.
* Nothing is sent or received in access-point (set-up / recovery) mode.

### The peer table

Each device keeps a fixed-size table (`TICKR_MAX_PEERS`, 16 by default;
`max_peers` in `/api/identity`), served by `GET /api/peers` and persisted
as `/peers.bin` – written when the table has been quiet for a while, never
per beacon, and flushed before a planned restart.

| State | Meaning |
|---|---|
| **fresh** | A beacon within the last `4 × T` (plus the planned sleep for a battery device). Fresh members drive the beacon period and `members` in `/api/group` |
| **stale** | No beacon for longer, or loaded from `/peers.bin` after a reboot and not heard since. Stale entries stay – they are the memory of sleeping neighbours |
| **member** | The last beacon carried *this* device's group id and epoch and its tag is valid under the group secret; any other epoch is "not my group". Re-learnt after a join, leave or rotation |
| **manual** | Added by `POST /api/peers`, not learnt from a broadcast |

When full, the oldest non-member is evicted first, then the oldest stale
member; **members are never evicted for a non-member**. The shelf page adds
its own view: a battery device whose last beacon is stale or older than 20 s
is **asleep** (`asleep ~hh:mm`, not polled); a device whose frame poll
failed twice is **offline** (card dimmed, retried every 30 s); a stale
peer, or one silent for 2 min, shows an amber **stale** bar while its LED is
off.

### The shelf layout

The page at `/` on every device is the shelf: a 16 × 16 grid of cards, one
per group member; non-members appear under *Nearby* with a *Pair* button.
Three stores hold the layout, in order of authority: **each device's own
slot** (`layout {x,y}` in `/api/identity`, set with `POST /api/identity`) –
the source of truth for that device; **`/layout.json` on every USB member**
(`GET`/`PUT /api/layout`) – all slots, names and saved groups, written to
every reachable USB member after each change, newest `updated_at` wins on
read, so devices that are asleep or off keep their place; and the browser's
`localStorage` (`tickr_panel`) – a cache of frames and identities, never
secrets.

A card is moved by drag-and-drop (mouse, touch, pen), with the arrow keys,
or with *Move* in the device sheet (tap the card, then the destination);
dropping on an occupied cell swaps. A move writes `POST /api/identity` to
every moved device and the whole document to every reachable USB member.
Two devices claiming one cell: fresh identity wins, then the host device,
then the lower id; the other gets the first free cell and a *position not
saved* badge until the next save.

**Identify** numbers the reachable cards 1…N in shelf order (row by row) and
asks each device to show its number for 30 s (`POST /api/screen/identify`);
pressing again clears both sides; offline and sleeping devices are skipped.

**Previews** poll `GET /api/screen/raw` with `If-None-Match`; only a changed
frame is downloaded. At most 6 requests are in flight, at least 250 ms
apart, each device roughly every 10 s (longer on big shelves and while
offline), paused while the tab is hidden. A device showing a pairing code
answers `503`; the card keeps its last frame with a *pairing…* badge.

### One page, every device

The page loaded from `http://<device-ip>/` calls every other device's
`/api/*` straight from the browser. Each device is its own origin, so the
firmware sends `Access-Control-Allow-Origin: *` on every response (also
`401`/`403`/`304`), exposes `ETag` and the `X-Screen-*` headers and answers
`OPTIONS` preflights on `/api/*`, `/update` and `/config` with `204`. `*` is
safe because no cookies are involved – authentication is the `X-Api-Token`
header ([`API.md` → Authentication](API.md#authentication)). Group commands
are executed by the browser with bounded concurrency; there is no group
endpoint on the device. The same file works from another host with
`?host=<device-ip>`.

---

## 3. Groups and pairing

Joining a device to a group – or moving it to another one – requires
physically seeing the device: a numeric code on its screen, as with
Bluetooth pairing.

**Group credentials**, held by every member in `config.json`: `group_id`
(8 random bytes, public – names the group in beacons and signatures),
`group_secret` (32 random bytes – key for beacon tags and device-to-device
signatures), `group_name` (≤ 31 characters) and `group_epoch` (incremented
on every rotation; members ignore beacons and signatures with another
epoch). No API returns the secret in clear text (`/api/identity` and
`/api/group` expose id, name, epoch; `/api/config` only `group_set`). The
one way it leaves a device is `GET /api/group/secret`, wrapped for the
caller's ephemeral X25519 public key (step 0 below).

### Roles: API token versus group secret

| | `api_token` | `group_secret` |
|---|---|---|
| Held by | The human's browser (`localStorage`) or script; per device | Every member; the browser only transiently, during one join or rotation |
| Authenticates | Browser → device: configuration, payloads, OTA, pairing and group routes, layout / peers / relay writes | Device → device: beacon tags, the sleeper's relay fetch and frame upload; the `rekey` mode of pairing |
| Never accepted for | Device-to-device traffic | Any token-protected route – a group signature never unlocks configuration, OTA or `/api/screen` |
| Obtained by | Typing it on `/system` (or *Protect this device* on the shelf) | Pairing, or – for the browser – `GET /api/group/secret` from a member |

Because the browser never stores the secret, **every group function needs a
configured API token**: without one the group routes answer
`403 {"error":"set an API token on /system first"}`, with one but without
credentials `401`. Read-only routes (`/api/peers`, `/api/layout`,
`/api/pair/status`, `/api/screen/*`, `/api/identity`) are public.

### The pairing protocol

Implemented in `logic/pairing.cpp`, `managers/pairing_manager.cpp` and
`www/src/_crypto.js` (`crypto.subtle` is unavailable on plain HTTP, so the
page carries its own X25519, SHA-256 and HMAC). Routes and status codes:
[`API.md` → Pairing and group](API.md#pairing-and-group).

An **ephemeral X25519 key agreement** between browser and device produces a
shared key; the **6-digit code** from the screen proves that the person at
the browser can see the device; a **4-letter check word** derived from the
shared key – shown on the screen *and* on the page – exposes a
man-in-the-middle. A captured transcript gives an attacker nothing to
brute-force, because the code never protects the secret by itself.

`HMAC` = HMAC-SHA-256, `HKDF` = RFC 5869 over HMAC, wire fields are standard
base64. **Labels are part of the wire protocol** – changing one breaks every
browser/device pair:

```
PRK  = HKDF-Extract(salt = n_d, ikm = K)                     K = X25519 shared secret
K_c  = HKDF-Expand(PRK, "tickr-pair-v1 confirm" || pk_i || pk_d, 32)
K_e  = HKDF-Expand(PRK, "tickr-pair-v1 box"     || pk_i || pk_d, 32)
chk  = HKDF-Expand(PRK, "tickr-pair-v1 check", 2) -> 4 letters, one nibble each,
       alphabet "ACDEFGHJKMNPRTWX" (no 0/O, 1/I/L, 2/Z, 5/S, 8/B, U/V)
c_i  = HMAC(K_c, "i" || code6)            c_d = HMAC(K_c, "d" || code6)
c_i  = HMAC(K_c, "rekey" || secret_old)   c_d = HMAC(K_c, "d-rekey" || secret_new)   (mode rekey)
Enc(K_e, m): ks = HKDF-Expand(K_e, "box-ks", len(m)); ct = m XOR ks;
             tag = HMAC(K_e, "box-tag" || ct)[0..16];  wire = ct || tag
creds = group_id (8) || group_secret (32) || epoch (2, big-endian) || name (32, NUL-padded)
box   = Enc(K_e, creds)
GET /api/group/secret: K_eg = HKDF-Expand(HKDF-Extract(n_a, K), "tickr-group-v1 box" || pk_i || pk_a, 32)
```

```
Initiator (the page, or a script)           Device D (shows the screen)
---------------------------------------     -----------------------------------------------
0. JOIN only: GET A/api/group/secret?pk=<pk_i> on a member A (A's api_token):
   202 "computing", then 200 {pk_a, n_a, box};  creds = Dec(K_eg, box)
1. sk_i random, pk_i = X25519(sk_i, 9)
   POST /api/pair/start pk=<pk_i>[, mode=rekey]
                                         <- 202 {"sid","state":"starting","mode"}
                                            main task: sk_d, pk_d, K = X25519(sk_d, pk_i), n_d random;
                                            PRK, K_c, K_e, chk;  mode code: 6 random digits,
                                            FULL REFRESH "Pairing - enter this code:" / "795 624" /
                                            "check KCJH  90s  <name>";  mode rekey: nothing drawn
   GET /api/pair/status (poll)           <- {"state":"pairing","sid","pk_d","n_d","chk",
                                            "expires_s","attempts_left","mode"}
2. K = X25519(sk_i, pk_d); same PRK, K_c, K_e, chk. The page shows chk; code
   field and Confirm stay disabled until the user ticks "the check word matches".
   POST /api/pair/confirm sid, ci[, box][, name (CREATE)]
                                         -> constant-time compare of c_i
                                            wrong: 401 + attempts_left; 3rd wrong: COOLDOWN, 429
                                            right + box: Dec, verify tag, ADOPT (JOIN / rekey)
                                            right, no box: CREATE (403 if D already has a group)
                                            config.json written, beacon credentials updated,
                                            outcome frame 5 s, FULL REFRESH back to the content
                                         <- 200 {"c_d","group":{"id","name","epoch"}}
3. check c_d (mutual confirmation); forget K, creds and the code
```

`pair/start` answers `202` with only the session id; key agreement, nonce
and e-ink refresh run on the main task, so the page collects `pk_d`, `n_d`,
`chk`, `expires_s` from `/api/pair/status` once the state is `pairing` (a
few seconds – the full refresh dominates). `pk_d`, `n_d` and `chk` are
public by construction: an attacker cannot produce a second session with
the same check word. `GET /api/group/secret` is a two-step call (box
computed on the main task, collected with a repeated `GET`, wiped after
15 s; one request per 2 s). Nothing in the exchange is reusable – session
id, nonces, keys and code die with the session.

### Screen, state machine, limits

One full refresh on `start`, one into an **outcome frame** when the session
ends for any reason (held 5 s), one back to the previous content
([`DEVICE_UI.md` → Screens](DEVICE_UI.md#screens), row 8):

```
│ Pairing - enter this code:                   │  small
│                 795 624                      │  large, centred
│ check KCJH  90s  Tickr-B2C3                  │  small; on a member: "check JJCD  90s  Shelf -> ?"
```

Outcome: *Paired: \<group name\>*, *Pairing failed* with the reason
(*timeout*, *too many wrong codes*, *box does not verify*, *already in a
group*, *config write failed*) or *Pairing cancelled*; the LED flashes green
or red for 1 s. `rekey` sessions draw nothing. **The code must not be
readable over the network:** while it is on the screen the release build
answers `GET /api/screen/raw` and `.bmp` with `503 {"error":"pairing in
progress"}`; `/api/screen/state` stays `200` with the *stored* title and
value. The developer build `tickr_dev` keeps the frame readable so the flow
can be automated without a camera – it must never be handed to users.

```
          POST start (202)        main task: X25519 ×2, keys,          confirm ok (200)
 IDLE ─────────────────► STARTING ── nonce, code, FULL REFRESH ──► PAIRING ──────────────► IDLE
  ▲                                                                │  ▲  │
  │   3rd wrong code (429) / 90 s timeout / cancel / bad box       │  └──┘ wrong code
  │   -> outcome frame 5 s -> FULL REFRESH back                    │       (401, attempts++)
  ▼                                                                ▼
 COOLDOWN (1, 2, 4, 8, 16, 32 min; start -> 429 + Retry-After) ── elapsed -> IDLE
```

* **6 digits**, displayed as `482 913`, from the hardware RNG with rejection
  sampling. The code only has to defeat *online* guessing: **3 attempts per
  session**, then a cooldown doubling per exhausted session in a row (reset
  after 1 h without a wrong code). **90 s** per session, one session per
  device (`409`).
* **Pairable = USB power, nothing else.** On battery `POST /api/pair/start`
  answers `403 {"error":"pairable only on USB power"}`, `/api/identity` says
  `pairable:false` and the page disables *Pair*. Checks on `start`, in
  order: power, cooldown, open session, free heap (`503` when low).

### What the group secret signs

* **Beacon tags** – `HMAC(group_secret, "tickr-beacon-v1\n" mac "\n" n "\n"
  ip "\n" pw "\n" sl "\n" seq "\n" g "\n" ep)`, first 8 bytes. A receiver
  marks the sender `member` only when `g` and `ep` equal its own id and
  epoch *and* the tag checks (constant time). A replayed beacon only
  re-asserts a presence that was true.
* **Device-to-device requests** – the sleeper's `GET /api/relay/<id>` and
  `PUT /api/peers/<id>/screen` carry one `X-Tickr-Group` header: group id,
  epoch, a relay-issued nonce and an HMAC over method, path, nonce and body
  hash. Devices have no clock, so nonces replace timestamps: the relay
  issues one in its probe reply (`rn`) and one in every accepted answer
  (`X-Tickr-Nonce`), each valid 30 s and usable once, burnt *before* the
  HMAC check. Format: [`API.md` → Relay signature](API.md#relay-signature).
* **Membership without a round trip** – a device knows from the beacon
  alone whether a peer is in its group (`members` in `/api/group`).

### Re-pairing, leaving, rotation

* **Re-pair to another group:** the same protocol with a `box` carrying the
  new credentials – **the code is required again**; the screen shows
  `<old group> -> ?`. The next beacon carries the new id, epoch and tag, so
  the old members flip the peer to `member:false` within one period.
* **Leave** (`POST /api/group/leave`, token, no code): credentials wiped,
  one unsigned beacon at once so members flip the peer within seconds. The
  same function serves *Leave group* in recovery mode
  (`POST /api/recovery/group/leave`). Leaving is less dangerous than
  joining – the token already allows reflashing.
* **Forget a lost device:** `DELETE /api/peers/<id>` on the members, plus a
  rotation if it may leak the secret.
* **Rotation – routine** (*Rotate secret* on the page): fetch the current
  credentials, `POST /api/group/rekey` (new random secret, `epoch + 1`, same
  id and name), fetch the new credentials, then a `mode=rekey` pairing
  against every member on USB – no screen, no code; the proof is
  `HMAC(K_c, "rekey" || old secret)` and the new credentials travel in the
  `box`. A sleeping member cannot get them through the relay (its
  signatures carry the old epoch), so **sleeping members must be paired
  again**; the page says so before it starts.
* **Rotation – compromise:** `rekey` is useless when the attacker holds the
  old secret. Every device is re-paired **with its code**, one by one
  (`leave`, then JOIN from the first re-created group); devices with an
  older epoch are ignored until re-paired. There is deliberately no
  "trusted window after a power-cycle" – it would let anyone on the LAN who
  notices a reboot re-key the device.

---

## 4. Relay for sleeping members

A battery device connects, sends one beacon, fetches once, refreshes once
and sleeps; it cannot be reached in between. Content and updates for it are
therefore **parked on a USB member** – the *relay* – and picked up on the
next wake-up. Every USB-powered group member is a relay (`relay:true` in
`/api/identity`); the page uses the device it was opened on if that is one,
otherwise the first online USB member. Routes: [`API.md` →
Relay](API.md#relay-content-for-sleeping-members).

**What a relay parks**, per sleeping member, written by the page with the
API token: a **payload** (`PUT /api/relay/<id>`, one JSON object – the
sleeper's renderer validates it) from *For all…* with a *Text* source, the
result line reading *parked on \<relay\> – shown on its next wake*; and a
**firmware URL** (`PUT /api/relay/<id>/ota`, plain `http://`) from the
*Updates* card. A relay refuses to park on battery power or without a group
(`403 "not a relay"`) and for an id that is not a member (`404`). A change
of *source* (ticker, Pull URL) cannot be parked – the page reports *asleep –
plug it in to change its source* – and a single sleeping device's own
*Change…* editor disables *Send* with *plug it in (USB)*.

**Pick-up on wake.** After Wi-Fi and its wake-up beacon, a battery device
with **no Pull URL** and group credentials runs the relay client
(`relay_client_run()` in `managers/relay_manager.cpp`); the source order is
*explicit Pull URL → relay → the WAITING card*:

1. **Find the relay:** one unicast probe to the relay used last (kept in RTC
   memory, 600 ms wait); on a miss one broadcast probe (900 ms). The first
   announce from a USB member with a valid tag and a relay nonce, not the
   device itself, is used.
2. **Fetch:** signed `GET /api/relay/<own id>?consume=1` – `200` with the
   payload or `204`; `X-Tickr-OTA-URL` names a waiting update. `consume=1`
   deletes both files after delivery, so a payload is shown once.
3. **Render** the payload.
4. **Update**, if a URL waited: the device fetches and flashes the image
   itself and restarts (the `update_from_url` path).
5. **Report** its frame with a signed `PUT /api/peers/<id>/screen` – only
   when the refresh interval is ≥ 10 min. The relay serves it as
   `GET /api/peers/<id>/screen` (public) for the previews.
6. Sleep.

Nothing pending → the WAITING card, once per discharge. No relay or a failed
fetch → the normal battery back-off (nothing drawn on the first miss, the
OFFLINE card from the second). MQTT never runs on battery.

**What the page shows:** `asleep ~hh:mm` from the planned sleep in the last
beacon (a plan, made optimistic by the back-off, hence "~"); `pending` when
the relay holds a payload or an update for the device (`GET /api/relay` is
read on every presence refresh; the device sheet lists what is parked and
offers *Clear pending*); the last uploaded frame as the preview, with its
age when the relay knows it, else the browser's cached frame with a *cached
hh:mm* badge.

**Group OTA.** The *Updates* card in the *…* drawer takes a `.bin` file
and/or a URL. The file goes (`POST /update`) to every online USB member in
turn, the device the page runs on last (the page reloads after it). The URL
is parked for every sleeping member, which flashes on its next wake.
Per-device result lines report progress and errors.

---

## 5. Limits

* **Peers:** 16 per device (`TICKR_MAX_PEERS`, compile-time); members are
  never evicted for non-members.
* **Layout document:** ≤ 4096 bytes, ≤ 64 devices, ≤ 8 saved groups, names
  ≤ 31 ASCII characters, slots 0–15. With many devices *and* large groups
  the body limit is tight.
* **Parked payloads:** ≤ 4096 bytes, one per sleeping member (a new one
  replaces it).
* **Pairing:** one session per device, 90 s, 3 attempts, then cooldown; USB
  power only.
* **Sleeper previews:** uploaded only at refresh intervals ≥ 10 min; shorter
  intervals leave only the browser cache.
* **Rotation** reaches USB members only; sleeping members are paired again.
* **Relay OTA** and `update_from_url`: plain `http://` only.
* **Big shelves:** polling is bounded and presence comes from the peer
  tables, but there is no zoom/pan, paging or per-room grouping.

---

## 6. Unverified

The following exists in the firmware and the page and is covered by host
tests, but has not been exercised on hardware:

> **Unverified:** the sleeper flow end to end – wake-up beacon with the
> planned sleep, `next_wake_s` on the receivers, relay lookup, signed fetch,
> rendering a parked payload, frame upload, and the `asleep` / `pending`
> badges that depend on it; the `403 "pairable only on USB power"` refusal
> on a real battery-powered device.

> **Unverified:** group OTA from the *Updates* card, and a parked firmware
> URL being flashed on a sleeper's wake-up.

> **Unverified:** the two-device `rekey` rotation and re-pairing a member to
> a different group.

> **Unverified:** fleets of more than two devices – beacon period
> adaptation, peer-table eviction under load, the polling scheduler with
> tens of devices.

> **Unverified:** touch drag-and-drop on a real phone (*Move* uses plain
> taps).

> **Unverified:** the outcome frames *Paired: …* and *Pairing failed* have
> not been seen on a device; only *Pairing cancelled* (same code path).
