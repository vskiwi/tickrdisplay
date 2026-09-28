#include "layout.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>

static bool bad(char* err, size_t err_len, const char* msg) {
    if (err && err_len) snprintf(err, err_len, "%s", msg);
    return false;
}

static bool slot_ok(JsonVariantConst v) {
    if (!v.is<int>()) return false;
    int n = v.as<int>();
    return n >= 0 && n <= LAYOUT_SLOT_MAX;
}

bool layout_validate(const char* json, size_t len, size_t* devices, char* err, size_t err_len) {
    if (devices) *devices = 0;
    if (!json || len == 0) return bad(err, err_len, "empty body");
    if (len > LAYOUT_MAX_LEN) return bad(err, err_len, "layout too large");
    // ArduinoJson's const char* reader copies strings, and a dense document of
    // tiny entries ("tickr-XXXXXX":{"x":0,"y":0} = 29 B) needs ~2x its size in
    // slots on the 32-bit target (~3.3x on a 64-bit host); 4x + slack covers
    // both (<= 17 KB transient for the 4 KB maximum).
    DynamicJsonDocument doc(4 * len + 1024);
    DeserializationError e = deserializeJson(doc, json, len);
    if (e == DeserializationError::NoMemory) return bad(err, err_len, "layout too complex");
    if (e != DeserializationError::Ok || !doc.is<JsonObjectConst>()) return bad(err, err_len, "invalid JSON object");
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (!root["v"].is<int>() || root["v"].as<int>() != 1) return bad(err, err_len, "v must be 1");
    JsonVariantConst ts = root["updated_at"];
    if (!(ts.is<long>() || ts.is<float>()) || ts.as<float>() < 0) return bad(err, err_len, "updated_at must be a number");
    JsonVariantConst by = root["by"];
    if (!by.isNull() && !by.is<const char*>()) return bad(err, err_len, "by must be a string");
    JsonVariantConst devs = root["devices"];
    if (!devs.is<JsonObjectConst>()) return bad(err, err_len, "devices must be an object");
    size_t n = 0;
    for (JsonPairConst kv : devs.as<JsonObjectConst>()) {
        if (++n > LAYOUT_MAX_DEVICES) return bad(err, err_len, "too many devices");
        const char* id = kv.key().c_str();
        if (strncmp(id, "tickr-", 6) != 0 || strlen(id) != 12) return bad(err, err_len, "device id must be tickr-XXXXXX");
        JsonVariantConst d = kv.value();
        if (!d.is<JsonObjectConst>()) return bad(err, err_len, "device entry must be an object");
        if (!slot_ok(d["x"]) || !slot_ok(d["y"])) return bad(err, err_len, "x/y must be 0..15");
        JsonVariantConst name = d["name"];
        if (!name.isNull()) {
            if (!name.is<const char*>()) return bad(err, err_len, "name must be a string");
            const char* s = name.as<const char*>();
            if (strlen(s) > LAYOUT_NAME_MAX) return bad(err, err_len, "name too long");
            for (; *s; s++) {
                unsigned char c = (unsigned char)*s;
                if (c < 0x20 || c > 0x7E) return bad(err, err_len, "name must be printable ASCII");
            }
        }
    }
    // Optional content groups {"<name>":["tickr-XXXXXX",...]} - the
    // panel's "for all / for a group" targets. Names 1..31 printable ASCII.
    JsonVariantConst groups = root["groups"];
    if (!groups.isNull()) {
        if (!groups.is<JsonObjectConst>()) return bad(err, err_len, "groups must be an object");
        size_t g = 0;
        for (JsonPairConst kv : groups.as<JsonObjectConst>()) {
            if (++g > LAYOUT_MAX_GROUPS) return bad(err, err_len, "too many groups");
            const char* name = kv.key().c_str();
            size_t nl = strlen(name);
            if (nl == 0 || nl > LAYOUT_NAME_MAX) return bad(err, err_len, "group name must be 1..31 chars");
            for (const char* s = name; *s; s++) {
                unsigned char c = (unsigned char)*s;
                if (c < 0x20 || c > 0x7E) return bad(err, err_len, "group name must be printable ASCII");
            }
            if (!kv.value().is<JsonArrayConst>()) return bad(err, err_len, "group must be an array of ids");
            size_t m = 0;
            for (JsonVariantConst v : kv.value().as<JsonArrayConst>()) {
                if (++m > LAYOUT_MAX_DEVICES) return bad(err, err_len, "group too large");
                const char* id = v.as<const char*>();
                if (!v.is<const char*>() || strncmp(id, "tickr-", 6) != 0 || strlen(id) != 12)
                    return bad(err, err_len, "group member must be tickr-XXXXXX");
            }
        }
    }
    if (devices) *devices = n;
    return true;
}
