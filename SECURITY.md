# Security policy

## Threat model – read this first

TickrDisplay is designed for a **trusted home/lab LAN**.

* The web UI and HTTP API are served over **plain HTTP**. Access to anything
  that changes state is protected by one shared API token – **once you set
  one**. Until then anyone on the same network can change the display, play
  sounds, change the MQTT/pull configuration and upload firmware. The shelf
  offers to generate a token on first open (*Protect this device*).
* **Authentication model** – one shared API token, accepted as
  `X-Api-Token: <token>` or as HTTP Basic (any user name, password = token)
  and compared in constant time. It is **required** for everything that
  mutates state, uploads or reveals a secret – every `POST`/`PUT`/`DELETE`
  (incl. `POST /update` and `POST /api/source/test`, which makes the device
  fetch a URL), `GET /api/config`, the raw partition and flash downloads
  (they contain the Wi-Fi password and all secrets), `GET /api/group/secret`,
  `GET /api/wifi/scan` – and **not required** for the pages and read-only
  data without secrets (`/api/status`, `/api/identity`, `/api/screen/*` –
  withheld with `503` while a pairing code is shown – `/api/peers`,
  `/api/layout`, `/api/system/info`, `/api/power/raw`, `/api/wifi/status`).
  Route by route: [`docs/API.md`](docs/API.md). Without a token the pages
  are read-only with an inline token field; the browser's Basic prompt is
  never triggered. The token is kept in the browser's `localStorage`
  ([`docs/WEB_UI.md`](docs/WEB_UI.md) → *API token in the browser*) – a lost
  or shared browser profile leaks it; revoke it on `/system` or in recovery
  mode. Group functions refuse to work (`403`) on a device without a token.
* MQTT traffic is unencrypted (`PubSubClient` over TCP). Use a broker on your
  LAN and a dedicated username/password.
* **Outbound TLS, per source kind** ([`docs/TICKERS.md`](docs/TICKERS.md) →
  *TLS*). The **ticker** – the presets CoinGecko, Kraken, Binance and a
  *Custom JSON* ticker URL – is **always verified**: against the root CA you
  upload on `/system` → *Advanced* (`/ca.pem`) when present, else against a
  bundle of four public roots baked into the firmware; a chain that does not
  verify is a failed fetch, never an unverified connection. **The legacy
  Pull URL is different:** HTTPS is verified against your uploaded root CA,
  and **without one the connection is made unverified** – `/api/status`
  reports `tls_insecure_used: true` and *System* shows an INSECURE warning.
  The bundle (`tickr_display/certs/`, compiled by `scripts/build_ca_bundle.py`;
  each PEM carries its provenance and fingerprint):

  | Root | SHA-256 fingerprint | Expires | Used by |
  |---|---|---|---|
  | DigiCert Global Root G2 | `CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F` | 2038-01-15 | Binance |
  | GTS Root R4 | `34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D` | 2036-06-22 | CoinGecko, Kraken (when served as a self-signed root) |
  | GlobalSign Root CA | `EB:D4:10:40:E4:BB:3E:C7:42:C9:E3:81:D3:1E:F2:A4:1A:48:B6:68:5C:96:E7:CE:F3:C1:DF:6C:D4:33:1C:99` | 2028-01-28 | CoinGecko, Kraken as served today (cross-signed GTS R4 on top – the verifier matches the issuer of the top certificate) |
  | ISRG Root X1 | `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6` | 2035-06-04 | Let's Encrypt – self-hosted proxies behind a custom ticker URL |

  Hostname verification is mbedTLS's usual one. There is **no certificate
  expiry check on the device** (mbedTLS is built without
  `MBEDTLS_HAVE_TIME_DATE`; the device has no clock), so an
  expired-but-otherwise-valid chain is accepted. A root leaving the bundle
  (GlobalSign in 2028) is a firmware change, not a config change.
* **`POST /api/source/test`** (token) makes the device fetch an arbitrary
  `http(s)://` URL and returns the extracted numbers, never the body – a
  bounded GET (≤ 4 KB, 10 s timeouts, one at a time). A token holder could
  use it to probe LAN hosts, as with a Pull URL.
* Wi-Fi credentials are stored in the ESP32 NVS in plain text (no flash
  encryption); anyone with physical access and a UART adapter can read them.
  This is the same as the stock firmware.
* Payloads larger than 4 KB are rejected (`413`); the JSON is validated
  completely before anything is drawn or played (`400` otherwise).
* **Discovery beacons** (UDP broadcast, port 47000) tell everyone on the
  subnet a device's MAC, name, IP, firmware version and power mode. Fake
  beacons only ever populate a presence list (`/api/peers`); a device never
  acts on one.
* **Groups and pairing** ([`docs/MULTI_DEVICE.md`](docs/MULTI_DEVICE.md)).
  Members share a 32-byte **group secret** in `config.json` (plain text, like
  the API token and MQTT password). Joining requires reading a **6-digit
  code off the device's screen**; the code is bound to an X25519 key
  exchange, so a passive sniffer cannot brute-force it from captured
  traffic, and the check word shown on both sides exposes a
  man-in-the-middle **if the user compares it**. Three wrong codes trigger a
  growing cooldown. The API token lets its holder make a device *leave* a
  group without a code (they could reflash it anyway) and fetch the group
  secret – wrapped in an ephemeral key exchange – to add further devices.
  While a code is on the screen the screen read-back answers `503` in
  release builds; **`tickr_dev` deliberately keeps it readable**
  (`TICKR_PAIR_DEBUG_SCREEN`) – never install a `tickr_dev` image on a
  device you want pairing to protect.
* **Device-to-device requests** (the sleeper's `GET /api/relay/<id>` and
  `PUT /api/peers/<id>/screen`) are signed with the group secret over the
  method, the path, a **single-use nonce** (issued by the relay, 30 s, burnt
  before the signature is checked) and the SHA-256 of the body
  (`X-Tickr-Group`); an outsider gets `401`, a replay fails on the nonce.
  What a relay parks for a sleeper is written only with the API token. A
  parked firmware URL is fetched over plain `http://`: whoever can write it
  or spoof the LAN host it points to can feed a sleeper an image – the same
  trust as `POST /update` with the token; the bootloader rollback applies.
* **Recovery mode** ([README](README.md#lost-your-token-or-wi-fi-recovery-mode),
  [`docs/WEB_UI.md`](docs/WEB_UI.md#recovery-mode)). **Whoever can operate
  the power switch is trusted as the person in charge of the device.** Three
  power cycles within 20 s of each start bring up the open access point
  `TickrDisplay` (`192.168.244.1`) next to the normal Wi-Fi link and – **only
  for clients of that access point** – unlock: change/forget Wi-Fi, replace
  or remove the API token (shown once), leave the group, factory reset, boot
  the previous firmware image. The check is the interface the request
  arrived on, not the client's address or any header, so nothing on the LAN
  can reach these routes, token or not. Entering the mode changes nothing by
  itself; it ends after 10 min without AP clients, after 30 min at most, or
  with any reboot. Software restarts, deep-sleep wake-ups and crash resets
  never count towards the three. Anyone with a minute of physical access can
  take over a device – as with a UART cable or the stock firmware's portal;
  physical-access resistance is outside this firmware's model.

Do **not** expose the device to the internet or to untrusted networks. Put it
on an IoT VLAN if you can.

## Reporting a vulnerability

If you find a way to escalate beyond the model above (remote code execution
from a crafted payload, a crash from network input, a bypass of the API token
where present, the Wi-Fi password leaking over the network, joining a group
or recovering the group secret without the code on the screen or the API
token, a recovery action reachable from the LAN side or recovery mode entered
without the power switch), please report it **privately**:

* GitHub's *"Report a vulnerability"* (Security tab → Advisories) on
  [`vskiwi/tickrdisplay`](https://github.com/vskiwi/tickrdisplay/security/advisories),
  **or**
* the e-mail address or contact links shown on the maintainer's GitHub
  profile, [github.com/vskiwi](https://github.com/vskiwi).

Do **not** open a public issue for a security bug.

Include the firmware version (`/api/system/info` → `firmware.version`), the
board revision, steps to reproduce and a proof-of-concept if you have one.
You will get an acknowledgement within 7 days; fixes are best effort in a
volunteer project. Public disclosure after a fix is released, or after
90 days, whichever is first, is fine. Issues *inside* the threat model ("the
API has no TLS") are not vulnerabilities – open a normal issue or PR.

## Hardening tips for users

* Set an API token (the shelf offers one on first open) and MQTT credentials;
  upload the root CA of an HTTPS Pull URL server. For a private proxy behind
  a Let's Encrypt certificate used as a *Custom JSON* ticker URL the built-in
  ISRG Root X1 suffices.
* Give the device a static DHCP lease and firewall it from anything but your
  automation host and broker.
* Keep a backup of the stock firmware ([`docs/FLASHING.md`](docs/FLASHING.md)).
* Keep the device where only you can reach its power switch – that switch
  is the recovery key.
