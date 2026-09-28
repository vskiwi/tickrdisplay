#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "../logic/source.h"   // SourceSpec (the ticker source, schema 8)

#define CONFIG_API_KEY_LEN 48

// 1: original, 2: mqtt_user/mqtt_pass/api_token, 3: ota_password,
// 4: device_name + layout slot + group credentials (multi-device, docs/MULTI_DEVICE.md "Groups and pairing"),
// 5: power_source override (docs/HARDWARE.md "Power sensing"),
// 6: adc_cell_num/den - divider of the cell channel (docs/HARDWARE.md "Power sensing"),
// 7: led_rule - LED by the sign of the change (docs/TICKERS.md "What the screen shows"),
// 8: source_kind + the ticker source (preset / symbol / market / custom URL and
//    paths / decimals / separator / label, api_key reserved) - docs/TICKERS.md "Presets" / "Custom JSON".
//    Migration: a file without source_kind gets `url` when pull_url is set, else `none`.
// Missing fields always fall back to their defaults, so older files load fine.
#define CONFIG_SCHEMA_VERSION      8
#define CONFIG_REFRESH_MIN_MINUTES 1
#define CONFIG_REFRESH_MAX_MINUTES 1440   // 24 h
#define CONFIG_NAME_LEN            32
#define CONFIG_LAYOUT_MAX          15     // x/y slots 0..15 (packed into Peer::pos nibbles)

struct AppConfig {
    char mqtt_server[64] = "";
    int  mqtt_port = 1883;
    char mqtt_topic[64] = "tickr/display";
    char mqtt_user[32] = "";
    char mqtt_pass[64] = "";
    char pull_url[128] = "";
    int  refresh_interval_min = 60;
    char api_token[64] = "";          // empty = API open (no authentication)
    // ArduinoOTA (LAN push-OTA) password or its MD5 hex. Empty = push-OTA disabled.
    char ota_password[64] = "";       // schema_version >= 3

    // --- schema_version >= 4: multi-device -------------------------------
    // Human name shown in beacons/panel. Empty = "Tickr-XXXX" (last two MAC bytes; fits the 15-char beacon field).
    char    device_name[CONFIG_NAME_LEN] = "";
    // This device's own slot on the virtual shelf (docs/MULTI_DEVICE.md "The shelf layout"); source of truth for the panel.
    uint8_t layout_x = 0;
    uint8_t layout_y = 0;
    char    layout_group[CONFIG_NAME_LEN] = "";
    // Group credentials (docs/MULTI_DEVICE.md "Groups and pairing") - empty until the device is paired.
    // Never returned by any API; group_secret is the beacon-tag / request-signature key.
    char     group_id[17] = "";       // 8 random bytes as 16 hex chars
    char     group_secret[65] = "";   // 32 random bytes as 64 hex chars
    char     group_name[CONFIG_NAME_LEN] = "";
    uint16_t group_epoch = 0;

    // --- schema_version >= 5: power ---------------------------------------
    // Power-source override for boards / adapters the detector gets wrong
    // (a weak adapter behind the stacking pads, an unrecognised board):
    // 0 = auto (detector, invalid -> USB + "unknown"), 1 = always USB,
    // 2 = always battery (deep-sleep cycle). Stored as "auto" | "usb" |
    // "battery". Set with POST /api/power/source, read with
    // GET /api/power/raw and /api/status.
    uint8_t power_source = 0;

    // --- schema_version >= 6: cell divider --------------------------------
    // GPIO 32 (cell) divider as num/den, calibrated pin mV -> cell mV
    // (logic/battery.h CELL_DIVIDER_*_DEFAULT = 189/100, the stock-equivalent
    // ratio; TODO(hw-verify) against a multimeter). Out-of-range values are
    // replaced by the default on load (cell_divider_valid()). Reported as
    // `cell_divider` in GET /api/power/raw, set through POST /config.
    uint16_t adc_cell_num = 189;
    uint16_t adc_cell_den = 100;

    // --- schema_version >= 7: ticker look ----------------------------------
    // LED rule (logic/ticker.h LedRule): 0 = off - the payload's alert.led as
    // before; 1 = sign - red / green by the payload's direction (`dir`, or
    // the sign of `change`) when the payload carries no alert.led. Stored as
    // "off" | "sign"; `led_rule` in GET /api/config and POST /config.
    uint8_t led_rule = 0;

    // --- schema_version >= 8: content source -------------------------------
    // What the device shows content from (logic/source.h SourceKind): only
    // `ticker` changes the pull target (the ticker source replaces pull_url);
    // `url` / `text` / `mqtt` / `none` record the editor's choice and are
    // derived from pull_url for files older than schema 8.
    uint8_t    source_kind = 0;
    // The ticker source: preset (coingecko | kraken | binance | custom),
    // symbol / market, the custom URL and JSONPath-lite paths, decimals
    // (255 = auto), separator, label. Stored as tk_* in config.json.
    SourceSpec ticker = {};
    // Reserved for keyed APIs (none of the presets needs one): stored, never
    // echoed (`tk_api_key_set` in GET /api/config), not used by any fetch.
    char       tk_api_key[CONFIG_API_KEY_LEN] = "";

    AppConfig() { source_spec_defaults(&ticker); }
};

// power_source <-> "auto"/"usb"/"battery" (unknown strings -> auto).
const char* config_power_source_str(uint8_t v);
uint8_t     config_power_source_parse(const char* s);

void config_init();

// Loads /config.json into `config`. On a missing or corrupt file the defaults
// are kept and false is returned (the error is logged).
bool config_load(AppConfig& config);

// Atomic save: writes /config.json.tmp then renames it over /config.json.
bool config_save(const AppConfig& config);

// Optional TLS root CA (PEM) for HTTPS pull, stored in /ca.pem.
bool   config_ca_exists();
String config_ca_load();                  // empty String if absent / unreadable
bool   config_ca_save(const char* pem);   // pem must look like a PEM certificate
bool   config_ca_remove();
