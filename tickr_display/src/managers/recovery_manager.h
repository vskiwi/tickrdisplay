#pragma once
//
// Power-cycle recovery mode (docs/WEB_UI.md "Recovery mode",
// SECURITY.md). The device has no button: switching it off and on three
// times within 20 s of each start (src/logic/recovery_counter) brings up the
// open access point "TickrDisplay" (192.168.244.1) NEXT TO the normal STA
// link - MQTT, pull, panel and API keep working - and unlocks a few
// non-destructive-by-default actions for clients of that AP only:
//
//   GET  /api/recovery/status         {active,via_ap,ap_ip,clients,remaining_s,token_set,
//                                      group,other_slot} - open through the AP; token from the LAN
//   POST /api/recovery/token           mode=new -> {ok,token} (shown once) | mode=clear
//   POST /api/recovery/group/leave     wipe the group credentials (pairing_leave_group)
//   POST /api/recovery/factory [wifi=1] config.json, layout.json, peers.bin, ca.pem
//                                      deleted (+ Wi-Fi credentials with wifi=1), restart
//   POST /api/recovery/boot/previous   other app slot -> boot partition, restart
//   POST /api/wifi/connect | forget    (wifi_portal.cpp) change / forget the network
//
// Authorisation: the request must have arrived through the AP interface
// (wifi_portal_request_via_ap(): the socket's local address is the AP
// address - not a header, not the client subnet) while recovery is active;
// everything else is 403, token or not. Being at the switch = being the
// owner; the LAN never gets these without the token. The mode ends after
// 10 min without AP clients (30 min at most), or with any reboot.
//
// tickr_dev (-DTICKR_RECOVERY_TEST): a software restart counts as a power
// cycle, POST /api/recovery/enter (token) enters the mode directly, the
// timeouts shrink to 2 / 4 min. None of it is in the release image.
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config_manager.h"
#include "../logic/recovery_counter.h"

// Boot-time decision: feeds the counter (NVS) with this boot's reset cause.
// Returns true when recovery mode is due. Call before anything blocks.
bool recovery_boot(RecoveryResetKind kind);
// 1-based position of this boot in a power-cycle series, 0 = not a counted boot.
uint8_t recovery_position();

// Arms the mode for recovery_start_if_pending(); main.cpp calls it with the
// boot decision (USB power only - on battery the device would sleep).
void recovery_set_pending(bool enter);
bool recovery_pending();

// /api/recovery/* on the shared server; `cfg` = the live configuration.
void recovery_register_routes(AsyncWebServer& server, AppConfig* cfg);

// After the STA link is up: AP + captive DNS, recovery frame, timers.
void recovery_start_if_pending();

// Main-task housekeeping: the counter's 20 s window, deferred actions
// (restart, factory reset), AP timeout. Also called from the blocking
// portal loop so a pending action is executed there as well.
void recovery_loop();

bool recovery_active();
