#include "connectivity_manager.h"
#include "../log.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include "ota_manager.h"
#include "arduino_ota.h"
#include "wifi_portal.h"
#include "wifi_link.h"
#include "recovery_manager.h"
#include "peer_manager.h"
#include "pairing_manager.h"
#include "relay_manager.h"
#include "layout_api.h"
#include "screen_api.h"
#include "power_api.h"
#include "../logic/peer_table.h"
#include "../logic/auth_policy.h"
#include "../web/www.h"
#include "../hal/hal_power.h"
#include "../hal/hal_indication.h"
#include "../hal/hal_display.h"
#include "../logic/command_queue.h"
#include "../logic/payload.h"
#include "../logic/rtttl.h"
#include "../logic/renderer.h"   // renderer_set_led_rule, renderer_apply
#include "../logic/source.h"     // ticker source: presets, extraction
#include "../logic/spark_hist.h" // on-device sparkline history
#include "../generated/ca_bundle.h"   // TLS roots of the ticker sources (scripts/build_ca_bundle.py)
#include <ctype.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const uint16_t MQTT_BUFFER_SIZE     = PAYLOAD_MAX_LEN + 256;   // payload + MQTT header/topic
static const uint16_t MQTT_KEEPALIVE_S     = 30;
static const uint16_t MQTT_SOCKET_TIMEOUT_S = 5;
static const unsigned long MQTT_BACKOFF_MIN_MS = 5000;
static const unsigned long MQTT_BACKOFF_MAX_MS = 30000;
static const uint32_t HTTP_PULL_TIMEOUT_MS = 10000;
// Wi-Fi connect budget with saved credentials. Battery: unchanged 20 s (the
// caller deep-sleeps with backoff). USB: WiFiManager waited for a definitive
// result without a bound; 30 s covers a slow DHCP without hanging forever.
static const uint32_t STA_CONNECT_TIMEOUT_BATTERY_MS = 20000;
static const uint32_t STA_CONNECT_TIMEOUT_USB_MS = 30000;
static const uint32_t PORTAL_TIMEOUT_MS = 180000;   // idle timeout of the setup portal
static const char* JSON_CT = "application/json";
static const char* HTML_CT = "text/html";

// ---------------------------------------------------------------------------
// Web authentication (shared helper, see header)
// ---------------------------------------------------------------------------
static char s_api_token[sizeof(AppConfig::api_token)] = "";

void web_auth_set_token(const char* token) {
    strlcpy(s_api_token, token ? token : "", sizeof(s_api_token));
}

// Constant-time comparison: runs over the secret's length regardless of input.
static bool ct_equal(const char* input, size_t in_len, const char* secret, size_t sec_len) {
    if (sec_len == 0) return false;
    unsigned diff = (unsigned)(in_len ^ sec_len);
    for (size_t i = 0; i < sec_len; i++) {
        // index into input without branching on secret data; wraps when input is shorter
        char c = (in_len > 0) ? input[i % in_len] : 0;
        diff |= (unsigned)((unsigned char)c ^ (unsigned char)secret[i]);
    }
    return diff == 0;
}

bool web_auth_ok(AsyncWebServerRequest* request) {
    size_t tok_len = strlen(s_api_token);
    if (tok_len == 0) return true;   // authentication disabled
    if (!request) return false;

    // 1) X-Api-Token header
    if (request->hasHeader("X-Api-Token")) {
        const String& v = request->header("X-Api-Token");
        if (ct_equal(v.c_str(), v.length(), s_api_token, tok_len)) return true;
    }

    // 2) HTTP Basic: any username, password = token
    if (request->hasHeader("Authorization")) {
        const String& auth = request->header("Authorization");
        if (auth.startsWith("Basic ")) {
            const char* b64 = auth.c_str() + 6;
            while (*b64 == ' ') b64++;
            unsigned char decoded[160];
            size_t out_len = 0;
            int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                                           (const unsigned char*)b64, strlen(b64));
            if (rc == 0 && out_len > 0) {
                decoded[out_len] = '\0';
                const char* colon = strchr((const char*)decoded, ':');
                if (colon) {
                    const char* pass = colon + 1;
                    size_t pass_len = out_len - (size_t)(pass - (const char*)decoded);
                    if (ct_equal(pass, pass_len, s_api_token, tok_len)) return true;
                }
            }
        }
    }
    return false;
}

// The Basic challenge only for navigations (Accept: text/html); fetch/XHR get a
// bare 401 and the page shows its inline token field (auth_policy.h).
void web_auth_reject(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* resp = request->beginResponse(401, JSON_CT, "{\"error\":\"unauthorized\"}");
    if (request->hasHeader("Accept") && auth_challenge_wanted(request->header("Accept").c_str())) {
        resp->addHeader("WWW-Authenticate", "Basic realm=\"TickrDisplay\"");
    }
    request->send(resp);
}

bool web_auth_enabled() {
    return s_api_token[0] != '\0';
}

void web_auth_reject_no_token(AsyncWebServerRequest* request) {
    request->send(403, JSON_CT, "{\"error\":\"set an API token on /system first\"}");
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static String html_escape(const char* s) {
    String out;
    if (!s) return out;
    out.reserve(strlen(s) + 8);
    for (; *s; s++) {
        switch (*s) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *s;          break;
        }
    }
    return out;
}

// Minimal JSON string escaping for our own short messages.
static String json_escape(const char* s) {
    String out;
    if (!s) return out;
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { out += '\\'; out += *s; }
        else if ((unsigned char)*s < 0x20) { out += ' '; }
        else out += *s;
    }
    return out;
}

static void send_json_error(AsyncWebServerRequest* request, int code, const char* msg) {
    String body = "{\"status\":\"error\",\"error\":\"";
    body += json_escape(msg);
    body += "\"}";
    request->send(code, JSON_CT, body);
}

static bool is_printable_ascii(const char* s, bool allow_space) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x21 || c > 0x7E) {
            if (c == ' ' && allow_space) continue;
            return false;
        }
    }
    return true;
}

static uint8_t param_u8(AsyncWebServerRequest* request, const char* name, uint8_t def) {
    if (!request->hasParam(name)) return def;
    long v = request->getParam(name)->value().toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static void send_queued(AsyncWebServerRequest* request, bool ok) {
    if (ok) {
        String body = "{\"status\":\"queued\",\"queue\":";
        body += command_queue_depth();
        body += "}";
        request->send(202, JSON_CT, body);
    } else {
        send_json_error(request, 503, "command queue full, retry later");
    }
}

// ---------------------------------------------------------------------------
// ConnectivityManager
// ---------------------------------------------------------------------------
ConnectivityManager::ConnectivityManager() : server(80), mqtt(espClient) {
}

AppConfig& ConnectivityManager::getConfig() {
    return _config;
}

void ConnectivityManager::setLastError(const char* msg) {
    strlcpy(_last_error, msg ? msg : "", sizeof(_last_error));
}

bool ConnectivityManager::init(DataCallback onData, bool low_power) {
    _onData = onData;

    config_init();
    if (!config_load(_config)) {
        Serial.println("Config: defaults in use");
    }
    web_auth_set_token(_config.api_token);
    renderer_set_led_rule(_config.led_rule);

    // Hostname "<device-name>-XXXXXX", event counters (managers/wifi_link.h):
    // before WiFi.mode() - the core applies the hostname when the STA starts.
    wifi_link_begin(_config.device_name);

    // Bring the Wi-Fi driver up in its default persistent mode (credentials
    // live in NVS) and check whether it has a saved network. This is the same
    // NVS record the stock firmware and WiFiManager use, so switching slots
    // keeps the network.
    WiFi.mode(WIFI_STA);
    wifi_config_t conf = {};
    bool has_creds = esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
    _has_creds = has_creds;
    wifi_portal_set_saved_credentials(has_creds);

    // The web server starts before the connection so that /update and the
    // setup portal are reachable in AP mode as well.
    setup_webserver();

    // 1) Saved credentials: plain WiFi.begin() (what WiFiManager did
    //    internally), bounded wait. Battery mode keeps its 20 s budget and
    //    never opens the portal; the caller deep-sleeps with backoff.
    if (has_creds) {
        LOGV("WiFi: connecting to saved network '%s'\n", (const char*)conf.sta.ssid);
        WiFi.begin();
        if (wait_for_sta(low_power ? STA_CONNECT_TIMEOUT_BATTERY_MS : STA_CONNECT_TIMEOUT_USB_MS)) {
            return on_wifi_connected(low_power);
        }
        Serial.println("WiFi connect failed");
        if (low_power) return false;
    }

    // 2) No credentials, or the connect failed on USB power: open the setup
    //    portal (AP "TickrDisplay", 192.168.244.1) for PORTAL_TIMEOUT_MS. The
    //    timeout only counts while nobody is connected to the AP, exactly
    //    like WiFiManager's portal. On timeout the caller restarts (USB) or
    //    restarts into the portal again (battery, first setup).
    wifi_portal_start(WIFI_PORTAL_AP_SSID);
    uint32_t deadline = millis() + PORTAL_TIMEOUT_MS;
    bool connected = false;
    while (true) {
        wifi_portal_tick();
        recovery_loop();   // counter window + recovery actions requested through the portal AP
        // Either the user connected through the portal, or the saved network
        // came back and the driver's auto-reconnect joined it meanwhile.
        if (wifi_portal_connected() || WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        if (WiFi.softAPgetStationNum() > 0) {
            deadline = millis() + PORTAL_TIMEOUT_MS;   // somebody is configuring: keep the AP up
        } else if ((int32_t)(millis() - deadline) >= 0) {
            break;
        }
        delay(10);
    }
    if (connected) {
        // Let the browser fetch the "connected, IP x.x.x.x" status before the
        // AP disappears from under it.
        uint32_t grace = millis() + 3000;
        while ((int32_t)(millis() - grace) < 0) {
            wifi_portal_tick();
            delay(10);
        }
    }
    wifi_portal_stop();
    if (!connected) {
        Serial.println("Config portal timed out");
        return false;
    }
    return on_wifi_connected(low_power);
}

// Polls WiFi.status() for up to `timeout_ms`. Gives up early only on an
// authentication failure; "network not found" is retried by the driver's
// auto-reconnect until the deadline (a router that is still booting shows up
// a few seconds later) - the same rule WiFiManager's bounded wait applied.
bool ConnectivityManager::wait_for_sta(uint32_t timeout_ms) {
    uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(millis() - deadline) < 0) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) return true;
        if (st == WL_CONNECT_FAILED) return false;
        recovery_loop();   // the counter's 20 s window must not wait for a slow connect
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool ConnectivityManager::on_wifi_connected(bool low_power) {
    Serial.println("WiFi Connected");
    Serial.println(WiFi.localIP());

    // Allow time for WiFi stack to stabilize
    delay(100);

    // The awake device keeps its link itself from here on (docs/DEVICE_UI.md
    // "Wi-Fi link supervision"); a battery device sleeps before loop() runs
    // and begins afresh on every wake-up.
    if (!low_power) wifi_link_arm();

    // Stable client id derived from the MAC address: tickrdisplay-XXXXXX
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_mqtt_client_id, sizeof(_mqtt_client_id), "tickrdisplay-%02X%02X%02X", mac[3], mac[4], mac[5]);

    setup_mqtt();
    return true;
}

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------
// GET /api/config - everything the /system page needs to fill its form.
// Secrets (mqtt_pass, api_token, ota_password, the CA itself) are reported
// only as "set / not set" flags, never returned.
void ConnectivityManager::handle_config_api(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    DynamicJsonDocument doc(1536);
    doc["mqtt_server"]      = _config.mqtt_server;
    doc["mqtt_port"]        = _config.mqtt_port;
    doc["mqtt_topic"]       = _config.mqtt_topic;
    doc["mqtt_user"]        = _config.mqtt_user;
    doc["mqtt_pass_set"]    = strlen(_config.mqtt_pass) > 0;
    doc["pull_url"]         = _config.pull_url;
    doc["refresh_interval"] = _config.refresh_interval_min;
    doc["tls_ca_set"]       = config_ca_exists();
    doc["api_token_set"]    = strlen(_config.api_token) > 0;
    doc["ota_password_set"] = strlen(_config.ota_password) > 0;
    doc["group_set"]        = pairing_has_group();     // the credentials themselves never leave the device
    doc["arduino_ota_build"] = ota_arduino_built();   // UI hides the OTA password field otherwise
    doc["adc_cell_num"]     = _config.adc_cell_num;   // cell divider (docs/HARDWARE.md "Power sensing")
    doc["adc_cell_den"]     = _config.adc_cell_den;
    doc["led_rule"]         = led_rule_str(_config.led_rule);   // "off" | "sign" (docs/TICKERS.md "What the screen shows")
    // Content source (schema 8, docs/TICKERS.md "Presets"): the editor pre-fills its Ticker form from these.
    const SourceSpec& t = _config.ticker;
    doc["source_kind"]      = source_kind_str(_config.source_kind);
    doc["tk_preset"]        = source_preset_str(t.preset);
    doc["tk_symbol"]        = t.symbol;
    doc["tk_market"]        = t.market;
    doc["tk_url"]           = t.url;
    doc["tk_price"]         = t.path_price;
    doc["tk_change"]        = t.path_change;
    doc["tk_spark"]         = t.path_spark;
    doc["tk_mode"]          = source_change_mode_str(t.change_mode);
    doc["tk_decimals"]      = t.decimals;                        // 255 = auto
    doc["tk_sep"]           = source_sep_str(t.sep);
    doc["tk_label"]         = t.label;
    doc["tk_api_key_set"]   = _config.tk_api_key[0] != '\0';    // reserved; the key itself never leaves the device
    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* resp = request->beginResponse(200, JSON_CT, body);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

// Form-POST helpers (POST /config, POST /api/source/test): a parameter's value;
// a bounded printable-ASCII copy with the error text appended to `err`.
static bool getp(AsyncWebServerRequest* request, const char* name, String& out) {
    if (!request->hasParam(name, true)) return false;
    out = request->getParam(name, true)->value();
    return true;
}
static void copy_field(AsyncWebServerRequest* request, const char* name, char* dst, size_t dst_len, bool allow_space, String& err) {
    String v;
    if (!getp(request, name, v)) return;
    v.trim();
    if (v.length() >= dst_len) {
        err += String(name) + ": too long (max " + String(dst_len - 1) + " chars). ";
        return;
    }
    if (!is_printable_ascii(v.c_str(), allow_space)) {
        err += String(name) + ": invalid characters. ";
        return;
    }
    strlcpy(dst, v.c_str(), dst_len);
}
// Symbols, markets and paths travel in URLs and JSON keys: letters, digits and
// a few punctuation marks, no spaces or quotes.
static bool is_token(const char* s) {
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr("._-*[]$", *s)) return false;
    }
    return true;
}
// An enum parameter: `parse` maps the text, `str` maps it back - a mismatch
// means the text was not one of the names.
static void enum_field(AsyncWebServerRequest* request, const char* name, uint8_t (*parse)(const char*),
                       const char* (*str)(uint8_t), uint8_t* dst, String& err) {
    String v;
    if (!getp(request, name, v)) return;
    uint8_t e = parse(v.c_str());
    if (v != str(e)) { err += name; err += ": not one of the allowed values. "; }
    else *dst = e;
}

// The ticker source (schema 8, docs/TICKERS.md "Presets") from form parameters -
// POST /config saves them, POST /api/source/test fetches them once. `kind`
// may be NULL (the test has no source_kind). Only the parameters present
// change anything; errors are appended to `err`.
void ConnectivityManager::parse_source_params(AsyncWebServerRequest* request, uint8_t* kind, SourceSpec& tk, String& err) {
    String v;
    if (kind) enum_field(request, "source_kind", source_kind_parse, source_kind_str, kind, err);
    enum_field(request, "tk_preset", source_preset_parse, source_preset_str, &tk.preset, err);
    enum_field(request, "tk_mode", source_change_mode_parse, source_change_mode_str, &tk.change_mode, err);
    enum_field(request, "tk_sep", source_sep_parse, source_sep_str, &tk.sep, err);
    copy_field(request, "tk_symbol", tk.symbol, sizeof(tk.symbol), false, err);
    copy_field(request, "tk_market", tk.market, sizeof(tk.market), false, err);
    copy_field(request, "tk_url", tk.url, sizeof(tk.url), false, err);
    copy_field(request, "tk_price", tk.path_price, sizeof(tk.path_price), false, err);
    copy_field(request, "tk_change", tk.path_change, sizeof(tk.path_change), false, err);
    copy_field(request, "tk_spark", tk.path_spark, sizeof(tk.path_spark), false, err);
    copy_field(request, "tk_label", tk.label, sizeof(tk.label), true, err);
    if (!is_token(tk.symbol) || !is_token(tk.market) || !is_token(tk.path_price) || !is_token(tk.path_change) || !is_token(tk.path_spark)) {
        err += "tk_symbol/market/price/change/spark: letters, digits, . _ - only. ";
    }
    if (strpbrk(tk.url, "\"\\") || strpbrk(tk.label, "\"\\")) err += "tk_url / tk_label: no quotes or backslashes. ";
    if (tk.url[0] && strncasecmp(tk.url, "http://", 7) != 0 && strncasecmp(tk.url, "https://", 8) != 0) {
        err += "tk_url: must start with http:// or https://. ";
    }
    if (getp(request, "tk_decimals", v)) {
        long d = v == "auto" ? SRC_DECIMALS_AUTO : v.toInt();
        if (d != SRC_DECIMALS_AUTO && (d < 0 || d > SRC_DECIMALS_MAX || !isdigit((unsigned char)v[0]))) err += "tk_decimals: auto or 0..6. ";
        else tk.decimals = (uint8_t)d;
    }
    // ticker = the pull target: it must resolve to a URL
    SourcePlan plan;
    if ((!kind || *kind == SRC_KIND_TICKER) && !source_resolve(tk, &plan)) {
        err += tk.preset == SRC_PRESET_CUSTOM ? "ticker: URL and price path required. " : "ticker: symbol required. ";
    }
}

void ConnectivityManager::handle_config_post(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);

    AppConfig nc = _config;   // validate into a copy, commit only if everything is fine
    String err;
    #define getp(name, out) getp(request, name, out)
    #define copy_field(name, dst, len, sp) copy_field(request, name, dst, len, sp, err)

    copy_field("mqtt_server", nc.mqtt_server, sizeof(nc.mqtt_server), false);
    copy_field("mqtt_topic",  nc.mqtt_topic,  sizeof(nc.mqtt_topic),  false);
    copy_field("mqtt_user",   nc.mqtt_user,   sizeof(nc.mqtt_user),   false);
    copy_field("pull_url",    nc.pull_url,    sizeof(nc.pull_url),    false);

    String v;
    if (getp("mqtt_port", v)) {
        long port = v.toInt();
        if (port < 1 || port > 65535) err += "mqtt_port: must be 1..65535. ";
        else nc.mqtt_port = (int)port;
    }
    if (getp("refresh_interval", v)) {
        long iv = v.toInt();
        if (iv < CONFIG_REFRESH_MIN_MINUTES || iv > CONFIG_REFRESH_MAX_MINUTES) {
            err += "refresh_interval: must be " + String(CONFIG_REFRESH_MIN_MINUTES) + ".." +
                   String(CONFIG_REFRESH_MAX_MINUTES) + " minutes. ";
        } else {
            nc.refresh_interval_min = (int)iv;
        }
    }
    // Cell divider (schema 6): both fields together, validated as a ratio.
    String vd;
    bool has_cn = getp("adc_cell_num", v), has_cd = getp("adc_cell_den", vd);
    if (has_cn || has_cd) {
        long cn = has_cn ? v.toInt() : nc.adc_cell_num;
        long cd = has_cd ? vd.toInt() : nc.adc_cell_den;
        if (cn < 0 || cn > 65535 || cd < 0 || cd > 65535 || !cell_divider_valid((uint16_t)cn, (uint16_t)cd)) {
            err += "adc_cell_num/den: ratio must be 1.0..4.0, den 1..1000. ";
        } else {
            nc.adc_cell_num = (uint16_t)cn;
            nc.adc_cell_den = (uint16_t)cd;
        }
    }
    if (getp("led_rule", v)) {
        if (v == "off" || v == "sign") nc.led_rule = led_rule_parse(v.c_str());
        else err += "led_rule: must be off or sign. ";
    }

    // Content source (schema 8, docs/TICKERS.md "Presets"). The editor posts only the
    // fields of the chosen source; everything else keeps its value.
    bool has_kind = request->hasParam("source_kind", true);
    parse_source_params(request, &nc.source_kind, nc.ticker, err);
    if (request->hasParam("tk_api_key_clear", true)) {
        nc.tk_api_key[0] = '\0';
    } else if (getp("tk_api_key", v) && v.length() > 0) {
        if (v.length() >= sizeof(nc.tk_api_key)) err += "tk_api_key: too long. ";
        else if (!is_printable_ascii(v.c_str(), false)) err += "tk_api_key: invalid characters. ";
        else strlcpy(nc.tk_api_key, v.c_str(), sizeof(nc.tk_api_key));
    }
    if (strlen(nc.pull_url) > 0 &&
        strncasecmp(nc.pull_url, "http://", 7) != 0 && strncasecmp(nc.pull_url, "https://", 8) != 0) {
        err += "pull_url: must start with http:// or https://. ";
    }
    if (strlen(nc.mqtt_server) > 0 && strlen(nc.mqtt_topic) == 0) {
        err += "mqtt_topic: required when MQTT server is set. ";
    }

    // Secrets: empty = keep, checkbox = clear
    if (request->hasParam("mqtt_pass_clear", true)) {
        nc.mqtt_pass[0] = '\0';
    } else if (getp("mqtt_pass", v) && v.length() > 0) {
        if (v.length() >= sizeof(nc.mqtt_pass)) err += "mqtt_pass: too long. ";
        else strlcpy(nc.mqtt_pass, v.c_str(), sizeof(nc.mqtt_pass));
    }
    if (request->hasParam("api_token_clear", true)) {
        nc.api_token[0] = '\0';
    } else if (getp("api_token", v) && v.length() > 0) {
        if (v.length() >= sizeof(nc.api_token)) err += "api_token: too long. ";
        else if (!is_printable_ascii(v.c_str(), false)) err += "api_token: invalid characters. ";
        else strlcpy(nc.api_token, v.c_str(), sizeof(nc.api_token));
    }
    if (request->hasParam("ota_password_clear", true)) {
        nc.ota_password[0] = '\0';
    } else if (getp("ota_password", v) && v.length() > 0) {
        if (v.length() >= sizeof(nc.ota_password)) err += "ota_password: too long. ";
        else if (!is_printable_ascii(v.c_str(), false)) err += "ota_password: invalid characters. ";
        else strlcpy(nc.ota_password, v.c_str(), sizeof(nc.ota_password));
    }

    // Optional root CA for HTTPS pull
    bool ca_clear = request->hasParam("tls_ca_clear", true);
    String ca_pem;
    bool ca_set = !ca_clear && getp("tls_ca_pem", ca_pem) && ca_pem.length() > 0;
    if (ca_set) {
        ca_pem.trim();
        if (ca_pem.indexOf("-----BEGIN CERTIFICATE-----") < 0 || ca_pem.indexOf("-----END CERTIFICATE-----") < 0) {
            err += "tls_ca_pem: not a PEM certificate. ";
        }
    }

    if (err.length() > 0) {
        String body = "<p>Configuration NOT saved:</p><pre>" + html_escape(err.c_str()) +
                      "</pre><a href='/system'>Back</a>";
        request->send(400, HTML_CT, body);
        return;
    }

    if (ca_clear) {
        config_ca_remove();
    } else if (ca_set) {
        if (!config_ca_save(ca_pem.c_str())) {
            request->send(500, HTML_CT, "Failed to store CA certificate. <a href='/system'>Back</a>");
            return;
        }
    }

    if (!config_save(nc)) {
        request->send(500, HTML_CT, "Failed to write configuration. <a href='/system'>Back</a>");
        return;
    }

    // A Pull URL saved without a source_kind (the /system form) selects `url`;
    // clearing it drops back to `none` - the editor's pre-selection follows.
    if (!has_kind && nc.source_kind != SRC_KIND_TICKER) {
        nc.source_kind = source_pull_kind(nc.source_kind, nc.pull_url[0] != '\0');
    }
    // A changed Pull URL / ticker source / interval fetches at once on USB (main.cpp polls the flag).
    if (strcmp(nc.pull_url, _config.pull_url) != 0 || nc.refresh_interval_min != _config.refresh_interval_min ||
        nc.source_kind != _config.source_kind || memcmp(&nc.ticker, &_config.ticker, sizeof(SourceSpec)) != 0) {
        _pull_reconfigure = true;
    }
    _config = nc;
    web_auth_set_token(_config.api_token);
    power_set_cell_divider(_config.adc_cell_num, _config.adc_cell_den);
    renderer_set_led_rule(_config.led_rule);
    _mqtt_reconfigure = true;   // applied from loop(); MQTT socket must not be touched here

    request->send(200, HTML_CT, "Configuration Saved. <a href='/system'>Back</a>");
    #undef getp
    #undef copy_field
}

// GET /api/status - public: no secrets, and its auth_enabled flag is
// what tells a page without a token that it is read-only.
void ConnectivityManager::handle_status(AsyncWebServerRequest* request) {
    StaticJsonDocument<1024> doc;
    doc["uptime_s"] = (uint32_t)(millis() / 1000);
    doc["heap_free"] = ESP.getFreeHeap();
    doc["heap_min_free"] = ESP.getMinFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    // Dashboard fields (formerly template placeholders in the HTML page)
    // "usb" | "battery" | "unknown" (no detector signal in auto mode - the
    // firmware then behaves as USB); the override in power_mode; board
    // profile "A" | "B" | "?" (docs/HARDWARE.md "Power sensing"). vsys_v = the
    // cell (GPIO 32 x cell divider), vin_v = the rail (GPIO 33 x 2.1); null
    // on board ? only. Raw ADC data: GET /api/power/raw.
    doc["power"] = power_source_label();
    doc["power_mode"] = config_power_source_str(_config.power_source);
    doc["board"] = power_board_str();
    if (power_battery_measurable()) {
        doc["vsys_v"] = power_get_voltage_vsys();
        doc["vin_v"] = power_get_voltage_vin();
    } else {
        doc["vsys_v"] = nullptr;
        doc["vin_v"] = nullptr;
    }
    doc["pull_https"] = strncasecmp(_config.pull_url, "https://", 8) == 0;
    doc["tls_ca"] = config_ca_exists();
    doc["mqtt_enabled"] = strlen(_config.mqtt_server) > 0;
    doc["mqtt_connected"] = mqtt.connected();
    doc["mqtt_state"] = mqtt.state();
    doc["queue_depth"] = command_queue_depth();
    doc["queue_dropped"] = command_queue_dropped();
    doc["auth_enabled"] = strlen(_config.api_token) > 0;
    doc["tls_insecure_used"] = _tls_insecure_used;
    doc["reset_reason"] = (int)esp_reset_reason();
    doc["last_error"] = _last_error;
    // Link diagnostics (managers/wifi_link.h): counters kept across software
    // restarts and deep sleep, zero after a power-on.
    WifiLinkStats wl;
    wifi_link_stats(&wl);
    doc["wifi_disconnects"] = wl.disconnects;
    doc["wifi_last_reason"] = wl.last_reason;
    doc["wifi_down_s"] = wl.down_s;
    doc["wifi_reconnects"] = wl.reconnects;
    doc["wifi_restarts"] = wl.restarts;
    // Read-only LED state: the colour actually driven to the LEDs (an active
    // overlay - OTA, identify, pairing... - wins over the base colour), so the
    // panel's LED bar can mirror the real device (docs/WEB_UI.md "Card states and badges").
    uint8_t lr, lg, lb;
    indication_get_written_rgb(&lr, &lg, &lb);
    JsonObject led = doc.createNestedObject("led");
    led["r"] = lr;
    led["g"] = lg;
    led["b"] = lb;
    String body;
    serializeJson(doc, body);
    // Multi-device discovery counters (peer_manager.h), appended as a raw object.
    char peers[192];
    peers_stats_json(peers, sizeof(peers));
    body.remove(body.length() - 1);
    body += ",\"peers\":";
    body += peers;
    body += '}';
    request->send(200, JSON_CT, body);
}

int ConnectivityManager::enqueue_payload(const char* data, size_t len, uint8_t src, char* err, size_t err_len) {
    ScreenPayload* p = (ScreenPayload*)malloc(sizeof(ScreenPayload));
    if (!p) {
        snprintf(err, err_len, "out of memory");
        return 500;
    }
    PayloadResult r = payload_parse(data, len, p, err, err_len);
    free(p);
    if (r == PAYLOAD_ERR_TOO_LARGE) return 413;
    if (r != PAYLOAD_OK) {
        setLastError(err);
        return 400;
    }
    if (!command_queue_push(CMD_PAYLOAD, (CommandSource)src, data, len, PAYLOAD_MAX_LEN)) {
        snprintf(err, err_len, "command queue full");
        setLastError(err);
        return 503;
    }
    return 202;
}

void ConnectivityManager::handle_screen_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!web_auth_ok(request)) return;                    // final handler answers 401
    if (total == 0 || total > PAYLOAD_MAX_LEN) return;    // final handler answers 411/413
    if (index == 0 && request->_tempObject == nullptr) {
        request->_tempObject = malloc(total + 1);         // freed by the request destructor
        if (request->_tempObject) ((char*)request->_tempObject)[0] = '\0';
    }
    char* buf = (char*)request->_tempObject;
    if (!buf) return;
    if (index + len > total) return;                      // malformed; ignore extra bytes
    memcpy(buf + index, data, len);
    if (index + len == total) buf[total] = '\0';
}

void ConnectivityManager::handle_screen_done(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    size_t total = request->contentLength();
    if (total == 0) {
        send_json_error(request, 411, "JSON body with Content-Length required");
        return;
    }
    if (total > PAYLOAD_MAX_LEN) {
        send_json_error(request, 413, "payload too large (max 4096 bytes)");
        return;
    }
    char* buf = (char*)request->_tempObject;
    if (!buf) {
        send_json_error(request, 400, "no raw JSON body received (use Content-Type: application/json)");
        return;
    }
    char err[96] = "";
    int code = enqueue_payload(buf, total, SRC_HTTP, err, sizeof(err));
    if (code == 202) {
        String body = "{\"status\":\"queued\",\"queue\":";
        body += command_queue_depth();
        if (err[0]) { body += ",\"warning\":\""; body += json_escape(err); body += "\""; }
        body += "}";
        request->send(202, JSON_CT, body);
    } else {
        send_json_error(request, code, err);
    }
}

// ---------------------------------------------------------------------------
// Identity (docs/API.md "Peers, layout, beacon")
// ---------------------------------------------------------------------------
// GET /api/identity - open like "/": everything in it is already broadcast in
// the beacon (name, MAC, IP, version, power); no secrets, group secret never.
void ConnectivityManager::handle_identity_get(AsyncWebServerRequest* request) {
    char id[16];
    peers_self_id(id, sizeof(id));
    char name[CONFIG_NAME_LEN];
    if (_config.device_name[0]) strlcpy(name, _config.device_name, sizeof(name));
    else peers_default_name(name, sizeof(name));
    bool usb = power_get_source() == POWER_USB;
    // Fixed shape, all strings validated printable ASCII without quotes -> snprintf.
    // "relay" = USB power + group: this device parks payloads for sleepers;
    // "group" is {"id","name","epoch"} or null (never the secret).
    char group[128];
    pairing_group_json(group, sizeof(group));
    char body[704];
    snprintf(body, sizeof(body),
        "{\"id\":\"%s\",\"name\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\",\"version\":\"%s\","
        "\"power\":\"%s\",\"board\":\"%s\",\"sleep_s\":%d,\"relay\":%s,\"pairable\":%s,\"group\":%s,"
        "\"layout\":{\"x\":%u,\"y\":%u,\"group\":\"%s\"},"
        "\"screen\":{\"width\":296,\"height\":128,\"format\":\"1bpp-msb\"},"
        "\"caps\":[\"screen_raw\",\"screen_bmp\",\"peers_v1\",\"pair_v1\",\"layout_v1\",\"relay_v1\",\"update_url_v1\"],\"max_peers\":%u,\"uptime_s\":%lu}",
        id, name, WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str(), TICKR_FW_VERSION,
        usb ? "usb" : "battery", power_board_str(), usb ? 0 : _config.refresh_interval_min * 60,
        relay_is_relay() ? "true" : "false",
        usb ? "true" : "false",                 // pairable = USB power only
        group,
        (unsigned)_config.layout_x, (unsigned)_config.layout_y, _config.layout_group,
        (unsigned)TICKR_MAX_PEERS, (unsigned long)(millis() / 1000));
    AsyncWebServerResponse* resp = request->beginResponse(200, JSON_CT, body);
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
}

// Body collector for small JSON requests (POST /api/identity). Unauthenticated
// bodies are not stored; the request handler answers 401.
static void collect_json_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    const size_t kMaxBody = 384;
    if (!web_auth_ok(request)) return;
    if (total == 0 || total > kMaxBody) return;
    if (index == 0) request->_tempObject = calloc(total + 1, 1);   // freed by ~AsyncWebServerRequest
    if (request->_tempObject && index + len <= total) memcpy((uint8_t*)request->_tempObject + index, data, len);
}

// POST /api/identity {"name":"...","layout":{"x":0,"y":0,"group":"shelf"}}
// (or form fields name / layout_x / layout_y / layout_group). Persists to
// config.json (schema 4) and updates the beacon name. Single-device write:
// normal auth rule.
void ConnectivityManager::handle_identity_post(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    AppConfig nc = _config;
    String err;
    String name, group;
    long lx = nc.layout_x, ly = nc.layout_y;
    bool has_name = false, has_group = false;

    if (request->_tempObject) {
        StaticJsonDocument<384> doc;
        const char* body = (const char*)request->_tempObject;
        if (deserializeJson(doc, body, strlen(body)) != DeserializationError::Ok || !doc.is<JsonObject>()) {
            send_json_error(request, 400, "invalid JSON body");
            return;
        }
        if (doc.containsKey("name")) { has_name = true; name = doc["name"] | ""; }
        JsonVariant layout = doc["layout"];
        if (layout.is<JsonObject>()) {
            lx = layout["x"] | lx;
            ly = layout["y"] | ly;
            if (layout.containsKey("group")) { has_group = true; group = layout["group"] | ""; }
        }
    } else {
        if (request->hasParam("name", true)) { has_name = true; name = request->getParam("name", true)->value(); }
        if (request->hasParam("layout_x", true)) lx = request->getParam("layout_x", true)->value().toInt();
        if (request->hasParam("layout_y", true)) ly = request->getParam("layout_y", true)->value().toInt();
        if (request->hasParam("layout_group", true)) { has_group = true; group = request->getParam("layout_group", true)->value(); }
    }

    auto check_text = [&](const char* field, String& v, char* dst, size_t dst_len) {
        v.trim();
        if (v.length() >= dst_len) { err += String(field) + ": too long (max " + String(dst_len - 1) + " chars). "; return; }
        if (!is_printable_ascii(v.c_str(), true) || v.indexOf('"') >= 0 || v.indexOf('\\') >= 0) {
            err += String(field) + ": printable ASCII without quotes/backslashes only. ";
            return;
        }
        strlcpy(dst, v.c_str(), dst_len);
    };
    if (has_name) check_text("name", name, nc.device_name, sizeof(nc.device_name));
    if (has_group) check_text("layout.group", group, nc.layout_group, sizeof(nc.layout_group));
    if (lx < 0 || lx > CONFIG_LAYOUT_MAX || ly < 0 || ly > CONFIG_LAYOUT_MAX) {
        err += "layout.x/y: must be 0.." + String(CONFIG_LAYOUT_MAX) + ". ";
    } else {
        nc.layout_x = (uint8_t)lx;
        nc.layout_y = (uint8_t)ly;
    }
    if (err.length() > 0) {
        send_json_error(request, 400, err.c_str());
        return;
    }
    if (!config_save(nc)) {
        send_json_error(request, 500, "failed to write configuration");
        return;
    }
    _config = nc;
    peers_set_name(_config.device_name);
    handle_identity_get(request);
}

void ConnectivityManager::setup_webserver() {
    LOGVLN("Starting Web Server...");

    // CORS: the panel served by one device calls
    // the API of every other one from the browser. "*" is safe - no cookies,
    // the token travels in X-Api-Token. Preflight for /api/*, /update and
    // POST /config (the shelf's content editor writes another device's Pull URL) answers 204.
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Expose-Headers", "ETag, X-Screen-Width, X-Screen-Height, X-Screen-Format");
    auto preflight = [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* resp = request->beginResponse(204);
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, X-Api-Token, Authorization, If-None-Match, X-Tickr-Group");
        resp->addHeader("Access-Control-Max-Age", "86400");
        request->send(resp);
    };
    server.on("/api/*", HTTP_OPTIONS, preflight);
    server.on("/update", HTTP_OPTIONS, preflight);
    server.on("/config", HTTP_OPTIONS, preflight);

    // HTML pages: gzip'ed PROGMEM assets (src/web/www.h) that fill themselves
    // from the JSON API. Three pages (docs/WEB_UI.md "Pages and navigation"): "/" is
    // the shelf (panel.html), "/system" the merged settings/update page (also
    // served in AP mode - it carries the recovery upload), "/dev" the
    // diagnostics page (tickr_dev only, -DTICKR_DEV_PAGE; the release image
    // answers the JSON 404 of the catch-all). No page needs the token: without one they are
    // read-only and show an inline token field; the mutating API behind them
    // still answers 401. While the setup portal is up - or for a client of the
    // recovery AP - "/" is the Wi-Fi page (that is where the captive redirect lands).
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        www_send(request, wifi_portal_captive(request) ? "wifi.html" : "panel.html");
    });
    server.on("/system", HTTP_GET, [](AsyncWebServerRequest* request) { www_send(request, "system.html"); });
#ifdef TICKR_DEV_PAGE
    server.on("/dev", HTTP_GET, [](AsyncWebServerRequest* request) { www_send(request, "dev.html"); });
#endif
    // Legacy page URLs answer 301 so bookmarks, the docs and the vendor-style
    // muscle memory (/update) keep working. Only GET: POST /update and /u are
    // the upload routes (ota_manager) and stay as they are.
    static const struct { const char* from; const char* to; } kMoved[] = {
        {"/panel", "/"}, {"/group", "/#group"}, {"/config", "/system"}, {"/update", "/system#firmware"},
    };
    for (const auto& m : kMoved) {
        const char* to = m.to;
        server.on(m.from, HTTP_GET, [to](AsyncWebServerRequest* request) {
            AsyncWebServerResponse* r = request->beginResponse(301);
            r->addHeader("Location", to);
            request->send(r);
        });
    }
    server.on("/config", HTTP_POST, [this](AsyncWebServerRequest* request) { handle_config_post(request); });
    server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) { handle_config_api(request); });
    // Ticker source test: one fetch with the posted source, answered from the main task.
    server.on("/api/source/test", HTTP_POST, [this](AsyncWebServerRequest* request) { handle_source_test(request); });
    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) { handle_status(request); });

    // --- Hardware test API: every handler only enqueues a command ---------
    server.on("/api/test/led_red_pwm", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_LED_SET_R, SRC_HTTP, param_u8(request, "val", 0)));
    });
    server.on("/api/test/led_green_pwm", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_LED_SET_G, SRC_HTTP, param_u8(request, "val", 0)));
    });
    server.on("/api/test/led_blue_pwm", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_LED_SET_B, SRC_HTTP, param_u8(request, "val", 0)));
    });
    server.on("/api/test/volume", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_VOLUME, SRC_HTTP, param_u8(request, "val", 255)));
    });
    server.on("/api/test/play_rtttl", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        if (!request->hasParam("melody")) {
            send_json_error(request, 400, "missing 'melody' parameter");
            return;
        }
        const String& melody = request->getParam("melody")->value();
        RtttlSong song;
        if (melody.length() > RTTTL_MAX_LEN || !rtttl_parse(melody.c_str(), &song)) {
            send_json_error(request, 400, "invalid RTTTL melody");
            return;
        }
        send_queued(request, command_queue_push(CMD_PLAY_RTTTL, SRC_HTTP, melody.c_str(), melody.length(), RTTTL_MAX_LEN));
    });
    server.on("/api/test/led_red", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_LED_BLINK, SRC_HTTP, 1, 0, 0));
    });
    server.on("/api/test/led_green", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_LED_BLINK, SRC_HTTP, 0, 1, 0));
    });
    server.on("/api/test/led_blue", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        send_queued(request, command_queue_push_simple(CMD_LED_BLINK, SRC_HTTP, 0, 0, 1));
    });
    server.on("/api/test/beep", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        static const char MARIO[] = "Mario:d=4,o=5,b=100:32p,16e6,16e6,16p,16e6,16p,16c6,16e6,16p,16g6,8p,16g";
        send_queued(request, command_queue_push(CMD_PLAY_RTTTL, SRC_HTTP, MARIO, sizeof(MARIO) - 1, RTTTL_MAX_LEN));
    });
    server.on("/api/test/screen_refresh", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        static const char TXT[] = "Screen Test";
        send_queued(request, command_queue_push(CMD_SCREEN_STATUS, SRC_HTTP, TXT, sizeof(TXT) - 1, 64));
    });
    server.on("/api/test/screen_pattern", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        static const char TXT[] = "Test Pattern\n1234567890\nABCDEFGHIJ";
        send_queued(request, command_queue_push(CMD_SCREEN_MESSAGE, SRC_HTTP, TXT, sizeof(TXT) - 1, 256));
    });

    // Multi-device: read-only screen API + POST /api/screen/identify.
    // Registered BEFORE "/api/screen": the URI matcher treats a registered
    // path as "equal or startsWith(path + '/')" and the first handler wins,
    // so the payload route would otherwise swallow /api/screen/identify.
    screen_api_register_routes(server);
    // Power sensing diagnostics + power_source override (docs/HARDWARE.md "Power sensing").
    power_api_register_routes(server, &_config);

    // --- Screen payload: body is accumulated per request, processed once ---
    server.on("/api/screen", HTTP_POST,
        [this](AsyncWebServerRequest* request) { handle_screen_done(request); },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            handle_screen_body(request, data, len, index, total);
        });

    // Multi-device: identity, peer table, read-only screen API.
    server.on("/api/identity", HTTP_GET, [this](AsyncWebServerRequest* request) { handle_identity_get(request); });
    server.on("/api/identity", HTTP_POST,
        [this](AsyncWebServerRequest* request) { handle_identity_post(request); },
        NULL, collect_json_body);
    // Multi-device: relay for sleepers - before the peers routes,
    // whose "/api/peers" prefix would otherwise take /api/peers/<id>/screen.
    relay_register_routes(server);
    peers_register_routes(server);
    // Multi-device: pairing and group trust.
    pairing_register_routes(server);
    // Multi-device: fleet layout store for the panel.
    layout_register_routes(server);

    // OTA update / recovery / partition backup (see managers/ota_manager.h).
    // All mutating and data-exposing OTA endpoints use web_auth_ok() as well.
    ota_register_routes(server);

    // Power-cycle recovery mode (/api/recovery/*, AP clients only).
    recovery_register_routes(server, &_config);

    // Wi-Fi setup portal (/wifi, /api/wifi/*) and the catch-all handler
    // (captive-portal redirect while the AP is up, JSON 404 otherwise).
    wifi_portal_register_routes(server);

    server.begin();
    LOGVLN("Web Server Started");
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
void ConnectivityManager::build_mqtt_status_topic() {
    // <base>/status where <base> is the subscribe topic without any wildcard tail.
    char base[sizeof(_config.mqtt_topic)];
    strlcpy(base, _config.mqtt_topic, sizeof(base));
    char* wc = strpbrk(base, "+#");
    if (wc) *wc = '\0';
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') base[--n] = '\0';
    if (n == 0) strlcpy(base, "tickr", sizeof(base));
    snprintf(_mqtt_status_topic, sizeof(_mqtt_status_topic), "%s/status", base);
}

void ConnectivityManager::setup_mqtt() {
    _mqtt_configured = strlen(_config.mqtt_server) > 0;
    _mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    _mqtt_next_attempt = 0;
    if (!_mqtt_configured) return;

    build_mqtt_status_topic();
    mqtt.setServer(_config.mqtt_server, (uint16_t)_config.mqtt_port);
    if (!mqtt.setBufferSize(MQTT_BUFFER_SIZE)) {
        Serial.println("MQTT: failed to allocate buffer");
    }
    mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
    mqtt.setCallback([this](char* topic, byte* payload, unsigned int length) {
        this->mqtt_callback(topic, payload, length);
    });
}

void ConnectivityManager::connect_mqtt() {
    LOGV("MQTT: connecting to %s:%d as %s...\n", _config.mqtt_server, _config.mqtt_port, _mqtt_client_id);
    const char* user = strlen(_config.mqtt_user) ? _config.mqtt_user : nullptr;
    const char* pass = (user && strlen(_config.mqtt_pass)) ? _config.mqtt_pass : nullptr;

    bool ok = mqtt.connect(_mqtt_client_id, user, pass, _mqtt_status_topic, 0, true, "offline");
    if (!ok) {
        Serial.printf("MQTT: connect failed, rc=%d\n", mqtt.state());
        char msg[64];
        snprintf(msg, sizeof(msg), "mqtt connect failed rc=%d", mqtt.state());
        setLastError(msg);
        return;
    }
    Serial.println("MQTT: connected");
    if (!mqtt.subscribe(_config.mqtt_topic)) {
        Serial.printf("MQTT: subscribe to '%s' failed\n", _config.mqtt_topic);
        setLastError("mqtt subscribe failed");
    }
    mqtt.publish(_mqtt_status_topic, "online", true);
}

void ConnectivityManager::mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (topic && strcmp(topic, _mqtt_status_topic) == 0) return;   // our own status message
    if (length == 0 || length > PAYLOAD_MAX_LEN) {
        Serial.printf("MQTT: ignoring message of %u bytes\n", length);
        setLastError("mqtt payload size out of range");
        return;
    }
    char err[96] = "";
    int code = enqueue_payload((const char*)payload, length, SRC_MQTT, err, sizeof(err));
    if (code != 202) {
        Serial.printf("MQTT: payload rejected (%d): %s\n", code, err);
    } else if (err[0]) {
        Serial.printf("MQTT: warning: %s\n", err);
    }
}

void ConnectivityManager::loop() {
    // Link supervisor: no action while an access point is up (the set-up
    // portal / recovery AP drive the STA themselves) or an image is being
    // written; the outage timers keep running.
    wifi_link_loop(wifi_portal_ap_up() || ota_update_in_progress() || ota_arduino_in_progress());
    run_source_test();   // a paused POST /api/source/test, if any (blocks like a scheduled pull)
    if (_mqtt_reconfigure) {
        _mqtt_reconfigure = false;
        if (mqtt.connected()) {
            mqtt.publish(_mqtt_status_topic, "offline", true);
            mqtt.disconnect();
        }
        setup_mqtt();
    }
    if (!_mqtt_configured) return;

    if (!mqtt.connected()) {
        unsigned long now = millis();
        if ((long)(now - _mqtt_next_attempt) >= 0) {
            connect_mqtt();
            if (mqtt.connected()) {
                _mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
            } else {
                _mqtt_backoff_ms = min(_mqtt_backoff_ms * 2, MQTT_BACKOFF_MAX_MS);
            }
            _mqtt_next_attempt = millis() + _mqtt_backoff_ms;
        }
        return;
    }
    mqtt.loop();
}

// ---------------------------------------------------------------------------
// HTTP pull (main task: the battery flow in setup(), the USB scheduler in
// loop()). Two pull targets (docs/TICKERS.md "What the Ticker source does"): the Pull URL - a body
// that already is a TickrDisplay payload - and the ticker source, whose JSON
// the extractor turns into one. Both share the bounded GET below.
// ---------------------------------------------------------------------------
// Transport for one GET. TLS_LEGACY = the Pull URL as before: the user's
// /ca.pem, or unverified (setInsecure, tls_insecure_used) without one.
// TLS_BUNDLE = the ticker presets: the baked-in roots (src/generated/ca_bundle.h,
// scripts/build_ca_bundle.py), never unverified. TLS_USER_OR_BUNDLE = a custom
// ticker URL: the user's /ca.pem when uploaded (a private proxy), else the bundle.
enum TlsMode : uint8_t { TLS_LEGACY = 0, TLS_BUNDLE, TLS_USER_OR_BUNDLE };

struct PullClient {
    WiFiClient       plain;
    WiFiClientSecure* secure = nullptr;   // only for https (the mbedTLS context is large)
    String           ca;                  // must outlive the request: WiFiClientSecure keeps the pointer
    WiFiClient*      client = &plain;
    ~PullClient() { delete secure; }
};

bool ConnectivityManager::open_client(PullClient& pc, bool https, uint8_t tls_mode) {
    if (!https) return true;
    pc.secure = new WiFiClientSecure();
    if (!pc.secure) {
        setLastError("pull: out of memory (tls)");
        return false;
    }
    if (tls_mode != TLS_BUNDLE) pc.ca = config_ca_load();
    if (pc.ca.length() > 0) {
        pc.secure->setCACert(pc.ca.c_str());
        _tls_insecure_used = false;
    } else if (tls_mode == TLS_LEGACY) {
        Serial.println("WARNING: HTTPS pull without root CA (/ca.pem) - certificate NOT verified!");
        pc.secure->setInsecure();
        _tls_insecure_used = true;
    } else {
        pc.secure->setCACertBundle(TICKR_CA_BUNDLE);
        _tls_insecure_used = false;
    }
    pc.client = pc.secure;
    return true;
}

// GET `url` into buf (PAYLOAD_MAX_LEN + 1 bytes); *got = body length, NUL-terminated.
// HTTP/1.0, 10 s timeouts, 3 redirects. Failures go to last_error as "pull: ...".
bool ConnectivityManager::http_get_body(WiFiClient& client, const char* url, char* buf, size_t* got) {
    HTTPClient http;
    http.setConnectTimeout(HTTP_PULL_TIMEOUT_MS);
    http.setTimeout(HTTP_PULL_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(3);
    http.setReuse(false);
    http.useHTTP10(true);   // no chunked encoding -> Content-Length or close-delimited body

    if (!http.begin(client, url)) {
        LOGVLN("Pull failed: invalid URL");
        setLastError("pull: invalid url");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        if (httpCode > 0) LOGV("Pull failed, HTTP %d\n", httpCode);
        else LOGV("Pull failed, error: %s\n", http.errorToString(httpCode).c_str());
        char msg[64];
        // a TLS failure surfaces as the connection error; name it (the Test button shows it)
        if (httpCode == HTTPC_ERROR_CONNECTION_REFUSED && strncasecmp(url, "https://", 8) == 0) snprintf(msg, sizeof(msg), "pull: connect/tls failed");
        else snprintf(msg, sizeof(msg), "pull: http %d", httpCode);
        setLastError(msg);
        http.end();
        return false;
    }

    int declared = http.getSize();   // -1 if unknown
    if (declared > (int)PAYLOAD_MAX_LEN) {
        LOGV("Pull failed: response too large (%d bytes)\n", declared);
        setLastError("pull: response too large");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t n = 0;
    bool too_large = false;
    unsigned long deadline = millis() + HTTP_PULL_TIMEOUT_MS;
    while (stream && (long)(deadline - millis()) > 0) {
        size_t avail = stream->available();
        if (avail > 0) {
            size_t room = PAYLOAD_MAX_LEN - n;
            if (room == 0) { too_large = true; break; }
            int r = stream->read((uint8_t*)buf + n, avail < room ? avail : room);
            if (r > 0) {
                n += (size_t)r;
                deadline = millis() + HTTP_PULL_TIMEOUT_MS;
            }
            if (declared > 0 && n >= (size_t)declared) break;
        } else if (!stream->connected()) {
            break;
        } else {
            delay(1);
        }
    }
    http.end();

    if (too_large) {
        LOGVLN("Pull failed: response exceeds 4096 bytes");
        setLastError("pull: response too large");
        return false;
    }
    if (declared > 0 && n != (size_t)declared) {
        LOGV("Pull failed: incomplete body (%u of %d bytes)\n", (unsigned)n, declared);
        setLastError("pull: incomplete body");
        return false;
    }
    if (n == 0) {
        LOGVLN("Pull failed: empty body");
        setLastError("pull: empty body");
        return false;
    }
    buf[n] = '\0';
    *got = n;
    return true;
}

bool ConnectivityManager::pullConfigured() const {
    return source_pull_kind(_config.source_kind, _config.pull_url[0] != '\0') != SRC_KIND_NONE;
}

bool ConnectivityManager::fetch_pull_data() {
    uint8_t kind = source_pull_kind(_config.source_kind, _config.pull_url[0] != '\0');
    if (kind == SRC_KIND_TICKER) return fetch_ticker();
    if (kind != SRC_KIND_URL) return false;

    LOGV("Pulling data from: %s\n", _config.pull_url);
    PullClient pc;
    if (!open_client(pc, strncasecmp(_config.pull_url, "https://", 8) == 0, TLS_LEGACY)) return false;
    char* buf = (char*)malloc(PAYLOAD_MAX_LEN + 1);
    if (!buf) {
        setLastError("pull: out of memory");
        return false;
    }
    size_t got = 0;
    bool ok = http_get_body(*pc.client, _config.pull_url, buf, &got);
    if (ok) {
        LOGV("Pull success (%u bytes)\n", (unsigned)got);
        // A "pull: ..." entry in status.last_error is over once a later pull
        // succeeds; errors of other sources (MQTT, a rejected push) stay.
        if (strncmp(_last_error, "pull:", 5) == 0) _last_error[0] = '\0';
        if (_onData) {
            String payload;
            payload.concat(buf, (unsigned)got);
            _onData(payload);
        }
    }
    free(buf);
    return ok;
}

// The sparkline history (logic/spark_hist.h): RTC slow memory, so a battery
// device keeps its points across deep sleep; a cold boot or a changed source
// URL (the key) starts it empty. USB devices simply keep it in the same place.
RTC_DATA_ATTR static SparkHist s_spark;

// One fetch of a resolved ticker source: GET + extract. `err` names the failure
// (also stored as last_error). `ms` = round-trip time. Used by the scheduled
// ticker fetch and by POST /api/source/test.
bool ConnectivityManager::fetch_source(const SourcePlan& plan, bool custom, SourceResult* res, char* err, size_t err_len, uint32_t* ms) {
    uint32_t t0 = millis();
    err[0] = '\0';
    PullClient pc;
    bool ok = false;
    if (open_client(pc, plan.https, custom ? TLS_USER_OR_BUNDLE : TLS_BUNDLE)) {
        char* buf = (char*)malloc(PAYLOAD_MAX_LEN + 1);
        if (!buf) {
            setLastError("pull: out of memory");
        } else {
            size_t got = 0;
            if (http_get_body(*pc.client, plan.url, buf, &got)) {
                SourceError e = source_extract(buf, got, plan, res);
                if (e == SRC_OK) ok = true;
                else {
                    snprintf(err, err_len, "pull: %s", source_error_str(e));
                    setLastError(err);
                }
            }
            free(buf);
        }
    }
    if (!ok && !err[0]) strlcpy(err, _last_error, err_len);
    if (ms) *ms = millis() - t0;
    return ok;
}

bool ConnectivityManager::fetch_ticker() {
    SourcePlan plan;
    if (!source_resolve(_config.ticker, &plan)) {
        setLastError("pull: ticker not configured");
        return false;
    }
    SourceResult* res = (SourceResult*)malloc(sizeof(SourceResult));
    ScreenPayload* p = (ScreenPayload*)malloc(sizeof(ScreenPayload));   // ~1.3 KB: off the loop task's stack
    if (!res || !p) {
        free(res);
        free(p);
        setLastError("pull: out of memory");
        return false;
    }
    char err[SRC_ERR_MAX + 8];
    uint32_t ms = 0;
    bool ok = fetch_source(plan, _config.ticker.preset == SRC_PRESET_CUSTOM, res, err, sizeof(err), &ms);
    if (ok) {
        if (strncmp(_last_error, "pull:", 5) == 0) _last_error[0] = '\0';
        // The same payload a proxy would send (docs/API.md "Payload format"): the
        // ticker renderer draws it, the LED rule applies, no second parser.
        // cppcheck-suppress memsetClassFloat ; all-zero bytes are 0.0f
        memset(p, 0, sizeof(*p));
        p->has_text = true;
        strlcpy(p->title, plan.label, sizeof(p->title));
        strlcpy(p->value, res->price, sizeof(p->value));
        if (res->has_change) {
            strlcpy(p->change, res->change, sizeof(p->change));
            p->has_change = true;
            p->has_dir = true;
            p->dir = res->dir;
        }
        p->has_age = true;   // age_s = 0: the quote is as old as the frame
        // Sparkline: the source's own series (custom path) when it has one,
        // else the device history - min-max scaled by the ticker drawing code.
        spark_hist_push(&s_spark, spark_hist_key(plan.url), res->price_f);
        if (res->spark_n >= 2) {
            memcpy(p->spark, res->spark, res->spark_n * sizeof(float));
            p->spark_n = res->spark_n;
        } else {
            p->spark_n = spark_hist_values(&s_spark, p->spark, TICKER_SPARK_MAX);
        }
        renderer_apply(*p);
    } else {
        Serial.printf("Ticker: failed - %s (%lu ms)\n", err, (unsigned long)ms);
    }
    free(p);
    free(res);
    return ok;
}

// ---------------------------------------------------------------------------
// POST /api/source/test - one fetch with the posted (unsaved) source, the
// editor's Test button (docs/TICKERS.md "Setting it up in the web UI"). Form parameters as POST /config
// (tk_preset, tk_symbol, tk_market, tk_url, tk_price, tk_change, tk_spark,
// tk_mode, tk_decimals, tk_sep, tk_label). The handler validates and pauses
// the request; the fetch runs on the main task (run_source_test() from
// loop()) because the async_tcp task must not block for a 10 s TLS round
// trip, and the paused request is answered from there:
//   {"ok":true,"price":"84 000.06","change":"+0.07%","dir":1,"label":"...","url":"...","ms":812}
//   {"ok":false,"error":"http 451","url":"...","ms":1830}
// Nothing is drawn, nothing is saved, the sparkline history is not touched.
// 409 while another test runs; the answer is dropped when the browser has
// gone away meanwhile.
// ---------------------------------------------------------------------------
void ConnectivityManager::handle_source_test(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    if (_test_pending) {
        send_json_error(request, 409, "a source test is already running");
        return;
    }
    SourceSpec s;
    source_spec_defaults(&s);
    String err;
    parse_source_params(request, nullptr, s, err);
    if (err.length() > 0) {
        send_json_error(request, 400, err.c_str());
        return;
    }
    source_resolve(s, &_test_plan);   // validated by parse_source_params
    _test_custom = s.preset == SRC_PRESET_CUSTOM;
    _test_req = request->pause();
    _test_pending = true;
}

void ConnectivityManager::run_source_test() {
    if (!_test_pending) return;
    _test_pending = false;
    SourceResult* res = (SourceResult*)malloc(sizeof(SourceResult));
    if (!res) return;
    char err[SRC_ERR_MAX + 8] = "no wifi";
    uint32_t ms = 0;
    bool ok = WiFi.isConnected() && fetch_source(_test_plan, _test_custom, res, err, sizeof(err), &ms);
    // strings are quote-free by validation (label, url) or by construction (price, change, error)
    char body[SRC_URL_MAX + SRC_LABEL_MAX + SRC_PRICE_MAX + TICKER_CHANGE_MAX + 96];
    if (ok) {
        snprintf(body, sizeof(body), "{\"ok\":true,\"price\":\"%s\",\"change\":\"%s\",\"dir\":%d,\"label\":\"%s\",\"url\":\"%s\",\"ms\":%lu}",
                 res->price, res->change, (int)res->dir, _test_plan.label, _test_plan.url, (unsigned long)ms);
    } else {
        // the test's own failure must not linger as the device's last_error
        if (strncmp(_last_error, "pull:", 5) == 0) _last_error[0] = '\0';
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"url\":\"%s\",\"ms\":%lu}",
                 strncmp(err, "pull: ", 6) == 0 ? err + 6 : err, _test_plan.url, (unsigned long)ms);
    }
    free(res);
    if (auto req = _test_req.lock()) req->send(200, JSON_CT, body);
    else Serial.println("Source test: client gone");
    _test_req.reset();
}

// For /api/screen/state (additive): the configured pull target and, for
// the ticker, its symbol - "ticker" | "url" | "none".
const char* ConnectivityManager::pullSourceStr() const {
    return source_kind_str(source_pull_kind(_config.source_kind, _config.pull_url[0] != '\0'));
}

const char* ConnectivityManager::tickerSymbol() const {
    return source_pull_kind(_config.source_kind, _config.pull_url[0] != '\0') == SRC_KIND_TICKER ? _config.ticker.symbol : "";
}
