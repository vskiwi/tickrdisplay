#pragma once
// Screen payload (JSON) validation, separated from any hardware access so it can
// be unit-tested on the host (ArduinoJson compiles natively).
//
// Accepted document:
// {
//   "title": "Bitcoin",            // string (or number), <= PAYLOAD_TITLE_MAX-1 chars
//   "value": "$95,240",            // string (or number), <= PAYLOAD_VALUE_MAX-1 chars
//   "alert": {
//     "led":    "00FF00",          // RRGGBB hex, optional leading '#'
//     "sound":  "beep",            // preset: beep | double_beep | long_beep | none, or RTTTL
//     "volume": 128                // 0..255
//   },
//   // ticker look (docs/API.md "Payload format") - all optional:
//   "change": "+0.07%",            // string (or number) as formatted by the sender, <= 15 chars
//   "dir": 1,                      // -1 | 0 | 1, or "down" | "flat" | "up"; derived from the sign of `change` when absent
//   "age_s": 120,                  // age of the quote at send time, seconds (the device adds its own elapsed time)
//   "time": "19:00",               // pass-through string shown instead of the relative age, <= 15 chars
//   "spark": [83950, 83990, 84001] // <= 48 numbers (the rest is ignored); non-numbers are skipped
// }
// Every field is optional, but the document must be a JSON object and must
// contain at least one recognised field. `change` or `spark` selects the
// ticker layout; a plain title/value payload keeps the Text look.

#include <stdint.h>
#include <stddef.h>
#include "ticker.h"

#define PAYLOAD_MAX_LEN     4096   // maximum accepted raw JSON size (bytes)
#define PAYLOAD_JSON_DOC    4096   // ArduinoJson document capacity used for parsing
#define PAYLOAD_TITLE_MAX   64
#define PAYLOAD_VALUE_MAX   128
#define PAYLOAD_SOUND_MAX   1024   // >= RTTTL_MAX_LEN

enum PayloadSound : uint8_t {
    SOUND_NONE = 0,        // no sound requested (or "none")
    SOUND_BEEP,
    SOUND_DOUBLE_BEEP,
    SOUND_LONG_BEEP,
    SOUND_RTTTL,           // `rtttl` holds a validated melody
    SOUND_UNKNOWN          // unrecognised string: ignore, log
};

struct ScreenPayload {
    bool has_text;         // title and/or value present -> redraw screen
    char title[PAYLOAD_TITLE_MAX];
    char value[PAYLOAD_VALUE_MAX];

    bool has_led;
    uint8_t r, g, b;

    bool has_volume;
    uint8_t volume;

    PayloadSound sound;
    char rtttl[PAYLOAD_SOUND_MAX];

    // --- ticker fields ----------------------------------------------------
    bool     has_change;                 // non-empty `change`
    char     change[TICKER_CHANGE_MAX];
    bool     has_dir;                    // explicit `dir`, or derived from `change`
    TickerDir dir;
    bool     has_age;
    uint32_t age_s;
    char     time[TICKER_TIME_MAX];      // "" = absent
    uint8_t  spark_n;                    // numeric points kept (<= TICKER_SPARK_MAX)
    float    spark[TICKER_SPARK_MAX];
};
// The ticker layout is selected by `change` or `spark` (docs/API.md "Payload format").
static inline bool payload_is_ticker(const ScreenPayload& p) { return p.has_change || p.spark_n > 0; }

enum PayloadResult : uint8_t {
    PAYLOAD_OK = 0,
    PAYLOAD_ERR_EMPTY,          // zero length
    PAYLOAD_ERR_TOO_LARGE,      // > PAYLOAD_MAX_LEN
    PAYLOAD_ERR_JSON,           // deserialization failed (details in err)
    PAYLOAD_ERR_NOT_OBJECT,     // root is not an object
    PAYLOAD_ERR_FIELD,          // a field has the wrong type / range (details in err)
    PAYLOAD_ERR_NOTHING_TO_DO   // valid JSON but no recognised fields
};

// Parses and validates `json` (length `len`, need not be NUL-terminated).
// On failure `err` (if non-NULL) receives a short human-readable reason.
PayloadResult payload_parse(const char* json, size_t len, ScreenPayload* out,
                            char* err, size_t err_len);

// Static description for a result code (never NULL).
const char* payload_result_str(PayloadResult r);
