#include "payload.h"
#include "rtttl.h"
#include "fmt_float.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>

static void set_err(char* err, size_t err_len, const char* msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg);
}

static void set_err2(char* err, size_t err_len, const char* field, const char* msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s: %s", field, msg);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Copies a string (or number rendered as text) into dst. Returns false on type/length error.
static bool copy_text_field(JsonVariantConst v, const char* name, char* dst, size_t dst_len,
                            char* err, size_t err_len) {
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s) s = "";
        size_t n = strlen(s);
        if (n >= dst_len) {
            set_err2(err, err_len, name, "too long");
            return false;
        }
        memcpy(dst, s, n + 1);
        return true;
    }
    // Build uses ARDUINOJSON_USE_LONG_LONG=0 / ARDUINOJSON_USE_DOUBLE=0
    // (see platformio.ini), so the widest supported types are long / float.
    if (v.is<long>()) {
        snprintf(dst, dst_len, "%ld", v.as<long>());
        return true;
    }
    if (v.is<float>()) {
        // No "%g": the image links the ROM nano printf (fmt_float.h).
        if (!fmt_float_g(v.as<float>(), dst, dst_len)) {
            set_err2(err, err_len, name, "too long");
            return false;
        }
        return true;
    }
    set_err2(err, err_len, name, "must be a string");
    return false;
}

static bool parse_led(JsonVariantConst v, ScreenPayload* out, char* err, size_t err_len) {
    if (!v.is<const char*>()) {
        set_err(err, err_len, "alert.led: must be a string RRGGBB");
        return false;
    }
    const char* s = v.as<const char*>();
    if (!s) s = "";
    if (*s == '#') s++;
    if (strlen(s) != 6) {
        set_err(err, err_len, "alert.led: expected 6 hex digits RRGGBB");
        return false;
    }
    uint8_t rgb[3];
    for (int i = 0; i < 3; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            set_err(err, err_len, "alert.led: invalid hex digit");
            return false;
        }
        rgb[i] = (uint8_t)((hi << 4) | lo);
    }
    out->has_led = true;
    out->r = rgb[0];
    out->g = rgb[1];
    out->b = rgb[2];
    return true;
}

static bool parse_volume(JsonVariantConst v, ScreenPayload* out, char* err, size_t err_len) {
    if (!v.is<long>()) {
        set_err(err, err_len, "alert.volume: must be an integer 0..255");
        return false;
    }
    long vol = v.as<long>();
    if (vol < 0 || vol > 255) {
        set_err(err, err_len, "alert.volume: out of range 0..255");
        return false;
    }
    out->has_volume = true;
    out->volume = (uint8_t)vol;
    return true;
}

static bool parse_sound(JsonVariantConst v, ScreenPayload* out, char* err, size_t err_len) {
    if (!v.is<const char*>()) {
        set_err(err, err_len, "alert.sound: must be a string");
        return false;
    }
    const char* s = v.as<const char*>();
    if (!s) s = "";

    if (strcmp(s, "none") == 0 || *s == '\0') { out->sound = SOUND_NONE;        return true; }
    if (strcmp(s, "beep") == 0)                { out->sound = SOUND_BEEP;        return true; }
    if (strcmp(s, "double_beep") == 0)         { out->sound = SOUND_DOUBLE_BEEP; return true; }
    if (strcmp(s, "long_beep") == 0)           { out->sound = SOUND_LONG_BEEP;   return true; }

    RtttlSong song;
    if (rtttl_looks_like(s) && strlen(s) < sizeof(out->rtttl) && rtttl_parse(s, &song)) {
        out->sound = SOUND_RTTTL;
        strncpy(out->rtttl, s, sizeof(out->rtttl) - 1);
        out->rtttl[sizeof(out->rtttl) - 1] = '\0';
        return true;
    }

    // Not an error for the whole payload: the caller ignores it and logs.
    out->sound = SOUND_UNKNOWN;
    set_err(err, err_len, "alert.sound ignored: unknown preset or invalid RTTTL");
    return true;
}

PayloadResult payload_parse(const char* json, size_t len, ScreenPayload* out,
                            char* err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!out) return PAYLOAD_ERR_FIELD;
    // cppcheck-suppress memsetClassFloat ; all-zero bytes are 0.0f for spark[]
    memset(out, 0, sizeof(*out));
    out->sound = SOUND_NONE;

    if (!json || len == 0) {
        set_err(err, err_len, "empty body");
        return PAYLOAD_ERR_EMPTY;
    }
    if (len > PAYLOAD_MAX_LEN) {
        set_err(err, err_len, "payload too large");
        return PAYLOAD_ERR_TOO_LARGE;
    }

    DynamicJsonDocument doc(PAYLOAD_JSON_DOC);
    if (doc.capacity() == 0) {
        set_err(err, err_len, "out of memory");
        return PAYLOAD_ERR_JSON;
    }
    DeserializationError e = deserializeJson(doc, json, len);
    if (e) {
        set_err2(err, err_len, "invalid JSON", e.c_str());
        return PAYLOAD_ERR_JSON;
    }
    if (!doc.is<JsonObjectConst>()) {
        set_err(err, err_len, "root must be a JSON object");
        return PAYLOAD_ERR_NOT_OBJECT;
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();

    int recognised = 0;

    JsonVariantConst title = root["title"];
    JsonVariantConst value = root["value"];
    if (!title.isNull()) {
        if (!copy_text_field(title, "title", out->title, sizeof(out->title), err, err_len))
            return PAYLOAD_ERR_FIELD;
        out->has_text = true;
        recognised++;
    }
    if (!value.isNull()) {
        if (!copy_text_field(value, "value", out->value, sizeof(out->value), err, err_len))
            return PAYLOAD_ERR_FIELD;
        out->has_text = true;
        recognised++;
    }

    JsonVariantConst alert = root["alert"];
    if (!alert.isNull()) {
        if (!alert.is<JsonObjectConst>()) {
            set_err(err, err_len, "alert: must be an object");
            return PAYLOAD_ERR_FIELD;
        }
        JsonObjectConst a = alert.as<JsonObjectConst>();
        JsonVariantConst led = a["led"];
        JsonVariantConst vol = a["volume"];
        JsonVariantConst snd = a["sound"];
        if (!led.isNull()) {
            if (!parse_led(led, out, err, err_len)) return PAYLOAD_ERR_FIELD;
            recognised++;
        }
        if (!vol.isNull()) {
            if (!parse_volume(vol, out, err, err_len)) return PAYLOAD_ERR_FIELD;
            recognised++;
        }
        if (!snd.isNull()) {
            if (!parse_sound(snd, out, err, err_len)) return PAYLOAD_ERR_FIELD;
            recognised++;
        }
    }

    // --- ticker fields (docs/API.md "Payload format") --------------------------
    JsonVariantConst change = root["change"];
    if (!change.isNull()) {
        if (!copy_text_field(change, "change", out->change, sizeof(out->change), err, err_len))
            return PAYLOAD_ERR_FIELD;
        out->has_change = out->change[0] != '\0';   // "" = no change line, no direction
        recognised++;
    }
    JsonVariantConst dir = root["dir"];
    if (!dir.isNull()) {
        if (dir.is<long>()) {
            long d = dir.as<long>();
            out->dir = d < 0 ? -1 : (d > 0 ? 1 : 0);
        } else if (dir.is<const char*>()) {
            const char* s = dir.as<const char*>();
            if (!s) s = "";
            if (strcmp(s, "up") == 0) out->dir = 1;
            else if (strcmp(s, "down") == 0) out->dir = -1;
            else if (strcmp(s, "flat") == 0) out->dir = 0;
            else { set_err(err, err_len, "dir: expected -1|0|1 or up|down|flat"); return PAYLOAD_ERR_FIELD; }
        } else {
            set_err(err, err_len, "dir: expected -1|0|1 or up|down|flat");
            return PAYLOAD_ERR_FIELD;
        }
        out->has_dir = true;
        recognised++;
    } else if (out->has_change) {
        out->dir = ticker_dir_from_change(out->change);
        out->has_dir = true;
    }
    JsonVariantConst age = root["age_s"];
    if (!age.isNull()) {
        float a = age.is<long>() ? (float)age.as<long>() : age.is<float>() ? age.as<float>() : -1.0f;
        if (a < 0) {
            set_err(err, err_len, "age_s: must be a non-negative number");
            return PAYLOAD_ERR_FIELD;
        }
        out->has_age = true;
        out->age_s = a >= 4294967040.0f ? 0xFFFFFFFEu : (uint32_t)a;
        recognised++;
    }
    JsonVariantConst time = root["time"];
    if (!time.isNull()) {
        if (!copy_text_field(time, "time", out->time, sizeof(out->time), err, err_len))
            return PAYLOAD_ERR_FIELD;
        recognised++;
    }
    JsonVariantConst spark = root["spark"];
    if (!spark.isNull()) {
        if (!spark.is<JsonArrayConst>()) {
            set_err(err, err_len, "spark: must be an array of numbers");
            return PAYLOAD_ERR_FIELD;
        }
        // Bounded copy: at most TICKER_SPARK_MAX numeric points, the rest ignored.
        for (JsonVariantConst v : spark.as<JsonArrayConst>()) {
            if (out->spark_n >= TICKER_SPARK_MAX) break;
            if (v.is<float>() || v.is<long>()) out->spark[out->spark_n++] = v.as<float>();
        }
        recognised++;
    }

    if (recognised == 0) {
        set_err(err, err_len, "no recognised fields (title, value, alert, change, dir, age_s, time, spark)");
        return PAYLOAD_ERR_NOTHING_TO_DO;
    }
    return PAYLOAD_OK;
}

const char* payload_result_str(PayloadResult r) {
    switch (r) {
        case PAYLOAD_OK:                 return "ok";
        case PAYLOAD_ERR_EMPTY:          return "empty";
        case PAYLOAD_ERR_TOO_LARGE:      return "too_large";
        case PAYLOAD_ERR_JSON:           return "invalid_json";
        case PAYLOAD_ERR_NOT_OBJECT:     return "not_object";
        case PAYLOAD_ERR_FIELD:          return "invalid_field";
        case PAYLOAD_ERR_NOTHING_TO_DO:  return "nothing_to_do";
    }
    return "unknown";
}
