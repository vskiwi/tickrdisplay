# Manual test scenarios

Scenarios that need a person at the device – a phone, the power switch or a
Wi-Fi router. They complement the host tests (`pio test -e native`) and the
per-release smoke test in [`RELEASING.md`](RELEASING.md). Run them on a
release build unless stated otherwise; expected texts are the device's
(see [`DEVICE_UI.md`](DEVICE_UI.md)). If something deviates, open an issue
with the OS / browser used, the step and what the screen showed.

## Recovery mode

USB power, a phone. Behaviour described in [`WEB_UI.md`](WEB_UI.md) →
*Recovery mode*.

1. Switch off and on – the e-ink shows the splash with *Recovery: switch
   off/on now*.
2. Off and on again within 20 s – *Restart 2 of 3*. Wait longer than 20 s
   instead and the next cycle shows the plain splash again.
3. Third cycle within 20 s – the *Recovery mode* screen; the LAN address
   still answers; the open access point `TickrDisplay` appears.
4. Join it with the phone – `http://192.168.244.1/` shows the Wi-Fi page
   with the *Recovery* section; `/api/recovery/status` reports `via_ap: 1`.
5. *New API token* – the old token answers `401` from the LAN, the new one
   works.
6. *Leave group* – `/api/group` answers `404`; re-pair from the shelf.
7. *Boot previous firmware* – the other slot boots (`/api/system/info` →
   `running`) without the splash (software reset).
8. Optional, destructive: *Factory reset* with and without the Wi-Fi
   checkbox.
9. Timeout: the access point disappears about 10 min after the last client
   left and the content returns; any reboot ends the mode at once.
10. On battery: no plain splash; *Restart 2 of 3* on the second cycle,
    *Recovery: USB only* on the third.

## Wi-Fi set-up portal

USB power.

1. Make the saved network unreachable and power-cycle – after the connect
   timeout the e-ink shows the *Wi-Fi setup* screen and the open access
   point `TickrDisplay` appears.
2. Join from iOS, Android and a laptop – the captive sheet opens the Wi-Fi
   page (networks strongest first); otherwise `http://192.168.244.1/` shows
   it and any other URL redirects there. The client gets a `192.168.244.x`
   address.
3. `http://192.168.244.1/system` opens with the *Firmware* upload (the
   cable-free recovery path for a bad image).
4. Wrong password – *Connection failed* appears and *Connect* is re-enabled.
5. Right password – *Connected! IP …*, the access point closes shortly
   after, the device is reachable on the LAN; a power cycle rejoins without
   the portal.
6. Portal timeout: join the access point and leave it – the device restarts
   after a few minutes and, with the network still down, comes back into
   set-up mode.

## Shelf on a phone

A real phone joined to the LAN, at least two devices.

1. Open `http://<device-ip>/` – one column of cards in shelf order.
2. Tap a card – the device sheet opens as a bottom sheet; the backdrop, × and
   *Back* close it.
3. *Change…* → *Text* → *Send* – the card's frame updates.
4. *Move* – tap another card; the layout is saved and the other device's
   shelf shows the same order.
5. Drag a card sideways – it swaps with its neighbour; a vertical swipe
   scrolls the page instead.

## Device sheet write actions

Any browser with the API token entered, two USB-powered members.

1. *Rename* – the name changes on the card and in the other device's shelf.
2. *Identify this* – the device draws its number; *Stop identify* clears it.
3. *Test LED & sound* – colour, *Off*, *Beep* act on the device; the next
   payload restores the payload's LED and sound.
4. *Clear pending* on a sleeping member with parked content – the *pending*
   badge disappears and the relay's `/api/relay` no longer lists it.
5. *Forget* on an offline card – it leaves the shelf and returns with its
   next beacon.
