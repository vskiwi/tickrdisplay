#include "relay_manager.h"
#include "web_auth.h"
#include "peer_manager.h"
#include "ota_manager.h"
#include "../logic/relay.h"
#include "../logic/pairing.h"
#include "../logic/payload.h"   // PAYLOAD_MAX_LEN
#include "../logic/renderer.h"
#include "../logic/screen_bmp.h"
#include "../logic/device_state.h"
#include "../hal/hal_display.h"

#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const char CT_JSON[]  = "application/json";
static const char CT_OCTET[] = "application/octet-stream";
static const char HDR_GROUP[] = "X-Tickr-Group";
static const char HDR_NONCE[] = "X-Tickr-Nonce";
static const char HDR_OTA[]   = "X-Tickr-OTA-URL";
static const size_t OTA_BODY_MAX = 256;
static const uint32_t PROBE_WAIT_HINT_MS  = 600;    // unicast to the cached relay
static const uint32_t PROBE_WAIT_BCAST_MS = 900;    // broadcast: replies arrive within 0-200 ms + jitter
static const uint32_t CLIENT_TIMEOUT_MS   = 5000;

static RelayNonceStore s_nonces;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;   // nonce store: async_tcp + main task

// Age of the frames uploaded this boot (LittleFS has no clock): id tail -> uptime.
struct FrameAge { uint8_t tail[3]; uint32_t at_s; };
static FrameAge s_ages[TICKR_MAX_PEERS];
static uint8_t  s_ages_n = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t uptime_s() { return millis() / 1000; }

static void send_err(AsyncWebServerRequest* r, int code, const char* msg) {
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    r->send(code, CT_JSON, body);
}

static void sha256_of(const uint8_t* data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_ret(data, len, out, 0);
}

static void issue_nonce(char out[RELAY_NONCE_LEN]) {
    portENTER_CRITICAL(&s_mux);
    relay_nonce_issue(&s_nonces, uptime_s(), esp_random, out);
    portEXIT_CRITICAL(&s_mux);
}

static bool consume_nonce(const char* nonce) {
    portENTER_CRITICAL(&s_mux);
    bool ok = relay_nonce_consume(&s_nonces, nonce, uptime_s());
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

// Verifies X-Tickr-Group for `method`/`path` over the body hash. 0 = ok (and
// `next` holds the nonce for the caller's next request), else the HTTP code.
static int check_signature(AsyncWebServerRequest* r, const char* method, const char* path,
                           const uint8_t body_sha[32], char next[RELAY_NONCE_LEN]) {
    if (!r->hasHeader(HDR_GROUP)) return 401;
    RelayHeader h;
    if (!relay_header_parse(r->header(HDR_GROUP).c_str(), &h)) return 401;
    char gid[BEACON_HEX16_LEN];
    uint16_t epoch;
    uint8_t secret[GROUP_SECRET_LEN];
    if (!peers_group_snapshot(gid, &epoch, secret)) return 403;
    // The nonce is burnt first: a wrong signature never gets a second try with the same one.
    bool ok = consume_nonce(h.nonce) && relay_verify(h, gid, epoch, secret, method, path, body_sha);
    memset(secret, 0, sizeof(secret));
    if (!ok) return 401;
    issue_nonce(next);
    return 0;
}

// Whole small file into a malloc'd, NUL-terminated buffer (caller frees). NULL when absent / too big.
static char* read_small_file(const char* name, size_t cap, size_t* len) {
    *len = 0;
    File f = LittleFS.open(name, "r");
    if (!f) return nullptr;
    size_t n = f.size();
    char* buf = (n > 0 && n <= cap) ? (char*)malloc(n + 1) : nullptr;
    if (buf) {
        *len = f.read((uint8_t*)buf, n);
        buf[*len] = '\0';
    }
    f.close();
    return buf;
}

static bool write_file(const char* name, const uint8_t* data, size_t len) {
    File f = LittleFS.open(name, "w");
    bool ok = f && f.write(data, len) == len;
    if (f) f.close();
    if (!ok) LittleFS.remove(name);
    return ok;
}

// Age slot of `id` (create = allocate one). NULL when unknown / not creatable.
static FrameAge* frame_age_slot(const char* id, bool create) {
    uint8_t tail[6];
    char hex[16];
    snprintf(hex, sizeof(hex), "000000%s", id + 6);
    if (!peer_mac_from_str(hex, tail)) return nullptr;
    for (uint8_t i = 0; i < s_ages_n; i++) {
        if (memcmp(s_ages[i].tail, tail + 3, 3) == 0) return &s_ages[i];
    }
    if (!create) return nullptr;
    uint8_t i = s_ages_n < TICKR_MAX_PEERS ? s_ages_n++ : (uint8_t)(esp_random() % TICKR_MAX_PEERS);
    memcpy(s_ages[i].tail, tail + 3, 3);
    return &s_ages[i];
}

// ---------------------------------------------------------------------------
// HTTP handler (one class for every relay route: cheaper than six server.on())
// ---------------------------------------------------------------------------
class RelayHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* r) const override {
        WebRequestMethod m = r->method();
        if (m != HTTP_GET && m != HTTP_PUT && m != HTTP_DELETE) return false;
        char id[RELAY_ID_LEN];
        return relay_route(r->url().c_str(), id) != RELAY_ROUTE_NONE;
    }
    bool isRequestHandlerTrivial() const override { return false; }

    void handleBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) override {
        if (r->method() != HTTP_PUT) return;
        char id[RELAY_ID_LEN];
        RelayRoute route = relay_route(r->url().c_str(), id);
        size_t cap;
        if (route == RELAY_ROUTE_SCREEN) {
            cap = SCREEN_FRAME_LEN;                                    // the signature covers the body: collect, verify later
            if (total != cap) return;
        } else {
            if (!web_auth_enabled() || !web_auth_ok(r)) return;        // handleRequest answers 403/401
            cap = route == RELAY_ROUTE_OTA ? OTA_BODY_MAX : PAYLOAD_MAX_LEN;
        }
        if (total == 0 || total > cap) return;
        if (index == 0) r->_tempObject = calloc(total + 1, 1);       // freed by ~AsyncWebServerRequest
        if (r->_tempObject && index + len <= total) memcpy((uint8_t*)r->_tempObject + index, data, len);
    }

    void handleRequest(AsyncWebServerRequest* r) override {
        char id[RELAY_ID_LEN], file[40];
        RelayRoute route = relay_route(r->url().c_str(), id);
        relay_file_name(route, id, file, sizeof(file));
        const char* body = (const char*)r->_tempObject;
        size_t total = r->contentLength();

        if (route == RELAY_ROUTE_SCREEN) {
            if (r->method() == HTTP_GET) { send_frame(r, id, file); return; }
            if (r->method() != HTTP_PUT) { send_err(r, 405, "method"); return; }
            if (total != SCREEN_FRAME_LEN || !body) { send_err(r, 400, "4736-byte frame required"); return; }
            uint8_t sha[32];
            char next[RELAY_NONCE_LEN];
            sha256_of((const uint8_t*)body, total, sha);
            int code = check_signature(r, "PUT", r->url().c_str(), sha, next);
            if (code) { send_err(r, code, code == 403 ? "no group" : "bad signature"); return; }
            if (!write_file(file, (const uint8_t*)body, total)) { send_err(r, 500, "write failed"); return; }
            FrameAge* age = frame_age_slot(id, true);
            if (age) age->at_s = uptime_s();
            AsyncWebServerResponse* resp = r->beginResponse(200, CT_JSON, "{\"ok\":true}");
            resp->addHeader(HDR_NONCE, next);
            r->send(resp);
            return;
        }
        if (route == RELAY_ROUTE_PAYLOAD && r->method() == HTTP_GET) { serve_pending(r, id, file); return; }
        if (route == RELAY_ROUTE_LIST) {
            if (r->method() != HTTP_GET) { send_err(r, 405, "method"); return; }
            REQUIRE_GROUP_AUTH(r);
            list_pending(r);
            return;
        }
        // PUT / DELETE /api/relay/<id>[/ota]: the panel, with the token, on a member of our group.
        REQUIRE_GROUP_AUTH(r);
        if (!relay_is_relay()) { send_err(r, 403, "not a relay"); return; }
        if (r->method() == HTTP_DELETE) {
            if (!LittleFS.exists(file)) { send_err(r, 404, "nothing pending"); return; }
            r->send(LittleFS.remove(file) ? 200 : 500, CT_JSON, "{\"ok\":true}");
            return;
        }
        if (r->method() != HTTP_PUT) { send_err(r, 405, "method"); return; }
        if (!peers_is_member_id(id)) { send_err(r, 404, "not a group member"); return; }
        if (total == 0 || !body) { send_err(r, total > (route == RELAY_ROUTE_OTA ? OTA_BODY_MAX : PAYLOAD_MAX_LEN) ? 413 : 400, "JSON body required"); return; }
        if (route == RELAY_ROUTE_OTA) {
            char url[RELAY_URL_MAX];
            if (!relay_url_from_json(body, url, sizeof(url))) { send_err(r, 400, "{\\\"url\\\":\\\"http://...\\\"} required"); return; }
        } else if (body[0] != '{') {
            send_err(r, 400, "JSON object required");   // the sleeper's renderer validates the rest
            return;
        }
        if (!write_file(file, (const uint8_t*)body, total)) { send_err(r, 500, "write failed"); return; }
        char out[48];
        snprintf(out, sizeof(out), "{\"ok\":true,\"bytes\":%u}", (unsigned)total);
        r->send(200, CT_JSON, out);
    }

private:
    // GET /api/relay/<id>[?consume=1] - the sleeper's signed fetch.
    static void serve_pending(AsyncWebServerRequest* r, const char* id, const char* file) {
        uint8_t sha[32];
        char next[RELAY_NONCE_LEN];
        sha256_of((const uint8_t*)"", 0, sha);
        int code = check_signature(r, "GET", r->url().c_str(), sha, next);
        if (code) { send_err(r, code, code == 403 ? "no group" : "bad signature"); return; }
        bool consume = r->hasParam("consume") && r->getParam("consume")->value() == "1";
        char ota_file[40], url[RELAY_URL_MAX] = "";
        relay_file_name(RELAY_ROUTE_OTA, id, ota_file, sizeof(ota_file));
        size_t n;
        char* job = read_small_file(ota_file, OTA_BODY_MAX, &n);
        if (job && !relay_url_from_json(job, url, sizeof(url))) url[0] = '\0';
        free(job);
        char* payload = read_small_file(file, PAYLOAD_MAX_LEN, &n);
        AsyncWebServerResponse* resp = payload ? r->beginResponse(200, CT_JSON, payload) : r->beginResponse(204);
        resp->addHeader(HDR_NONCE, next);
        if (url[0]) resp->addHeader(HDR_OTA, url);
        resp->addHeader("Cache-Control", "no-store");
        free(payload);
        if (consume) { LittleFS.remove(file); LittleFS.remove(ota_file); }
        r->send(resp);
    }

    // GET /api/relay - {"payload":[ids],"ota":[ids]} from the /relay directory.
    static void list_pending(AsyncWebServerRequest* r) {
        char lists[2][TICKR_MAX_PEERS * 15 + 4] = { "", "" };   // "tickr-XXXXXX", per id
        File dir = LittleFS.open("/relay");
        File f = dir ? dir.openNextFile() : File();
        while (f) {
            char id[RELAY_ID_LEN];
            RelayRoute rt = relay_file_route(f.name(), id);
            if (rt == RELAY_ROUTE_PAYLOAD || rt == RELAY_ROUTE_OTA) {
                char* dst = lists[rt == RELAY_ROUTE_OTA];
                size_t n = strlen(dst);
                if (n + 16 < sizeof(lists[0])) snprintf(dst + n, sizeof(lists[0]) - n, "%s\"%s\"", n ? "," : "", id);
            }
            f = dir.openNextFile();
        }
        char body[2 * sizeof(lists[0]) + 32];
        snprintf(body, sizeof(body), "{\"payload\":[%s],\"ota\":[%s]}", lists[0], lists[1]);
        AsyncWebServerResponse* resp = r->beginResponse(200, CT_JSON, body);
        resp->addHeader("Cache-Control", "no-store");
        r->send(resp);
    }

    // GET /api/peers/<id>/screen - public like /api/screen/raw. The 4.7 KB
    // frame goes out from a String copy (no streaming filler: ~0.4 KB less code).
    static void send_frame(AsyncWebServerRequest* r, const char* id, const char* file) {
        size_t n;
        char* frame = read_small_file(file, SCREEN_FRAME_LEN, &n);
        if (!frame || n != SCREEN_FRAME_LEN) { free(frame); send_err(r, 404, "no frame stored"); return; }
        String body;
        body.concat(frame, (unsigned)n);
        free(frame);
        AsyncWebServerResponse* resp = r->beginResponse(200, CT_OCTET, body);
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("X-Screen-Width", String(SCREEN_W));
        resp->addHeader("X-Screen-Height", String(SCREEN_H));
        resp->addHeader("X-Screen-Format", "1bpp-msb");
        FrameAge* age = frame_age_slot(id, false);
        if (age) resp->addHeader("X-Screen-Age-S", String(uptime_s() - age->at_s));
        r->send(resp);
    }
};

// ---------------------------------------------------------------------------
// Public API - relay side
// ---------------------------------------------------------------------------
void relay_begin() {
    relay_nonce_init(&s_nonces);
    LittleFS.mkdir("/relay");
    LittleFS.mkdir("/peers");
}

void relay_register_routes(AsyncWebServer& server) {
    server.addHandler(new RelayHandler());
}

bool relay_is_relay() {
    char gid[BEACON_HEX16_LEN];
    uint16_t epoch;
    uint8_t secret[GROUP_SECRET_LEN];
    bool have = peers_group_snapshot(gid, &epoch, secret);
    memset(secret, 0, sizeof(secret));
    return have && peers_listening();   // USB mode = the discovery listener runs (no ADC read from the HTTP task)
}

bool relay_issue_nonce(char out[RELAY_NONCE_LEN]) {
    if (!relay_is_relay()) { out[0] = '\0'; return false; }
    issue_nonce(out);
    return true;
}

// ---------------------------------------------------------------------------
// Battery side: the sleeper's wake-up flow (docs/MULTI_DEVICE.md "Relay for sleeping members")
// ---------------------------------------------------------------------------
static bool signed_request(HTTPClient& http, const char* method, uint32_t relay_ip, const char* path, const char* nonce,
                           const uint8_t* body, size_t body_len, int* code) {
    char gid[BEACON_HEX16_LEN], ip[16], url[80], hdr[RELAY_HDR_MAX];
    uint16_t epoch;
    uint8_t secret[GROUP_SECRET_LEN], sha[32];
    if (!peers_group_snapshot(gid, &epoch, secret)) return false;
    sha256_of(body, body_len, sha);
    size_t n = relay_header_make(gid, epoch, secret, nonce, method, path, sha, hdr, sizeof(hdr));
    memset(secret, 0, sizeof(secret));
    if (!n) return false;
    beacon_ip_format(relay_ip, ip, sizeof(ip));
    snprintf(url, sizeof(url), "http://%s%s%s", ip, path, method[0] == 'G' ? "?consume=1" : "");
    http.setConnectTimeout(CLIENT_TIMEOUT_MS);
    http.setTimeout(CLIENT_TIMEOUT_MS);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(url)) return false;
    http.addHeader(HDR_GROUP, hdr);
    const char* keys[] = { HDR_NONCE, HDR_OTA };
    http.collectHeaders(keys, 2);
    *code = http.sendRequest(method, (uint8_t*)body, body_len);
    return *code > 0;
}

// Reads exactly `size` body bytes (Content-Length is always sent by a relay) into `out`.
static bool read_body(HTTPClient& http, String& out, size_t size) {
    char* buf = (char*)malloc(size + 1);
    if (!buf) return false;
    WiFiClient* st = http.getStreamPtr();
    size_t got = 0;
    uint32_t deadline = millis() + CLIENT_TIMEOUT_MS;
    while (st && got < size && (int32_t)(deadline - millis()) > 0) {
        size_t avail = st->available();
        if (avail) {
            int n = st->read((uint8_t*)buf + got, avail < size - got ? avail : size - got);
            if (n > 0) { got += (size_t)n; deadline = millis() + CLIENT_TIMEOUT_MS; }
        } else if (!st->connected()) {
            break;
        } else {
            delay(1);
        }
    }
    buf[got] = '\0';
    if (got == size) out = buf;
    free(buf);
    return got == size;
}

RelayClientResult relay_client_run(uint32_t* relay_ip_cache, uint32_t interval_min) {
    RelayFlow f;
    relay_flow_init(&f, *relay_ip_cache != 0, relay_report_frame(interval_min));
    uint32_t relay_ip = 0;
    char nonce[RELAY_NONCE_LEN] = "", id[PEER_ID_LEN], path[RELAY_PATH_MAX], ota_url[RELAY_URL_MAX] = "";
    String body;
    peers_self_id(id, sizeof(id));
    while (true) {
        esp_task_wdt_reset();
        bool ok = false, has_payload = false, has_ota = false;
        switch (f.step) {
            case RELAY_STEP_PROBE_HINT:
            case RELAY_STEP_PROBE_BCAST: {
                bool hint = f.step == RELAY_STEP_PROBE_HINT;
                ok = peers_find_relay(hint ? *relay_ip_cache : 0, &relay_ip, nonce, hint ? PROBE_WAIT_HINT_MS : PROBE_WAIT_BCAST_MS);
                *relay_ip_cache = ok ? relay_ip : 0;              // the cache follows what answered
                break;
            }
            case RELAY_STEP_FETCH: {
                HTTPClient http;
                int code = 0;
                relay_route_path(RELAY_ROUTE_PAYLOAD, id, path, sizeof(path));
                if (signed_request(http, "GET", relay_ip, path, nonce, (const uint8_t*)"", 0, &code)) {
                    int size = http.getSize();
                    if (code == 200 && size > 0 && size <= (int)PAYLOAD_MAX_LEN) {
                        has_payload = ok = read_body(http, body, (size_t)size);
                    } else if (code == 204) {
                        ok = true;
                    }
                    if (ok) {
                        strlcpy(nonce, http.header(HDR_NONCE).c_str(), sizeof(nonce));
                        strlcpy(ota_url, http.header(HDR_OTA).c_str(), sizeof(ota_url));
                        has_ota = relay_url_ok(ota_url);
                    }
                }
                http.end();
                break;
            }
            case RELAY_STEP_APPLY:
                ok = renderer_process_payload(body);
                body = String();
                break;
            case RELAY_STEP_OTA:
                display_set_card(CARD_OTA, DS_AGE_UNKNOWN);        // "Updating firmware / keep the power connected"
                display_show_base();
                ok = ota_update_from_url_now(ota_url);
                if (ok) ESP.restart();                              // flashed: the new image boots now
                display_set_card(CARD_NONE, DS_AGE_UNKNOWN);
                break;
            case RELAY_STEP_UPLOAD: {
                HTTPClient http;
                int code = 0;
                relay_route_path(RELAY_ROUTE_SCREEN, id, path, sizeof(path));
                ok = signed_request(http, "PUT", relay_ip, path, nonce, display_frame_buffer(), SCREEN_FRAME_LEN, &code) && code == 200;
                http.end();
                break;
            }
            case RELAY_STEP_DONE:
                return f.got_payload ? RELAY_RC_SHOWN : RELAY_RC_NOTHING;
            case RELAY_STEP_NO_RELAY:
                return RELAY_RC_NO_RELAY;
            default:
                return RELAY_RC_FAILED;
        }
        relay_flow_next(&f, ok, has_payload, has_ota);
    }
}
