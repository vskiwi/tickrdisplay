#include "wifi_portal.h"
#include "../log.h"
#include "web_auth.h"
#include "../web/www.h"
#include "../hal/hal_display.h"
#include "../hal/hal_indication.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

static const uint32_t PORTAL_CONNECT_TIMEOUT_MS = 20000;
static const char JSON_CT[] = "application/json";

static DNSServer* s_dns = nullptr;
static bool s_active = false;          // setup portal (blocking flow)
static bool s_ap_up = false;           // AP up: setup portal or recovery
static bool s_has_saved = false;
static char s_ap_ssid[33] = "";

// Portal-initiated connection attempt. The HTTP handler (async_tcp task) only
// fills s_req_* and raises s_req_pending; the main task performs the connect.
static volatile bool s_req_pending = false;
static volatile bool s_forget_pending = false;
static char s_req_ssid[33] = "";
static char s_req_pass[65] = "";
static volatile WifiPortalState s_state = WIFI_PORTAL_IDLE;
static char s_target_ssid[33] = "";
static char s_error[40] = "";
static uint32_t s_deadline = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool wifi_portal_request_via_ap(AsyncWebServerRequest* request) {
    if (!s_ap_up || !request || !request->client()) return false;
    return request->client()->localIP() == WiFi.softAPIP();
}

bool wifi_portal_captive(AsyncWebServerRequest* request) {
    return s_active || wifi_portal_request_via_ap(request);
}

// Read endpoints: open for captive clients, token otherwise.
static bool auth_ok_or_portal(AsyncWebServerRequest* request) {
    if (wifi_portal_captive(request)) return true;   // no token yet / user has no LAN access
    if (web_auth_ok(request)) return true;
    web_auth_reject(request);
    return false;
}

static const char* state_str(WifiPortalState s) {
    switch (s) {
        case WIFI_PORTAL_CONNECTING: return "connecting";
        case WIFI_PORTAL_CONNECTED:  return "connected";
        case WIFI_PORTAL_FAILED:     return "failed";
        default:                     return "idle";
    }
}

static const char* wl_error_str(wl_status_t st) {
    switch (st) {
        case WL_NO_SSID_AVAIL:  return "network not found";
        case WL_CONNECT_FAILED: return "authentication failed";
        case WL_CONNECTION_LOST: return "connection lost";
        default:                return "timeout";
    }
}

static void send_json(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* resp = request->beginResponse(code, JSON_CT, body);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

static void send_error(AsyncWebServerRequest* request, int code, const char* msg) {
    StaticJsonDocument<96> doc;
    doc["status"] = "error";
    doc["error"] = msg;
    send_json(request, code, doc);
}

static void redirect_to_portal(AsyncWebServerRequest* request) {
    String url = "http://" + WiFi.softAPIP().toString() + "/";
    AsyncWebServerResponse* resp = request->beginResponse(302, "text/plain", "redirect to portal");
    resp->addHeader("Location", url);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
// GET /api/wifi/status - public: SSID / IP / RSSI are on the air anyway;
// the scan keeps the captive-or-token rule (it drives the radio).
static void handle_status(AsyncWebServerRequest* request) {
    StaticJsonDocument<384> doc;
    doc["portal"] = s_active;
    doc["captive"] = wifi_portal_captive(request);   // connect/forget allowed for this client
    doc["saved"] = s_has_saved;
    bool sta_up = WiFi.status() == WL_CONNECTED;
    if (s_ap_up) {
        // Inside the portal report the portal-initiated attempt; before any
        // attempt a still-connected STA (unlikely) counts as connected.
        WifiPortalState st = s_state;
        if (st == WIFI_PORTAL_IDLE && sta_up) st = WIFI_PORTAL_CONNECTED;
        doc["state"] = state_str(st);
        doc["ssid"] = st == WIFI_PORTAL_IDLE ? "" : (sta_up && st == WIFI_PORTAL_CONNECTED ? WiFi.SSID() : String(s_target_ssid));
        if (st == WIFI_PORTAL_FAILED) doc["error"] = s_error;
        doc["ap_ssid"] = s_ap_ssid;
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["clients"] = WiFi.softAPgetStationNum();
    } else {
        doc["state"] = sta_up ? "connected" : "failed";
        doc["ssid"] = sta_up ? WiFi.SSID() : String();
    }
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = sta_up ? WiFi.RSSI() : 0;
    send_json(request, 200, doc);
}

// Scan management lives entirely on the async_tcp task (start + read), so the
// result buffer of WiFiScanClass is never freed while another task reads it.
static void handle_scan(AsyncWebServerRequest* request) {
    if (!auth_ok_or_portal(request)) return;
    if (s_state == WIFI_PORTAL_CONNECTING) {
        send_error(request, 409, "connect in progress");
        return;
    }
    int16_t n = WiFi.scanComplete();
    bool rescan = request->hasParam("rescan");
    if (n == WIFI_SCAN_FAILED || (rescan && n != WIFI_SCAN_RUNNING)) {
        WiFi.scanNetworks(true /*async*/, false /*hidden*/);
        n = WIFI_SCAN_RUNNING;
    }
    if (n == WIFI_SCAN_RUNNING) {
        StaticJsonDocument<64> doc;
        doc["status"] = "scanning";
        send_json(request, 202, doc);
        return;
    }
    // n >= 0: results available. Stream them, strongest first, one entry per
    // SSID (repeaters/mesh nodes show up several times).
    AsyncResponseStream* out = request->beginResponseStream(JSON_CT);
    out->addHeader("Cache-Control", "no-store");
    out->print("{\"status\":\"ok\",\"networks\":[");
    // Sort indices by RSSI (n is small: the driver caps at ~20 APs).
    int16_t idx[32];
    int16_t cnt = n > 32 ? 32 : n;
    for (int16_t i = 0; i < cnt; i++) idx[i] = i;
    for (int16_t i = 1; i < cnt; i++) {
        int16_t v = idx[i], j = i - 1;
        while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(v)) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    bool first = true;
    for (int16_t i = 0; i < cnt; i++) {
        String ssid = WiFi.SSID(idx[i]);
        if (ssid.length() == 0) continue;
        bool dup = false;
        for (int16_t k = 0; k < i && !dup; k++) dup = WiFi.SSID(idx[k]) == ssid;
        if (dup) continue;
        StaticJsonDocument<128> e;
        e["ssid"] = ssid;
        e["rssi"] = WiFi.RSSI(idx[i]);
        e["secure"] = WiFi.encryptionType(idx[i]) != WIFI_AUTH_OPEN;
        if (!first) out->print(',');
        String js;
        serializeJson(e, js);
        out->print(js);
        first = false;
    }
    out->print("]}");
    request->send(out);
}

// Mutating endpoints: only for captive clients (setup portal, or through the
// recovery AP). 403 for everybody else, token or not - see the header.
static bool captive_or_403(AsyncWebServerRequest* request) {
    if (wifi_portal_captive(request)) return true;
    send_error(request, 403, s_ap_up ? "only through the TickrDisplay access point" : "setup portal not active");
    return false;
}

static void handle_connect(AsyncWebServerRequest* request) {
    if (!captive_or_403(request)) return;
    if (s_state == WIFI_PORTAL_CONNECTING || s_req_pending) {
        send_error(request, 409, "connect in progress");
        return;
    }
    if (!request->hasParam("ssid", true)) {
        send_error(request, 400, "missing ssid");
        return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : String();
    if (ssid.length() == 0 || ssid.length() > 32) {
        send_error(request, 400, "ssid must be 1..32 characters");
        return;
    }
    if (pass.length() > 64) {
        send_error(request, 400, "password too long (max 64)");
        return;
    }
    strlcpy(s_req_ssid, ssid.c_str(), sizeof(s_req_ssid));
    strlcpy(s_req_pass, pass.c_str(), sizeof(s_req_pass));
    strlcpy(s_target_ssid, s_req_ssid, sizeof(s_target_ssid));
    s_error[0] = '\0';
    s_state = WIFI_PORTAL_CONNECTING;   // reported immediately; the main task starts the attempt
    s_req_pending = true;
    StaticJsonDocument<96> doc;
    doc["status"] = "connecting";
    doc["ssid"] = s_req_ssid;
    send_json(request, 202, doc);
}

// POST /api/wifi/forget: erase the saved network (recovery: "the device is
// on the wrong network / I moved"). The main task does it; the next boot
// without a connect through this page opens the setup portal.
static void handle_forget(AsyncWebServerRequest* request) {
    if (!captive_or_403(request)) return;
    s_forget_pending = true;
    request->send(200, JSON_CT, "{\"status\":\"ok\"}");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void wifi_portal_register_routes(AsyncWebServer& server) {
    server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* request) { www_send(request, "wifi.html"); });
    server.on("/api/wifi/status", HTTP_GET, handle_status);
    server.on("/api/wifi/scan", HTTP_GET, handle_scan);
    server.on("/api/wifi/connect", HTTP_POST, handle_connect);
    server.on("/api/wifi/forget", HTTP_POST, handle_forget);

    // Captive-portal behaviour: while the AP is up every unknown URL - which
    // is what the OS connectivity probes are - is redirected to the portal
    // (for the AP clients only in recovery mode; the LAN keeps its 404).
    server.onNotFound([](AsyncWebServerRequest* request) {
        if (request->method() == HTTP_GET && wifi_portal_captive(request)) {
            redirect_to_portal(request);
            return;
        }
        request->send(404, JSON_CT, "{\"status\":\"error\",\"error\":\"not found\"}");
    });
}

void wifi_portal_set_saved_credentials(bool saved) {
    s_has_saved = saved;
}

// SoftAP + captive DNS, shared by the setup portal and the recovery AP.
static void ap_up(const char* ap_ssid) {
    strlcpy(s_ap_ssid, ap_ssid, sizeof(s_ap_ssid));
    s_state = WIFI_PORTAL_IDLE;
    s_req_pending = false;
    s_forget_pending = false;
    s_error[0] = '\0';

    WiFi.mode(WIFI_AP_STA);
    delay(100);
    // Own subnet instead of the ESP default 192.168.4.x, which is also a
    // popular home-router range: a phone joined to the AP must not see the
    // portal address inside its own LAN.
    IPAddress ip;
    ip.fromString(WIFI_PORTAL_AP_IP);
    if (!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0))) {
        Serial.println("[Portal] softAPConfig() failed");
    }
    if (!WiFi.softAP(s_ap_ssid)) {
        Serial.println("[Portal] softAP() failed");
    }
    delay(200);   // let the AP netif come up before reading its IP
    ip = WiFi.softAPIP();

    s_dns = new DNSServer();
    s_dns->setErrorReplyCode(DNSReplyCode::NoError);
    s_dns->start(53, "*", ip);
    s_ap_up = true;
    Serial.printf("[Portal] AP '%s' up, portal at http://%s/\n", s_ap_ssid, ip.toString().c_str());
}

void wifi_portal_start(const char* ap_ssid) {
    if (s_active) return;
    if (!s_ap_up) ap_up(ap_ssid);
    s_active = true;
    display_show_setup_instruction(s_ap_ssid, WiFi.softAPIP().toString().c_str());
    indication_overlay(LED_OVL_SETUP, true);     // white while the portal is open (docs/DEVICE_UI.md "LED and sound")

    // Warm the network list so the page has it on first load.
    WiFi.scanNetworks(true, false);
}

void wifi_portal_start_ap(const char* ap_ssid) {
    if (s_ap_up) return;
    ap_up(ap_ssid);
    WiFi.scanNetworks(true, false);
}

void wifi_portal_tick() {
    if (!s_ap_up) return;
    if (s_dns) s_dns->processNextRequest();

    if (s_forget_pending) {
        s_forget_pending = false;
        Serial.println("[Portal] forgetting the saved network");
        WiFi.disconnect(false /*keep radio*/, true /*erase NVS credentials*/);
        s_has_saved = false;
        s_state = WIFI_PORTAL_IDLE;
    }

    if (s_req_pending) {
        s_req_pending = false;
        LOGV("[Portal] connecting to '%s'\n", s_req_ssid);
        // Default persistent mode: the driver stores ssid/pass in NVS, where
        // WiFi.begin() finds them on the next boot (same record WiFiManager
        // and the stock firmware use).
        WiFi.begin(s_req_ssid, s_req_pass[0] ? s_req_pass : nullptr);
        s_state = WIFI_PORTAL_CONNECTING;
        s_deadline = millis() + PORTAL_CONNECT_TIMEOUT_MS;
        memset(s_req_pass, 0, sizeof(s_req_pass));
    }

    if (s_state == WIFI_PORTAL_CONNECTING) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            s_state = WIFI_PORTAL_CONNECTED;
            s_has_saved = true;
            Serial.printf("[Portal] connected, IP %s\n", WiFi.localIP().toString().c_str());
        } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || (int32_t)(millis() - s_deadline) >= 0) {
            strlcpy(s_error, wl_error_str(st), sizeof(s_error));
            s_state = WIFI_PORTAL_FAILED;
            Serial.printf("[Portal] connect failed: %s\n", s_error);
        }
    }
}

void wifi_portal_stop() {
    if (!s_ap_up) return;
    s_active = false;
    s_ap_up = false;
    indication_overlay(LED_OVL_SETUP, false);
    if (s_dns) {
        s_dns->stop();
        delete s_dns;
        s_dns = nullptr;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println("[Portal] closed");
}

bool wifi_portal_active() { return s_active; }
bool wifi_portal_ap_up() { return s_ap_up; }
WifiPortalState wifi_portal_state() { return s_state; }
bool wifi_portal_connected() { return s_state == WIFI_PORTAL_CONNECTED; }
