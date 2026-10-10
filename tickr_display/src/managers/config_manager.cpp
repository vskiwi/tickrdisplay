#include "config_manager.h"
#include "../logic/battery.h"   // cell divider default + validity
#include "../logic/ticker.h"    // led_rule "off" | "sign"

static const char* CONFIG_FILE     = "/config.json";
static const char* CONFIG_FILE_TMP = "/config.json.tmp";
static const size_t CONFIG_FILE_MAX = 6144;   // config.json is ~1 KB, ~2 KB with three grid rows; the parse buffer is a heap transient
static const size_t CONFIG_DOC_SIZE = 4096;   // ArduinoJson document for load / save (heap transient; a read copies every string)
static const char* CA_FILE         = "/ca.pem";
static const size_t CA_MAX_LEN     = 8192;

static bool s_fs_ok = false;

const char* config_power_source_str(uint8_t v) {
    return v == 1 ? "usb" : v == 2 ? "battery" : "auto";
}

uint8_t config_power_source_parse(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "usb") == 0) return 1;
    if (strcmp(s, "battery") == 0) return 2;
    return 0;
}

// One ticker source <-> its tk_* keys: the flat keys of the file's root for
// row 0, the same keys inside a `tickers[]` object for the grid rows.
static void spec_load(JsonObjectConst o, SourceSpec& t) {
    t.preset = source_preset_parse(o["tk_preset"] | "coingecko");
    strlcpy(t.symbol, o["tk_symbol"] | "", sizeof(t.symbol));
    strlcpy(t.market, o["tk_market"] | "", sizeof(t.market));
    strlcpy(t.url, o["tk_url"] | "", sizeof(t.url));
    strlcpy(t.path_price, o["tk_price"] | "", sizeof(t.path_price));
    strlcpy(t.path_change, o["tk_change"] | "", sizeof(t.path_change));
    strlcpy(t.path_spark, o["tk_spark"] | "", sizeof(t.path_spark));
    t.change_mode = source_change_mode_parse(o["tk_mode"] | "pct");
    int dec = o["tk_decimals"] | (int)SRC_DECIMALS_AUTO;
    t.decimals = (dec < 0 || dec > SRC_DECIMALS_MAX) ? SRC_DECIMALS_AUTO : (uint8_t)dec;
    t.sep = source_sep_parse(o["tk_sep"] | "space");
    strlcpy(t.label, o["tk_label"] | "", sizeof(t.label));
    strlcpy(t.short_label, o["tk_short"] | "", sizeof(t.short_label));   // absent in older files = auto
}

static void spec_save(JsonObject o, const SourceSpec& t) {
    o["tk_preset"] = source_preset_str(t.preset);
    o["tk_symbol"] = t.symbol;
    o["tk_market"] = t.market;
    o["tk_url"] = t.url;
    o["tk_price"] = t.path_price;
    o["tk_change"] = t.path_change;
    o["tk_spark"] = t.path_spark;
    o["tk_mode"] = source_change_mode_str(t.change_mode);
    o["tk_decimals"] = t.decimals;
    o["tk_sep"] = source_sep_str(t.sep);
    o["tk_label"] = t.label;
    o["tk_short"] = t.short_label;
}

void config_init() {
    s_fs_ok = LittleFS.begin(true);
    if (!s_fs_ok) {
        Serial.println("LittleFS Mount Failed");
        return;
    }
    // Leftover from an interrupted save: the real file is still intact, drop the temp.
    if (LittleFS.exists(CONFIG_FILE_TMP)) {
        LittleFS.remove(CONFIG_FILE_TMP);
    }
}

bool config_load(AppConfig& config) {
    config = AppConfig();   // start from defaults
    if (!s_fs_ok) return false;

    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("Config file not found, using defaults");
        return false;
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
        Serial.println("Failed to open config file, using defaults");
        return false;
    }

    // Read into a buffer and parse it bounded: one ArduinoJson reader
    // instantiation for the whole image (flash budget, docs/DEVELOPMENT.md "Size gate").
    size_t size = file.size();
    char* buf = size > 0 && size <= CONFIG_FILE_MAX ? (char*)malloc(size) : nullptr;
    size_t got = buf ? file.read((uint8_t*)buf, size) : 0;
    file.close();
    DynamicJsonDocument doc(CONFIG_DOC_SIZE);
    DeserializationError error = got == size && buf ? deserializeJson(doc, (const char*)buf, size) : DeserializationError::NoMemory;
    free(buf);
    if (error) {
        Serial.printf("Config file corrupt (%s), using defaults\n", error.c_str());
        return false;
    }

    int schema = doc["schema_version"] | 1;
    if (schema > CONFIG_SCHEMA_VERSION) {
        Serial.printf("Config schema %d is newer than supported %d, reading known fields only\n",
                      schema, CONFIG_SCHEMA_VERSION);
    }

    strlcpy(config.mqtt_server, doc["mqtt_server"] | "", sizeof(config.mqtt_server));
    config.mqtt_port = doc["mqtt_port"] | 1883;
    strlcpy(config.mqtt_topic, doc["mqtt_topic"] | "tickr/display", sizeof(config.mqtt_topic));
    strlcpy(config.mqtt_user, doc["mqtt_user"] | "", sizeof(config.mqtt_user));
    strlcpy(config.mqtt_pass, doc["mqtt_pass"] | "", sizeof(config.mqtt_pass));
    strlcpy(config.pull_url, doc["pull_url"] | "", sizeof(config.pull_url));
    config.refresh_interval_min = doc["refresh_interval_min"] | 60;
    strlcpy(config.api_token, doc["api_token"] | "", sizeof(config.api_token));
    strlcpy(config.ota_password, doc["ota_password"] | "", sizeof(config.ota_password));
    // schema 4 (absent in older files -> defaults)
    strlcpy(config.device_name, doc["device_name"] | "", sizeof(config.device_name));
    int lx = doc["layout_x"] | 0;
    int ly = doc["layout_y"] | 0;
    config.layout_x = (uint8_t)((lx < 0 || lx > CONFIG_LAYOUT_MAX) ? 0 : lx);
    config.layout_y = (uint8_t)((ly < 0 || ly > CONFIG_LAYOUT_MAX) ? 0 : ly);
    strlcpy(config.layout_group, doc["layout_group"] | "", sizeof(config.layout_group));
    strlcpy(config.group_id, doc["group_id"] | "", sizeof(config.group_id));
    strlcpy(config.group_secret, doc["group_secret"] | "", sizeof(config.group_secret));
    strlcpy(config.group_name, doc["group_name"] | "", sizeof(config.group_name));
    config.group_epoch = (uint16_t)(doc["group_epoch"] | 0);
    // schema 5
    config.power_source = config_power_source_parse(doc["power_source"] | "auto");
    // schema 6
    int cn = doc["adc_cell_num"] | (int)CELL_DIVIDER_NUM_DEFAULT;
    int cd = doc["adc_cell_den"] | (int)CELL_DIVIDER_DEN_DEFAULT;
    // schema 7
    config.led_rule = led_rule_parse(doc["led_rule"] | "off");
    // schema 8: the content source. Older files: pull_url set -> url, else none.
    if (doc.containsKey("source_kind")) config.source_kind = source_kind_parse(doc["source_kind"] | "none");
    else config.source_kind = source_pull_kind(SRC_KIND_NONE, config.pull_url[0] != '\0');
    spec_load(doc.as<JsonObjectConst>(), config.ticker);
    strlcpy(config.tk_api_key, doc["tk_api_key"] | "", sizeof(config.tk_api_key));
    // schema 9: the grid. Older files have neither key -> single, one source.
    config.tk_view = source_view_parse(doc["tk_view"] | "single");
    config.tk_n = source_rows_clamp(doc["tk_n"] | 1);
    JsonArrayConst rows = doc["tickers"].as<JsonArrayConst>();
    uint8_t i = 0;
    for (JsonObjectConst o : rows) {
        if (i >= SRC_ROWS_MAX - 1) break;
        spec_load(o, config.tickers_more[i++]);
    }

    // Sanitise values that may come from an older / hand-edited file.
    if (cn < 0 || cn > 65535 || cd < 0 || cd > 65535 || !cell_divider_valid((uint16_t)cn, (uint16_t)cd)) {
        cn = CELL_DIVIDER_NUM_DEFAULT;
        cd = CELL_DIVIDER_DEN_DEFAULT;
    }
    config.adc_cell_num = (uint16_t)cn;
    config.adc_cell_den = (uint16_t)cd;
    if (config.mqtt_port < 1 || config.mqtt_port > 65535) config.mqtt_port = 1883;
    if (config.refresh_interval_min < CONFIG_REFRESH_MIN_MINUTES ||
        config.refresh_interval_min > CONFIG_REFRESH_MAX_MINUTES) {
        config.refresh_interval_min = 60;
    }
    return true;
}

bool config_save(const AppConfig& config) {
    if (!s_fs_ok) return false;

    File file = LittleFS.open(CONFIG_FILE_TMP, "w");
    if (!file) {
        Serial.println("Failed to open temp config file for writing");
        return false;
    }

    DynamicJsonDocument doc(CONFIG_DOC_SIZE);
    doc["schema_version"] = CONFIG_SCHEMA_VERSION;
    doc["mqtt_server"] = config.mqtt_server;
    doc["mqtt_port"] = config.mqtt_port;
    doc["mqtt_topic"] = config.mqtt_topic;
    doc["mqtt_user"] = config.mqtt_user;
    doc["mqtt_pass"] = config.mqtt_pass;
    doc["pull_url"] = config.pull_url;
    doc["refresh_interval_min"] = config.refresh_interval_min;
    doc["api_token"] = config.api_token;
    doc["ota_password"] = config.ota_password;
    doc["device_name"] = config.device_name;
    doc["layout_x"] = config.layout_x;
    doc["layout_y"] = config.layout_y;
    doc["layout_group"] = config.layout_group;
    doc["group_id"] = config.group_id;
    doc["group_secret"] = config.group_secret;
    doc["group_name"] = config.group_name;
    doc["group_epoch"] = config.group_epoch;
    doc["power_source"] = config_power_source_str(config.power_source);
    doc["adc_cell_num"] = config.adc_cell_num;
    doc["adc_cell_den"] = config.adc_cell_den;
    doc["led_rule"] = led_rule_str(config.led_rule);
    doc["source_kind"] = source_kind_str(config.source_kind);
    spec_save(doc.as<JsonObject>(), config.ticker);
    doc["tk_api_key"] = config.tk_api_key;
    // schema 9: the grid rows 1..tk_n-1 (rows beyond tk_n are not kept)
    doc["tk_view"] = source_view_str(config.tk_view);
    doc["tk_n"] = config.tk_n;
    JsonArray rows = doc.createNestedArray("tickers");
    for (uint8_t i = 1; i < config.tk_n && i < SRC_ROWS_MAX; i++) spec_save(rows.createNestedObject(), config.tickers_more[i - 1]);

    String out;
    size_t written = serializeJson(doc, out);   // Writer<String>: shared with every other JSON response
    if (file.print(out) != written) written = 0;
    file.flush();
    file.close();
    if (written == 0) {
        Serial.println("Failed to write config");
        LittleFS.remove(CONFIG_FILE_TMP);
        return false;
    }

    // rename() replaces the destination atomically on LittleFS.
    if (!LittleFS.rename(CONFIG_FILE_TMP, CONFIG_FILE)) {
        // Some FS implementations refuse to overwrite: remove and retry once.
        LittleFS.remove(CONFIG_FILE);
        if (!LittleFS.rename(CONFIG_FILE_TMP, CONFIG_FILE)) {
            Serial.println("Failed to rename temp config file");
            LittleFS.remove(CONFIG_FILE_TMP);
            return false;
        }
    }
    return true;
}

bool config_ca_exists() {
    return s_fs_ok && LittleFS.exists(CA_FILE);
}

String config_ca_load() {
    String pem;
    if (!config_ca_exists()) return pem;
    File f = LittleFS.open(CA_FILE, "r");
    if (!f) return pem;
    size_t size = f.size();
    if (size == 0 || size > CA_MAX_LEN) {
        f.close();
        return pem;
    }
    pem.reserve(size + 1);
    while (f.available()) {
        char buf[128];
        int n = f.readBytes(buf, sizeof(buf));
        if (n <= 0) break;
        pem.concat(buf, (unsigned)n);
    }
    f.close();
    return pem;
}

bool config_ca_save(const char* pem) {
    if (!s_fs_ok || !pem) return false;
    size_t len = strlen(pem);
    if (len < 64 || len > CA_MAX_LEN) return false;
    if (strstr(pem, "-----BEGIN CERTIFICATE-----") == nullptr ||
        strstr(pem, "-----END CERTIFICATE-----") == nullptr) {
        return false;
    }
    File f = LittleFS.open(CA_FILE, "w");
    if (!f) return false;
    size_t written = f.write((const uint8_t*)pem, len);
    f.close();
    if (written != len) {
        LittleFS.remove(CA_FILE);
        return false;
    }
    return true;
}

bool config_ca_remove() {
    if (!config_ca_exists()) return true;
    return LittleFS.remove(CA_FILE);
}
