#pragma once
//
// OTA / recovery manager.
//
// Provides:
//   GET  /update                        HTML page (upload form, partition table, actions)
//   POST /update  (alias: POST /u)      firmware upload, multipart field "update" (same as WiFiManager)
//   GET  /api/system/info               chip / flash / partition / OTA state JSON (public)
//   POST /api/system/boot_partition     {"label":"app0"} -> esp_ota_set_boot_partition + restart
//   GET  /api/system/partition/<label>  stream raw partition contents (backup)
//   GET  /api/system/flash?offset=&length=  stream raw flash contents
//   POST /api/system/restart
//   POST /api/system/update_from_url    {"url":"http(s)://.../firmware.bin"} -> 202, flashed by ota_loop()
//
// No partition offsets are hard-coded: everything goes through esp_partition_* / esp_ota_* and
// partitions are resolved by label, so the firmware does not depend on the partition table layout.
//
#include <ESPAsyncWebServer.h>

#ifndef TICKR_FW_NAME
#define TICKR_FW_NAME "TickrDisplay"
#endif

// FIRMWARE_VERSION is the git describe string injected by scripts/version.py
// (e.g. "v0.3.1-4-g1a2b3c4"); the fallback is for builds outside PlatformIO.
#ifndef TICKR_FW_VERSION
#ifdef FIRMWARE_VERSION
#define TICKR_FW_VERSION FIRMWARE_VERSION
#else
#define TICKR_FW_VERSION "0.1.0"
#endif
#endif

// Register all OTA / system routes on the given server. Call before server.begin().
void ota_register_routes(AsyncWebServer& server);

// Must be called from the main loop(). Performs deferred restarts requested by web handlers
// (so ESP.restart() never runs inside the async_tcp task), warms the sketch MD5 cache and
// services ArduinoOTA (see arduino_ota.h).
void ota_loop();

// True while a firmware upload is being written to flash.
bool ota_update_in_progress();

// OTA from a URL: fetches http(s)://.../firmware.bin into the next
// slot and makes it the boot partition. Blocking on the calling (main) task,
// resets the task watchdog while streaming; true = flashed, the caller
// restarts. False with the reason in /api/system/info's update state (Serial).
// POST /api/system/update_from_url {"url"} schedules the same for ota_loop().
bool ota_update_from_url_now(const char* url);

// Recovery "boot previous image": the other app slot, when it holds a valid
// image (same checks as POST /api/system/boot_partition). Writes its label
// into `label` (>= 17 bytes); with `select` it is also made the boot
// partition. False (and `err` set) when there is no usable other slot.
bool ota_other_slot(char* label, size_t label_len, bool select, const char** err);
