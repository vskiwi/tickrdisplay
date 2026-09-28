#pragma once
//
// Web UI assets: the pages under www/src/ are compiled by scripts/build_www.py
// into gzip'ed PROGMEM arrays (src/web/generated/www_assets.h, a build product)
// and served straight from flash with Content-Encoding: gzip - no RAM copy, no
// template processor. Everything dynamic on a page is fetched by its own
// JavaScript from the JSON API (/api/status, /api/config, /api/system/info).
//
#include <stdint.h>
#include <stddef.h>

class AsyncWebServerRequest;

struct WwwAsset {
    const char*    name;   // file name in www/src/, e.g. "panel.html"
    const char*    mime;
    const uint8_t* data;   // gzip'ed body, PROGMEM
    uint32_t       len;
    const char*    etag;   // quoted CRC32 of the gzip'ed body
};

// Looks an asset up by its file name; nullptr if unknown.
const WwwAsset* www_find(const char* name);

// Sends the asset with Content-Encoding: gzip, ETag and Cache-Control: no-cache
// (browsers revalidate and get a 304 when If-None-Match matches). Answers 404
// when `name` is unknown.
void www_send(AsyncWebServerRequest* request, const char* name);

// Total size of all gzipped assets (for /api/system/info diagnostics).
size_t www_total_gz_bytes();
