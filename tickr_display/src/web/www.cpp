#include "www.h"
#include <ESPAsyncWebServer.h>
#include "generated/www_assets.h"

const WwwAsset* www_find(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < WWW_ASSETS_COUNT; i++) {
        if (strcmp(WWW_ASSETS[i].name, name) == 0) return &WWW_ASSETS[i];
    }
    return nullptr;
}

void www_send(AsyncWebServerRequest* request, const char* name) {
    const WwwAsset* a = www_find(name);
    if (!a) {
        request->send(404, "text/plain", "asset not found");
        return;
    }
    if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == a->etag) {
        AsyncWebServerResponse* resp = request->beginResponse(304);
        resp->addHeader("ETag", a->etag);
        resp->addHeader("Cache-Control", "no-cache");
        request->send(resp);
        return;
    }
    AsyncWebServerResponse* resp = request->beginResponse(200, a->mime, a->data, a->len);
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("ETag", a->etag);
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
}

size_t www_total_gz_bytes() {
    return WWW_ASSETS_GZ_TOTAL;
}
