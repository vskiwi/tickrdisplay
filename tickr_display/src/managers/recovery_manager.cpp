#include "recovery_manager.h"
#include "web_auth.h"
#include "wifi_portal.h"
#include "pairing_manager.h"
#include "peer_manager.h"
#include "ota_manager.h"
#include "../hal/hal_display.h"

#include <WiFi.h>
#include <nvs.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// Constants / state
// ---------------------------------------------------------------------------
#ifdef TICKR_RECOVERY_TEST
static const uint32_t IDLE_MS = 2UL * 60UL * 1000UL;    // shortened for the remote test run
static const uint32_t MAX_MS  = 4UL * 60UL * 1000UL;
#else
static const uint32_t IDLE_MS = 10UL * 60UL * 1000UL;   // no AP client -> close
static const uint32_t MAX_MS  = 30UL * 60UL * 1000UL;   // absolute, clients or not
#endif
static const uint32_t ACTION_DELAY_MS = 1200;           // let the JSON answer reach the phone
static const char CT_JSON[] = "application/json";

enum Action : uint8_t { ACT_NONE, ACT_RESTART, ACT_FACTORY, ACT_FACTORY_WIFI };

static RecoveryCounter s_counter;
static AppConfig*      s_cfg = nullptr;
static bool            s_pending = false;
static bool            s_active = false;
static uint32_t        s_started_ms = 0;
static volatile Action s_action = ACT_NONE;
static uint32_t        s_action_at_ms = 0;
#ifdef TICKR_RECOVERY_TEST
static volatile bool   s_enter_req = false;
#endif

// ---------------------------------------------------------------------------
// NVS store for the counter (namespace "tickr", key "rcnt"; the same NVS
// partition the Wi-Fi driver keeps the credentials in)
// ---------------------------------------------------------------------------
static uint8_t nvs_load(void*) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("tickr", NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, "rcnt", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}

static void nvs_save(void*, uint8_t n) {
    nvs_handle_t h;
    if (nvs_open("tickr", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "rcnt", n);
    nvs_commit(h);
    nvs_close(h);
}

bool recovery_boot(RecoveryResetKind kind) {
    static const RecoveryStore store = { nullptr, nvs_load, nvs_save };
    bool enter = recovery_counter_boot(&s_counter, &store, recovery_reset_is_cold(kind));
    Serial.printf("[Recovery] series position %u%s\n", (unsigned)s_counter.position, enter ? " -> recovery mode" : "");
    return enter;
}

uint8_t recovery_position() { return s_counter.position; }
void recovery_set_pending(bool enter) { s_pending = enter; }
bool recovery_pending() { return s_pending; }
bool recovery_active() { return s_active; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void send_json(AsyncWebServerRequest* r, int code, const char* body) {
    AsyncWebServerResponse* resp = r->beginResponse(code, CT_JSON, body);
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
}

static void send_error(AsyncWebServerRequest* r, int code, const char* msg) {
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    send_json(r, code, body);
}

// The mode is "on" for a client when the AP is up because of recovery, or
// when recovery was due at boot and the setup portal holds the AP instead
// (no network to join: the portal page then carries the recovery section).
static bool mode_on() {
    return s_active || (s_pending && wifi_portal_active());
}

static bool allowed(AsyncWebServerRequest* r) {
    return mode_on() && wifi_portal_request_via_ap(r);
}

static void schedule(Action a) {
    s_action_at_ms = millis() + ACTION_DELAY_MS;
    s_action = a;
}

static void draw_frame(const char* note) {
    display_show_recovery(WIFI_PORTAL_AP_SSID, WIFI_PORTAL_AP_IP, note);
    display_temp_hold(MAX_MS);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static void h_status(AsyncWebServerRequest* r) {
    bool via_ap = wifi_portal_request_via_ap(r);
    if (!via_ap) REQUIRE_AUTH(r);
    uint32_t elapsed = s_active ? millis() - s_started_ms : 0;
    unsigned clients = WiFi.softAPgetStationNum();
    uint32_t budget = clients ? MAX_MS : IDLE_MS;
    uint32_t remaining = s_active && elapsed < budget ? (budget - elapsed) / 1000 : 0;
    char group[128], other[24] = "null";
    const char* err;
    pairing_group_json(group, sizeof(group));
    if (ota_other_slot(other + 1, sizeof(other) - 2, false, &err)) {   // -> "\"app1\""
        other[0] = '"';
        strcat(other, "\"");
    }
    char body[320];
    snprintf(body, sizeof(body),
             "{\"active\":%d,\"via_ap\":%d,\"ap_ip\":\"%s\",\"clients\":%u,\"remaining_s\":%lu,\"token_set\":%d,\"group\":%s,\"other_slot\":%s}",
             mode_on(), via_ap, wifi_portal_ap_up() ? WiFi.softAPIP().toString().c_str() : WIFI_PORTAL_AP_IP,
             clients, (unsigned long)remaining, web_auth_enabled(), group, other);
    send_json(r, 200, body);
}

// mode=new: fresh 32-hex token, returned exactly once. mode=clear: API open.
static void h_token(AsyncWebServerRequest* r) {
    const char* mode = r->hasParam("mode", true) ? r->getParam("mode", true)->value().c_str() : "";
    char tok[33] = "";
    if (strcmp(mode, "new") == 0) {
        uint8_t raw[16];
        esp_fill_random(raw, sizeof(raw));
        for (int i = 0; i < 16; i++) snprintf(tok + 2 * i, 3, "%02x", raw[i]);
    } else if (strcmp(mode, "clear") != 0) {
        send_error(r, 400, "mode=new|clear");
        return;
    }
    char old[sizeof(s_cfg->api_token)];
    memcpy(old, s_cfg->api_token, sizeof(old));
    strlcpy(s_cfg->api_token, tok, sizeof(s_cfg->api_token));
    if (!config_save(*s_cfg)) {
        memcpy(s_cfg->api_token, old, sizeof(old));
        send_error(r, 500, "failed to write configuration");
        return;
    }
    web_auth_set_token(s_cfg->api_token);
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"token\":\"%s\"}", tok);
    send_json(r, 200, body);
}

static void h_leave(AsyncWebServerRequest* r) {
    if (!pairing_has_group()) { send_error(r, 404, "no group"); return; }
    if (!pairing_leave_group()) { send_error(r, 500, "failed to write configuration"); return; }
    send_json(r, 200, "{\"ok\":true}");
}

static void h_factory(AsyncWebServerRequest* r) {
    schedule(r->hasParam("wifi", true) ? ACT_FACTORY_WIFI : ACT_FACTORY);
    send_json(r, 200, "{\"ok\":true,\"message\":\"resetting and restarting\"}");
}

static void h_boot_previous(AsyncWebServerRequest* r) {
    char label[17];
    const char* err;
    if (!ota_other_slot(label, sizeof(label), true, &err)) { send_error(r, 409, err); return; }
    schedule(ACT_RESTART);
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"boot\":\"%s\"}", label);
    send_json(r, 200, body);
}

class RecoveryHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* r) const override {
        return r->url().startsWith("/api/recovery/");
    }
    void handleRequest(AsyncWebServerRequest* r) override {
        const char* path = r->url().c_str() + strlen("/api/recovery/");
        bool post = r->method() == HTTP_POST;
        if (!post && strcmp(path, "status") == 0) { h_status(r); return; }
#ifdef TICKR_RECOVERY_TEST
        if (post && strcmp(path, "enter") == 0) {
            REQUIRE_AUTH(r);
            s_enter_req = true;
            send_json(r, 200, "{\"ok\":true}");
            return;
        }
#endif
        int k = strcmp(path, "token") == 0 ? 1 : strcmp(path, "group/leave") == 0 ? 2
              : strcmp(path, "factory") == 0 ? 3 : strcmp(path, "boot/previous") == 0 ? 4 : 0;
        if (!post || !k) { send_error(r, 404, "not found"); return; }
        if (!allowed(r)) { send_error(r, 403, "recovery mode: only through the TickrDisplay access point"); return; }
        if (k == 1) h_token(r);
        else if (k == 2) h_leave(r);
        else if (k == 3) h_factory(r);
        else h_boot_previous(r);
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void recovery_register_routes(AsyncWebServer& server, AppConfig* cfg) {
    s_cfg = cfg;
    server.addHandler(new RecoveryHandler());
}

static void start() {
    if (s_active) return;
    s_pending = false;
    s_active = true;
    s_started_ms = millis();
    wifi_portal_start_ap(WIFI_PORTAL_AP_SSID);
    char note[48];
    snprintf(note, sizeof(note), UI_RECOVERY_NOTE_EXIT_FMT, (unsigned long)(IDLE_MS / 60000UL));
    draw_frame(note);
    Serial.println("[Recovery] mode active");
}

void recovery_start_if_pending() {
    if (s_pending) start();
}

static void stop() {
    s_active = false;
    wifi_portal_stop();
    display_temp_end();                  // back to the content (no-op if it was replaced)
    Serial.println("[Recovery] mode ended");
}

static void factory_reset(bool wifi) {
    static const char* const kFiles[] = { "/config.json", "/config.json.tmp", "/layout.json", "/layout.json.tmp", "/peers.bin", "/ca.pem" };
    for (const char* f : kFiles) {
        if (LittleFS.exists(f)) LittleFS.remove(f);
    }
    if (wifi) WiFi.disconnect(true, true);   // erase the NVS credentials, radio off (we restart anyway)
}

void recovery_loop() {
    recovery_counter_tick(&s_counter, millis());
#ifdef TICKR_RECOVERY_TEST
    if (s_enter_req) { s_enter_req = false; s_pending = true; start(); }
#endif
    Action a = s_action;
    if (a != ACT_NONE && (int32_t)(millis() - s_action_at_ms) >= 0) {
        s_action = ACT_NONE;
        if (a == ACT_FACTORY || a == ACT_FACTORY_WIFI) factory_reset(a == ACT_FACTORY_WIFI);
        else peers_flush();                  // planned restart: keep the peer table
        Serial.println("[Recovery] restart");
        ESP.restart();
    }
    if (!s_active) return;
    wifi_portal_tick();                      // captive DNS, connect / forget requests from the AP
    uint32_t elapsed = millis() - s_started_ms;
    if (elapsed >= MAX_MS || (elapsed >= IDLE_MS && WiFi.softAPgetStationNum() == 0)) stop();
}
