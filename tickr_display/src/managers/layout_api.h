#pragma once
//
// Fleet layout store (docs/MULTI_DEVICE.md "The shelf layout"): /layout.json in
// LittleFS, written by the panel to every USB member and read back from all
// of them (newest `updated_at` wins in the browser). Validated by
// src/logic/layout.cpp, written via temp file + rename like config.json.
//
//   GET    /api/layout   the stored document or 404 (public)
//   PUT    /api/layout   body = JSON <= LAYOUT_MAX_LEN (group rule: 403 without a token)
//   DELETE /api/layout   forget it (group rule)
//
#include <ESPAsyncWebServer.h>

void layout_register_routes(AsyncWebServer& server);
