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

static bool parse_brightness(JsonVariantConst v, ScreenPayload* out, char* err, size_t err_len) {
    if (!v.is<long>()) {
        set_err(err, err_len, "alert.brightness: must be an integer 0..255");
        return false;
    }
    long bri = v.as<long>();
    if (bri < 0 || bri > 255) {
        set_err(err, err_len, "alert.brightness: out of range 0..255");
        return false;
    }
    out->has_brightness = true;
    out->brightness = (uint8_t)bri;
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

// `dir`: -1 | 0 | 1 or "down" | "flat" | "up". `name` prefixes the error.
static bool parse_dir(JsonVariantConst v, const char* name, TickerDir* out, char* err, size_t err_len) {
    if (v.is<long>()) {
        long d = v.as<long>();
        *out = d < 0 ? -1 : (d > 0 ? 1 : 0);
        return true;
    }
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s) s = "";
        if (strcmp(s, "up") == 0)        { *out = 1;  return true; }
        if (strcmp(s, "down") == 0)      { *out = -1; return true; }
        if (strcmp(s, "flat") == 0)      { *out = 0;  return true; }
    }
    set_err2(err, err_len, name, "expected -1|0|1 or up|down|flat");
    return false;
}

// Printable ASCII only (the fonts are Latin-1 and the fields are small).
static bool ascii_only(const char* s) {
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
    }
    return true;
}

// --- tile layouts (docs/API.md "Tile layouts") ---------------------------------

// A `style` object (root defaults or a tile's): look / size / invert, unknown keys ignored.
// `name` is "style" or "tiles[N].style".
static bool parse_style(JsonVariantConst v, const char* name, TileStyleSpec* out, char* err, size_t err_len) {
    char field[40];
    if (!v.is<JsonObjectConst>()) {
        set_err2(err, err_len, name, "must be an object");
        return false;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    JsonVariantConst look = o["look"], size = o["size"], inv = o["invert"];
    if (!look.isNull()) {
        if (!look.is<const char*>() || !tile_look_parse(look.as<const char*>(), &out->st.look)) {
            snprintf(field, sizeof(field), "%s.look", name);
            set_err2(err, err_len, field, "expected stack|row|ticker");
            return false;
        }
        out->has_look = true;
    }
    if (!size.isNull()) {
        if (!size.is<const char*>() || !tile_size_parse(size.as<const char*>(), &out->st.size)) {
            snprintf(field, sizeof(field), "%s.size", name);
            set_err2(err, err_len, field, "expected auto|l|m|s");
            return false;
        }
        out->has_size = true;
    }
    if (!inv.isNull()) {
        if (!inv.is<bool>()) {
            snprintf(field, sizeof(field), "%s.invert", name);
            set_err2(err, err_len, field, "must be true or false");
            return false;
        }
        out->st.invert = inv.as<bool>();
        out->has_invert = true;
    }
    return true;
}

// One element of `tiles`: an object (title / value / change / dir / style, other keys
// ignored), or null / {} for an empty cell. `idx` names the element in errors.
static bool parse_tile(JsonVariantConst v, uint8_t idx, const TileStyleSpec* root, Tile* t,
                       char* err, size_t err_len) {
    char name[32];
    TileStyleSpec own = {};
    memset(t, 0, sizeof(*t));
    if (v.isNull()) { tiles_style_resolve(root, nullptr, &t->style); return true; }
    if (!v.is<JsonObjectConst>()) {
        snprintf(name, sizeof(name), "tiles[%u]", (unsigned)idx);
        set_err2(err, err_len, name, "must be an object or null");
        return false;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    JsonVariantConst title = o["title"], value = o["value"], change = o["change"], dir = o["dir"], style = o["style"];
    if (!title.isNull()) {
        snprintf(name, sizeof(name), "tiles[%u].title", (unsigned)idx);
        if (!copy_text_field(title, name, t->title, sizeof(t->title), err, err_len)) return false;
        if (!ascii_only(t->title)) { set_err2(err, err_len, name, "printable ASCII only"); return false; }
        t->has = true;
    }
    if (!value.isNull()) {
        snprintf(name, sizeof(name), "tiles[%u].value", (unsigned)idx);
        if (!copy_text_field(value, name, t->value, sizeof(t->value), err, err_len)) return false;
        t->has = true;
    }
    if (!change.isNull()) {
        snprintf(name, sizeof(name), "tiles[%u].change", (unsigned)idx);
        if (!copy_text_field(change, name, t->change, sizeof(t->change), err, err_len)) return false;
        if (t->change[0]) t->has = true;
    }
    if (!dir.isNull()) {
        snprintf(name, sizeof(name), "tiles[%u].dir", (unsigned)idx);
        if (!parse_dir(dir, name, &t->dir, err, err_len)) return false;
        t->has_dir = true;
    } else if (t->change[0]) {
        t->dir = ticker_dir_from_change(t->change);
        t->has_dir = true;
    }
    if (!style.isNull()) {
        snprintf(name, sizeof(name), "tiles[%u].style", (unsigned)idx);
        if (!parse_style(style, name, &own, err, err_len)) return false;
    }
    tiles_style_resolve(root, &own, &t->style);
    return true;
}

// `layout`, `tiles`, `style`, `status`. Sets has_tiles / layout / tiles_n and
// counts the recognised keys; the layout defaults by the tile count.
static bool parse_tiles(JsonObjectConst root, ScreenPayload* out, int* recognised, char* err, size_t err_len) {
    JsonVariantConst layout = root["layout"], tiles = root["tiles"], style = root["style"], status = root["status"];
    bool has_layout = false;
    if (!layout.isNull()) {
        if (!layout.is<const char*>() || !tile_layout_parse(layout.as<const char*>(), &out->layout)) {
            set_err(err, err_len, "layout: expected single|1x1|1x2|2x1|2x2");
            return false;
        }
        has_layout = true;
        (*recognised)++;
    }
    if (!style.isNull()) {
        if (!parse_style(style, "style", &out->style, err, err_len)) return false;
        (*recognised)++;
    }
    if (!status.isNull()) {
        if (!status.is<bool>()) {
            set_err(err, err_len, "status: must be true or false");
            return false;
        }
        out->status = status.as<bool>();
        (*recognised)++;
    }
    if (tiles.isNull()) {
        if (has_layout && out->layout != TILE_LAYOUT_SINGLE) {
            char msg[48];
            snprintf(msg, sizeof(msg), "layout: %s needs tiles", tile_layout_str(out->layout));
            set_err(err, err_len, msg);
            return false;
        }
        return true;
    }
    if (!tiles.is<JsonArrayConst>()) {
        set_err(err, err_len, "tiles: must be an array of up to 4 tiles");
        return false;
    }
    if (has_layout && out->layout == TILE_LAYOUT_SINGLE) {
        set_err(err, err_len, "layout: single cannot carry tiles");
        return false;
    }
    JsonArrayConst arr = tiles.as<JsonArrayConst>();
    size_t n = arr.size();
    if (n > TILES_MAX) {
        set_err(err, err_len, "tiles: more than 4");
        return false;
    }
    if (!has_layout) out->layout = tiles_layout_default((uint8_t)n);
    if (n > tiles_layout_cells(out->layout)) {
        char msg[48];
        snprintf(msg, sizeof(msg), "tiles: more than %u for layout %s",
                 (unsigned)tiles_layout_cells(out->layout), tile_layout_str(out->layout));
        set_err(err, err_len, msg);
        return false;
    }
    uint8_t i = 0;
    for (JsonVariantConst v : arr) {
        if (!parse_tile(v, i, &out->style, &out->tiles[i], err, err_len)) return false;
        i++;
    }
    // the cells the array leaves out are empty, in the root style
    for (uint8_t k = i; k < TILES_MAX; k++) tiles_style_resolve(&out->style, nullptr, &out->tiles[k].style);
    out->tiles_n = (uint8_t)n;
    out->has_tiles = true;
    (*recognised)++;
    return true;
}

void payload_tiles_frame(const ScreenPayload& p, TilesFrame* out) {
    memset(out, 0, sizeof(*out));
    out->layout = p.layout;
    out->n = tiles_layout_cells(p.layout);
    out->status = p.status;
    memcpy(out->t, p.tiles, sizeof(out->t));
    out->age_s = p.has_age ? p.age_s : 0;
}

PayloadResult payload_parse(const char* json, size_t len, ScreenPayload* out,
                            char* err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!out) return PAYLOAD_ERR_FIELD;
    // cppcheck-suppress memsetClassFloat ; all-zero bytes are 0.0f for spark[]
    memset(out, 0, sizeof(*out));
    out->sound = SOUND_NONE;
    out->status = true;

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

    // Tile layouts first: with `tiles` the root value / ticker fields are
    // ignored (a warning), the root title is the Showing line only.
    if (!parse_tiles(root, out, &recognised, err, err_len)) return PAYLOAD_ERR_FIELD;
    const bool tiles = out->has_tiles;
    const char* ignored = nullptr;

    JsonVariantConst title = root["title"];
    JsonVariantConst value = root["value"];
    if (!title.isNull()) {
        if (!copy_text_field(title, "title", out->title, sizeof(out->title), err, err_len))
            return PAYLOAD_ERR_FIELD;
        out->has_text = true;
        recognised++;
    }
    if (tiles) {
        static const char* const kIgnored[] = { "value", "change", "dir", "time", "spark", "short" };   // age_s feeds the age line
        for (const char* k : kIgnored) {
            if (!root[k].isNull()) { ignored = k; break; }
        }
    } else if (!value.isNull()) {
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
        JsonVariantConst bri = a["brightness"];
        JsonVariantConst vol = a["volume"];
        JsonVariantConst snd = a["sound"];
        if (!led.isNull()) {
            if (!parse_led(led, out, err, err_len)) return PAYLOAD_ERR_FIELD;
            recognised++;
        }
        if (!bri.isNull()) {
            if (!parse_brightness(bri, out, err, err_len)) return PAYLOAD_ERR_FIELD;
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
    // (skipped with `tiles`, except age_s - the tiles carry their own)
    JsonVariantConst change = tiles ? JsonVariantConst() : root["change"];
    if (!change.isNull()) {
        if (!copy_text_field(change, "change", out->change, sizeof(out->change), err, err_len))
            return PAYLOAD_ERR_FIELD;
        out->has_change = out->change[0] != '\0';   // "" = no change line, no direction
        recognised++;
    }
    JsonVariantConst dir = tiles ? JsonVariantConst() : root["dir"];
    if (!dir.isNull()) {
        if (!parse_dir(dir, "dir", &out->dir, err, err_len)) return PAYLOAD_ERR_FIELD;
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
    JsonVariantConst time = tiles ? JsonVariantConst() : root["time"];
    if (!time.isNull()) {
        if (!copy_text_field(time, "time", out->time, sizeof(out->time), err, err_len))
            return PAYLOAD_ERR_FIELD;
        recognised++;
    }
    JsonVariantConst spark = tiles ? JsonVariantConst() : root["spark"];
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
    JsonVariantConst shrt = tiles ? JsonVariantConst() : root["short"];
    if (!shrt.isNull()) {
        if (!copy_text_field(shrt, "short", out->short_label, sizeof(out->short_label), err, err_len))
            return PAYLOAD_ERR_FIELD;
        // the badge font is Latin-1 and the field is tiny: printable ASCII only
        if (!ascii_only(out->short_label)) {
            set_err(err, err_len, "short: printable ASCII, up to 7 chars");
            return PAYLOAD_ERR_FIELD;
        }
        recognised++;
    }

    if (recognised == 0) {
        set_err(err, err_len, "no recognised fields (title, value, alert, change, dir, age_s, time, spark, short, layout, tiles, style, status)");
        return PAYLOAD_ERR_NOTHING_TO_DO;
    }
    // Not an error (like an unknown alert.sound): the root field is ignored, the tiles are drawn.
    if (ignored && err && err_len && !err[0]) snprintf(err, err_len, "%s ignored: tiles present", ignored);
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
