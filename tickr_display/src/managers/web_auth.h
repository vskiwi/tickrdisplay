#pragma once
//
// Shared web authentication helper (implemented in connectivity_manager.cpp).
//
// Returns true if the request may proceed: either no api_token is configured
// (API open, default) or the request carries valid credentials:
//   * HTTP Basic auth (any username, password = api_token), or
//   * header "X-Api-Token: <api_token>".
// Token comparison is constant-time. Every module that registers a route that
// mutates state, uploads, or exposes secrets (config, raw partitions / flash,
// the wrapped group secret) must call this at the top of its handlers.
// Read-only routes without secrets (/api/status, /api/screen/*, /api/peers,
// GET /api/layout, /api/system/info, /api/power/raw, /api/wifi/status, the
// pages) are public: without a token the pages are read-only and show an
// inline token field (docs/WEB_UI.md "API token in the browser").
//
#include <ESPAsyncWebServer.h>

bool web_auth_ok(AsyncWebServerRequest* request);

// Sends 401 {"error":"unauthorized"}. "WWW-Authenticate: Basic" is added only
// for navigations (Accept: text/html - address bar, link, plain form), never
// for fetch/XHR, so the pages never trigger the browser's Basic prompt
// (src/logic/auth_policy.h). Use after web_auth_ok() returned false.
void web_auth_reject(AsyncWebServerRequest* request);

// Updates the token used by web_auth_ok() (called by ConnectivityManager).
void web_auth_set_token(const char* token);

// True when an api_token is configured at all.
bool web_auth_enabled();

// Sends 403 {"error":"set an API token on /system first"} - group functions
// (docs/MULTI_DEVICE.md "Roles: API token versus group secret") refuse to work on a device without a token.
void web_auth_reject_no_token(AsyncWebServerRequest* request);

// Early-return helper for request handlers.
#define REQUIRE_AUTH(req) do { if (!web_auth_ok(req)) { web_auth_reject(req); return; } } while (0)

// Group-function rule: 403 without a configured token, 401 without a valid one.
#define REQUIRE_GROUP_AUTH(req) do { \
        if (!web_auth_enabled()) { web_auth_reject_no_token(req); return; } \
        if (!web_auth_ok(req)) { web_auth_reject(req); return; } \
    } while (0)
