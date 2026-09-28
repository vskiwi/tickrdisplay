#include "layout_api.h"
#include "web_auth.h"
#include "../logic/layout.h"
#include <LittleFS.h>
#include <memory>

static const char LAYOUT_FILE[]     = "/layout.json";
static const char LAYOUT_FILE_TMP[] = "/layout.json.tmp";
static const char CT_JSON[]         = "application/json";

static void send_error(AsyncWebServerRequest* r, int code, const char* msg) {
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    r->send(code, CT_JSON, body);
}

// GET: stream the file in chunks; the document is never held in RAM. Public:
// positions and names only.
static void handle_get(AsyncWebServerRequest* request) {
    if (!LittleFS.exists(LAYOUT_FILE)) { send_error(request, 404, "no layout stored"); return; }
    auto f = std::make_shared<File>(LittleFS.open(LAYOUT_FILE, "r"));
    if (!*f) { send_error(request, 500, "cannot open layout"); return; }
    size_t size = f->size();
    AsyncWebServerResponse* r = request->beginResponse(CT_JSON, size,
        [f](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            if (!*f) return 0;
            f->seek(index);
            size_t n = f->read(buf, maxLen);
            if (index + n >= f->size()) f->close();
            return n;
        });
    r->addHeader("Cache-Control", "no-cache");
    request->send(r);
}

static void collect_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!web_auth_enabled() || !web_auth_ok(request)) return;          // final handler answers 403/401
    if (total == 0 || total > LAYOUT_MAX_LEN) return;                   // final handler answers 400/413
    if (index == 0) request->_tempObject = calloc(total + 1, 1);       // freed by ~AsyncWebServerRequest
    if (request->_tempObject && index + len <= total) memcpy((uint8_t*)request->_tempObject + index, data, len);
}

static void handle_put(AsyncWebServerRequest* request) {
    REQUIRE_GROUP_AUTH(request);
    size_t total = request->contentLength();
    if (total > LAYOUT_MAX_LEN) { send_error(request, 413, "layout too large (max 4096 bytes)"); return; }
    const char* body = (const char*)request->_tempObject;
    if (total == 0 || !body) { send_error(request, 400, "JSON body with Content-Length required"); return; }
    char err[48];
    size_t devices = 0;
    if (!layout_validate(body, total, &devices, err, sizeof(err))) { send_error(request, 400, err); return; }
    File f = LittleFS.open(LAYOUT_FILE_TMP, "w");
    bool ok = f && f.write((const uint8_t*)body, total) == total;
    if (f) f.close();
    if (ok && !LittleFS.rename(LAYOUT_FILE_TMP, LAYOUT_FILE)) {
        LittleFS.remove(LAYOUT_FILE);
        ok = LittleFS.rename(LAYOUT_FILE_TMP, LAYOUT_FILE);
    }
    if (!ok) { LittleFS.remove(LAYOUT_FILE_TMP); send_error(request, 500, "failed to write layout"); return; }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"bytes\":%u,\"devices\":%u}", (unsigned)total, (unsigned)devices);
    request->send(200, CT_JSON, out);
}

static void handle_delete(AsyncWebServerRequest* request) {
    REQUIRE_GROUP_AUTH(request);
    if (!LittleFS.exists(LAYOUT_FILE)) { send_error(request, 404, "no layout stored"); return; }
    request->send(LittleFS.remove(LAYOUT_FILE) ? 200 : 500, CT_JSON, "{\"ok\":true}");
}

void layout_register_routes(AsyncWebServer& server) {
    server.on("/api/layout", HTTP_GET, handle_get);
    server.on("/api/layout", HTTP_PUT | HTTP_POST, handle_put, nullptr, collect_body);
    server.on("/api/layout", HTTP_DELETE, handle_delete);
}
