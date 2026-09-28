#include "ota_manager.h"
#include "../log.h"
#include "arduino_ota.h"
#include "web_auth.h"
#include "peer_manager.h"
#include "../hal/hal_indication.h"
#include "../hal/hal_power.h"
#include "../logic/relay.h"   // relay_url_from_json / RELAY_URL_MAX

#include <Arduino.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include <Update.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <esp_system.h>
#include <esp_app_format.h>

// ---------------------------------------------------------------------------------------------
// Constants / state
// ---------------------------------------------------------------------------------------------

static const char CT_JSON[] = "application/json";
static const char CT_OCTET[] = "application/octet-stream";
static const char PARTITION_ROUTE_PREFIX[] = "/api/system/partition/";
static const uint8_t IMAGE_MAGIC = ESP_IMAGE_HEADER_MAGIC; // 0xE9

// Deferred restart handled in ota_loop() (main task) - never restart from the async_tcp task.
static volatile bool s_restart_pending = false;
static volatile uint32_t s_restart_at_ms = 0;

// Upload state. Only one upload can be in flight at a time.
static bool s_upload_active = false;
static bool s_upload_ok = false;
static size_t s_upload_written = 0;
static uint32_t s_upload_last_log = 0;
static String s_upload_error;
static String s_upload_target;
static AsyncWebServerRequest* s_upload_request = nullptr;

// Running image size and MD5, computed once on the main task in ota_loop()
// (see handle_system_info). Never computed from a request handler: the
// image verification behind ESP.getSketchSize() is not safe to run on two
// tasks at once, and ESP.getSketchMD5() caches its first result forever - a
// concurrent first call used to leave the MD5 of an empty buffer
// (d41d8cd9...) in that cache for the rest of the uptime.
static uint32_t s_sketch_size = 0;
static char s_sketch_md5[33] = "";

static void schedule_restart(uint32_t delay_ms) {
    s_restart_at_ms = millis() + delay_ms;
    s_restart_pending = true;
}

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

static const char* partition_type_str(esp_partition_type_t t) {
    switch (t) {
        case ESP_PARTITION_TYPE_APP:  return "app";
        case ESP_PARTITION_TYPE_DATA: return "data";
        default:                      return "other";
    }
}

static String partition_subtype_str(esp_partition_type_t t, esp_partition_subtype_t st) {
    if (t == ESP_PARTITION_TYPE_APP) {
        if (st == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
        if (st == ESP_PARTITION_SUBTYPE_APP_TEST) return "test";
        if (st >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && st < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            return String("ota_") + String((int)(st - ESP_PARTITION_SUBTYPE_APP_OTA_MIN));
        }
    } else if (t == ESP_PARTITION_TYPE_DATA) {
        switch (st) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:       return "ota";
            case ESP_PARTITION_SUBTYPE_DATA_PHY:       return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:       return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:  return "coredump";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:  return "nvs_keys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:  return "efuse";
            case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED: return "undefined";
            case ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD:  return "esphttpd";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:       return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:    return "spiffs";
            default: break;
        }
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02x", (unsigned)st);
    return String(buf);
}

static const char* ota_state_str(esp_ota_img_states_t s) {
    switch (s) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
        default:                         return "unknown";
    }
}

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

static bool is_bootable_app_subtype(esp_partition_subtype_t st) {
    return st == ESP_PARTITION_SUBTYPE_APP_FACTORY ||
           (st >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && st < ESP_PARTITION_SUBTYPE_APP_OTA_MAX);
}

// First byte of an app partition must be the ESP image magic (0xE9).
static bool partition_has_image_magic(const esp_partition_t* p) {
    uint8_t first = 0;
    if (esp_partition_read(p, 0, &first, 1) != ESP_OK) return false;
    return first == IMAGE_MAGIC;
}

// Find any partition (any type) by label. Returns nullptr if not found.
static const esp_partition_t* find_partition_by_label(const char* label) {
    if (!label || !*label) return nullptr;
    return esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
}

static void send_json(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
    String body;                              // Writer<String>: one serializer instantiation image-wide
    serializeJson(doc, body);
    request->send(code, CT_JSON, body);
}

static void send_error(AsyncWebServerRequest* request, int code, const String& error) {
    StaticJsonDocument<256> doc;
    doc["ok"] = false;
    doc["error"] = error;
    send_json(request, code, doc);
}

// ---------------------------------------------------------------------------------------------
// GET /api/system/info
// ---------------------------------------------------------------------------------------------

static void fill_partition_json(JsonObject o, const esp_partition_t* p,
                                const esp_partition_t* running, const esp_partition_t* boot) {
    // Trimmed for flash (docs/DEVELOPMENT.md "Size gate"): type_id, subtype_id, encrypted and the
    // description's secure_version/time were dropped - nothing read them.
    o["label"] = p->label;
    o["type"] = partition_type_str(p->type);
    o["subtype_str"] = partition_subtype_str(p->type, p->subtype);
    o["address"] = p->address;
    o["size"] = p->size;

    if (p->type != ESP_PARTITION_TYPE_APP) return;

    bool same_running = running && running->address == p->address;
    bool same_boot = boot && boot->address == p->address;
    o["running"] = same_running;
    o["boot"] = same_boot;
    o["bootable"] = partition_has_image_magic(p);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(p, &state) == ESP_OK) {
        o["ota_state"] = ota_state_str(state);
    } else {
        o["ota_state"] = nullptr; // not present in otadata
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
        JsonObject d = o.createNestedObject("description");
        d["project_name"] = desc.project_name;
        d["version"] = desc.version;
        d["idf_ver"] = desc.idf_ver;
        d["date"] = desc.date;
    } else {
        o["description"] = nullptr;
    }
}

// GET /api/system/info - public: versions, slots, MD5, chip - no secrets.
// The raw partition / flash downloads below stay token-protected.
static void handle_system_info(AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(5120);

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

    JsonObject fw = doc.createNestedObject("firmware");
    fw["name"] = TICKR_FW_NAME;
    fw["version"] = TICKR_FW_VERSION;
    fw["build"] = __DATE__ " " __TIME__;
    fw["sdk"] = ESP.getSdkVersion();
    // Both values are computed once on the main task (ota_loop): ESP.getSketchSize() runs a full
    // esp_image_verify() (SHA-256 over the whole image) and must not run on the async_tcp task.
    // Until the first loop() iteration they read 0 / "" (a few hundred ms after boot).
    fw["sketch_size"] = s_sketch_size;
    fw["sketch_md5"] = s_sketch_md5;
    // free_sketch_space and app_desc (= the running partition's description) are not reported.

    JsonObject chip = doc.createNestedObject("chip");
    chip["model"] = ESP.getChipModel();
    chip["revision"] = ESP.getChipRevision();
    chip["cores"] = ESP.getChipCores();
    chip["mac"] = WiFi.macAddress();

    // Board revision profile (docs/HARDWARE.md "Power sensing"): "A" = sense
    // network always on; "B" = switched by GPIO 4 (driven high while awake);
    // both measure the cell on GPIO 32. "?" = neither - nothing measurable.
    JsonObject board = doc.createNestedObject("board");
    board["revision"] = power_board_str();
    board["battery_measurable"] = power_battery_measurable();

    JsonObject flash = doc.createNestedObject("flash");
    // chip_size = size the *bootloader* was configured for (Arduino reads it from the bootloader
    // image header at 0x1000). On a device still running the stock bootloader this says 4 MB even
    // when the physical chip is 8 MB; esp_flash_read() is bounded by this value as well.
    flash["chip_size"] = ESP.getFlashChipSize();
    // Physical size from the JEDEC ID (last byte = log2 of capacity), so the two can be compared.
    uint32_t jedec = 0;
    if (esp_flash_read_id(nullptr, &jedec) == ESP_OK) {
        uint8_t cap = jedec & 0xFF;
        char idbuf[12];
        snprintf(idbuf, sizeof(idbuf), "0x%06x", (unsigned)jedec);
        flash["jedec_id"] = idbuf;
        if (cap >= 0x10 && cap <= 0x1A) flash["chip_size_jedec"] = (uint32_t)1 << cap;
        else flash["chip_size_jedec"] = nullptr;
    }
    // Flash size the running image was built for (from the image header, not the chip).
    esp_image_header_t hdr;
    if (running && esp_partition_read(running, 0, &hdr, sizeof(hdr)) == ESP_OK && hdr.magic == IMAGE_MAGIC) {
        flash["image_header_size"] = (uint32_t)(1UL << 20) << hdr.spi_size;
    } else {
        flash["image_header_size"] = nullptr;
    }

    JsonObject mem = doc.createNestedObject("memory");
    mem["free_heap"] = ESP.getFreeHeap();
    mem["min_free_heap"] = ESP.getMinFreeHeap();
    mem["max_alloc_heap"] = ESP.getMaxAllocHeap();

    doc["reset_reason"] = reset_reason_str(esp_reset_reason());
    doc["uptime_ms"] = millis();
    doc["update_in_progress"] = s_upload_active || ota_arduino_in_progress();
#ifdef TICKR_DEV_PAGE
    doc["dev_page"] = true;    // /dev is served (tickr_dev); /system shows its links only then
#else
    doc["dev_page"] = false;
#endif

    JsonObject aota = doc.createNestedObject("arduino_ota");
    aota["built"] = ota_arduino_built();     // false in the release image (TICKR_ARDUINO_OTA off)
    aota["enabled"] = ota_arduino_enabled();
    aota["hostname"] = ota_arduino_hostname();
    aota["port"] = ota_arduino_port();

    JsonObject wifi = doc.createNestedObject("wifi");
    wifi["ip"] = WiFi.localIP().toString();
    wifi["rssi"] = WiFi.RSSI();
    wifi["hostname"] = WiFi.getHostname();

    auto part_summary = [&doc](const char* key, const esp_partition_t* p) {
        if (!p) { doc[key] = nullptr; return; }
        JsonObject o = doc.createNestedObject(key);
        o["label"] = p->label;
        o["address"] = p->address;
        o["size"] = p->size;
    };
    part_summary("running", running);
    part_summary("boot", boot);
    part_summary("next_update", next);

    JsonArray parts = doc.createNestedArray("partitions");
    // esp_partition_next() frees the iterator itself when it runs off the end and returns NULL
    // (IDF partition.c), so the original handle must never be touched afterwards. Same idiom as
    // log_flash_info() in main.cpp: iterate until NULL, then release(NULL) is a harmless no-op.
    // (Releasing the original handle here was a double free -> heap assert -> panic on every
    // /api/system/info call, i.e. every time the /update page was opened.)
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
        fill_partition_json(parts.createNestedObject(), esp_partition_get(it), running, boot);
    }
    esp_partition_iterator_release(it);

    if (doc.overflowed()) {
        Serial.println("[OTA] system/info JSON document overflowed");
    }
    send_json(request, 200, doc);
}

// ---------------------------------------------------------------------------------------------
// POST /update  - firmware upload (multipart, field "update")
// ---------------------------------------------------------------------------------------------

static void upload_fail(const String& why) {
    if (s_upload_error.isEmpty()) s_upload_error = why;
    Serial.printf("[OTA] Update failed: %s\n", why.c_str());
    if (Update.isRunning()) Update.abort();
    s_upload_active = false;
    s_upload_ok = false;
    indication_overlay(LED_OVL_OTA, false);
}

static void handle_update_upload(AsyncWebServerRequest* request, const String& filename,
                                 size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        // Unauthenticated clients never touch flash; handle_update_request() answers 401.
        if (!web_auth_ok(request)) return;
        if (s_upload_active && s_upload_request != request) {
            // Another client is already flashing - refuse silently, the request handler reports it.
            Serial.println("[OTA] Rejecting concurrent upload");
            return;
        }
        s_upload_active = true;
        s_upload_ok = false;
        s_upload_written = 0;
        s_upload_last_log = 0;
        s_upload_error = "";
        s_upload_target = "";
        s_upload_request = request;

        Serial.printf("[OTA] Upload start: '%s', content-length %u\n", filename.c_str(),
                      (unsigned)request->contentLength());

        if (ota_arduino_in_progress()) {
            upload_fail("a LAN push-OTA (ArduinoOTA) transfer is in progress");
            return;
        }
        if (len == 0 || data[0] != IMAGE_MAGIC) {
            upload_fail("not an ESP32 application image (first byte must be 0xE9)");
            return;
        }
        const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
        if (!target) {
            upload_fail("no OTA partition available in the partition table");
            return;
        }
        // The multipart body is only slightly larger than the file itself, so this rejects
        // obviously oversized images before we start erasing flash.
        if (request->contentLength() > target->size + 4096) {
            upload_fail(String("image too large for OTA slot ") + target->label + " (" +
                        String(target->size) + " bytes)");
            return;
        }
        s_upload_target = target->label;
        LOGV("[OTA] Target partition: %s @ 0x%06x (%u bytes)\n", target->label,
                      (unsigned)target->address, (unsigned)target->size);

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            upload_fail(Update.errorString());
            return;
        }
        // LED blue while flashing (docs/DEVICE_UI.md "LED and sound"); the OTA card is drawn
        // by the main loop's state machine from ota_update_in_progress().
        indication_overlay(LED_OVL_OTA, true);

        // Abort a half-written update if the client goes away mid-transfer.
        request->onDisconnect([request]() {
            if (s_upload_request != request) return;
            if (s_upload_active) upload_fail("client disconnected during upload");
            s_upload_request = nullptr; // never keep a pointer to a destroyed request
        });
    }

    if (!s_upload_active || s_upload_request != request) return;

    if (len) {
        if (Update.write(data, len) != len) {
            upload_fail(Update.errorString());
            return;
        }
        s_upload_written += len;
        if (s_upload_written - s_upload_last_log >= 131072) {
            s_upload_last_log = s_upload_written;
            LOGV("[OTA] %u bytes written\n", (unsigned)s_upload_written);
        }
    }

    if (final) {
        if (Update.end(true)) {
            s_upload_ok = true;
            s_upload_active = false;
            Serial.printf("[OTA] Update successful: %u bytes -> %s\n", (unsigned)s_upload_written,
                          s_upload_target.c_str());
            indication_overlay(LED_OVL_OTA, false);
            indication_flash(0, 255, 0, 3000);   // short green until the deferred restart
        } else {
            upload_fail(Update.errorString());
        }
    }
}

static void handle_update_request(AsyncWebServerRequest* request) {
    // Called after the whole body (and thus the upload) has been processed.
    REQUIRE_AUTH(request);
    if (s_upload_request != request) {
        send_error(request, 409, s_upload_active ? "another update is in progress" : "no firmware file received (multipart field 'update')");
        return;
    }
    s_upload_request = nullptr;

    if (s_upload_ok) {
        StaticJsonDocument<256> doc;
        doc["ok"] = true;
        doc["target"] = s_upload_target;
        doc["written"] = s_upload_written;
        doc["message"] = "Update successful, rebooting";
        send_json(request, 200, doc);
        schedule_restart(1500);
    } else {
        String err = s_upload_error.isEmpty() ? String("upload incomplete") : s_upload_error;
        send_error(request, 500, err);
        indication_overlay(LED_OVL_OTA, false);
    }
}

// ---------------------------------------------------------------------------------------------
// POST /api/system/boot_partition   {"label":"app0"}  (or ?label=app0 / form field)
// ---------------------------------------------------------------------------------------------

static void collect_small_body(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                               size_t index, size_t total) {
    const size_t kMaxBody = 512;
    if (!web_auth_ok(request)) return;   // request handler answers 401
    if (total == 0 || total > kMaxBody) return;
    if (index == 0) {
        request->_tempObject = calloc(total + 1, 1); // freed by ~AsyncWebServerRequest
    }
    if (request->_tempObject && index + len <= total) {
        memcpy((uint8_t*)request->_tempObject + index, data, len);
    }
}

static String label_from_json(const char* json, bool* restart) {
    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, json, strlen(json)) != DeserializationError::Ok) return String();
    *restart = doc["restart"] | true;
    return String(doc["label"] | "");
}

static String extract_label(AsyncWebServerRequest* request, bool* restart) {
    *restart = true;
    if (request->hasParam("label", true)) return request->getParam("label", true)->value();
    if (request->hasParam("label")) return request->getParam("label")->value();
    // JSON body sent with Content-Type: application/json
    if (request->_tempObject) return label_from_json((const char*)request->_tempObject, restart);
    // JSON body sent by `curl -d '{...}'` without a Content-Type header: the server treats it as
    // form-urlencoded and the whole JSON text ends up as a parameter *name*.
    for (size_t i = 0; i < request->params(); i++) {
        const AsyncWebParameter* p = request->getParam(i);
        if (p->isPost() && p->name().startsWith("{")) return label_from_json(p->name().c_str(), restart);
    }
    return String();
}

static void handle_boot_partition(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    bool restart = true;
    String label = extract_label(request, &restart);
    if (label.isEmpty()) {
        send_error(request, 400, "missing 'label' (JSON body {\"label\":\"app0\"} or ?label=)");
        return;
    }
    if (s_upload_active) {
        send_error(request, 409, "firmware update in progress");
        return;
    }

    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
    if (!part) {
        send_error(request, 404, "no app partition with label '" + label + "'");
        return;
    }
    if (!is_bootable_app_subtype(part->subtype)) {
        send_error(request, 400, "partition '" + label + "' is not a factory/OTA app partition");
        return;
    }
    if (!partition_has_image_magic(part)) {
        send_error(request, 400, "partition '" + label + "' does not contain an ESP32 image (no 0xE9 magic)");
        return;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        send_error(request, 400, "partition '" + label + "' has no valid application description");
        return;
    }

    // NOTE: esp_ota_set_boot_partition() also runs esp_image_verify() on the target, so a corrupt
    // image is rejected here. It does NOT care about the otadata state: a slot previously marked
    // 'invalid' or 'aborted' by the bootloader is happily selected again (and, with
    // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, gets state 'new' -> 'pending_verify' on next boot).
    esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        send_error(request, 500, String("esp_ota_set_boot_partition failed: ") + esp_err_to_name(err));
        return;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    StaticJsonDocument<384> doc;
    doc["ok"] = true;
    doc["boot"] = part->label;
    doc["address"] = part->address;
    doc["running"] = running ? running->label : "";
    doc["project_name"] = desc.project_name;
    doc["version"] = desc.version;
    doc["idf_ver"] = desc.idf_ver;
    doc["restart"] = restart;
    Serial.printf("[OTA] Boot partition set to %s (%s %s, %s)\n", part->label, desc.project_name,
                  desc.version, desc.idf_ver);
    send_json(request, 200, doc);
    if (restart) schedule_restart(1500);
}

// ---------------------------------------------------------------------------------------------
// GET /api/system/partition/<label>   - raw partition download
// GET /api/system/flash?offset=&length= - raw flash download
// ---------------------------------------------------------------------------------------------

class PartitionDownloadHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        return request->method() == HTTP_GET && request->url().startsWith(PARTITION_ROUTE_PREFIX);
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        REQUIRE_AUTH(request);   // raw partitions contain WiFi credentials and secrets
        String label = request->url().substring(strlen(PARTITION_ROUTE_PREFIX));
        while (label.endsWith("/")) label.remove(label.length() - 1);

        const esp_partition_t* part = find_partition_by_label(label.c_str());
        if (!part) {
            send_error(request, 404, "no partition with label '" + label + "'");
            return;
        }
        if (s_upload_active) {
            send_error(request, 409, "firmware update in progress");
            return;
        }

        AsyncWebServerResponse* resp = request->beginResponse(
            CT_OCTET, part->size,
            [part](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                if (index >= part->size) return 0;
                size_t n = part->size - index;
                if (n > maxLen) n = maxLen;
                if (esp_partition_read(part, index, buffer, n) != ESP_OK) {
                    Serial.printf("[OTA] esp_partition_read(%s, %u) failed\n", part->label, (unsigned)index);
                    return 0; // terminates the response early
                }
                return n;
            });

        char disp[80];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s_0x%06x.bin\"", part->label, (unsigned)part->address);
        resp->addHeader("Content-Disposition", disp);
        char addr[16];
        snprintf(addr, sizeof(addr), "0x%06x", (unsigned)part->address);
        resp->addHeader("X-Partition-Address", addr);
        resp->addHeader("X-Partition-Type", partition_type_str(part->type));
        resp->addHeader("X-Partition-Subtype", partition_subtype_str(part->type, part->subtype));
        resp->addHeader("Cache-Control", "no-store");
        request->send(resp);
    }
};

static void handle_flash_download(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    const uint32_t chip_size = ESP.getFlashChipSize();
    uint32_t offset = 0;
    uint32_t length = 0;

    if (request->hasParam("offset")) offset = strtoul(request->getParam("offset")->value().c_str(), nullptr, 0);
    if (request->hasParam("length")) length = strtoul(request->getParam("length")->value().c_str(), nullptr, 0);

    if (offset >= chip_size) {
        send_error(request, 400, "offset beyond flash size (" + String(chip_size) + ")");
        return;
    }
    if (length == 0) length = chip_size - offset;
    if (length > chip_size - offset) {
        send_error(request, 400, "offset+length beyond flash size (" + String(chip_size) + ")");
        return;
    }
    if (s_upload_active) {
        send_error(request, 409, "firmware update in progress");
        return;
    }

    AsyncWebServerResponse* resp = request->beginResponse(
        CT_OCTET, length,
        [offset, length](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= length) return 0;
            size_t n = length - index;
            if (n > maxLen) n = maxLen;
            if (esp_flash_read(nullptr, buffer, offset + index, n) != ESP_OK) {
                Serial.printf("[OTA] esp_flash_read(0x%x) failed\n", (unsigned)(offset + index));
                return 0;
            }
            return n;
        });

    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"flash_0x%06x_0x%06x.bin\"", (unsigned)offset, (unsigned)length);
    resp->addHeader("Content-Disposition", disp);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

// ---------------------------------------------------------------------------------------------
// OTA from a URL (docs/API.md "System, OTA, partitions"): the device fetches the image itself.
//   POST /api/system/update_from_url {"url":"http(s)://host/firmware.bin"} -> 202, the main task
//   flashes on its next iterations (ota_loop); a sleeper runs ota_update_from_url_now() straight
//   from its relay job. Same slot / magic / size rules as the upload; plain http:// only (a LAN
//   image server - TLS would cost ~45 KB of heap on a sleeper for no verification without a CA).
// ---------------------------------------------------------------------------------------------

static char     s_url_job[RELAY_URL_MAX] = "";   // pending job (handler -> main task)
static uint32_t s_url_start_ms = 0;               // the OTA card gets one state-machine tick first

bool ota_update_from_url_now(const char* url) {
    if (s_upload_request) return false;                      // a web upload is in flight
    WiFiClient client;                                       // plain http only: a LAN image server, no TLS
    s_upload_active = true;
    s_upload_ok = false;
    s_upload_written = 0;
    s_upload_error = "";
    indication_overlay(LED_OVL_OTA, true);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    bool ok = false;
    do {
        if (strncasecmp(url, "http://", 7) != 0 || !http.begin(client, url)) { s_upload_error = "http:// url required"; break; }
        int code = http.GET();
        if (code != HTTP_CODE_OK) { s_upload_error = String("http ") + code; break; }
        int size = http.getSize();
        const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
        if (size <= 0 || !target || (size_t)size > target->size) { s_upload_error = "bad image size"; break; }
        s_upload_target = target->label;
        if (!Update.begin((size_t)size, U_FLASH)) { s_upload_error = Update.errorString(); break; }
        WiFiClient* stream = http.getStreamPtr();
        uint8_t* buf = (uint8_t*)malloc(1024);
        unsigned long deadline = millis() + 10000;
        while (buf && stream && s_upload_written < (size_t)size && (long)(deadline - millis()) > 0) {
            size_t avail = stream->available();
            if (avail) {
                size_t n = (size_t)stream->read(buf, avail < 1024 ? avail : 1024);
                if (Update.write(buf, n) != n) { s_upload_error = Update.errorString(); break; }   // incl. the 0xE9 check
                s_upload_written += n;
                deadline = millis() + 10000;
                esp_task_wdt_reset();
            } else if (!stream->connected()) {
                break;
            } else {
                delay(1);
            }
        }
        free(buf);
        if (s_upload_written != (size_t)size) { if (s_upload_error.isEmpty()) s_upload_error = "incomplete download"; break; }
        if (!Update.end(true)) { s_upload_error = Update.errorString(); break; }
        ok = true;
    } while (false);
    http.end();
    if (!ok) {
        if (Update.isRunning()) Update.abort();
        Serial.printf("[OTA] URL update failed: %s\n", s_upload_error.c_str());
        indication_flash(255, 0, 0, 1000);
    } else {
        indication_flash(0, 255, 0, 3000);
    }
    s_upload_active = false;
    s_upload_ok = ok;
    indication_overlay(LED_OVL_OTA, false);
    return ok;
}

static void handle_update_from_url(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    char url[RELAY_URL_MAX];
    if (!relay_url_from_json((const char*)request->_tempObject, url, sizeof(url))) {
        send_error(request, 400, "body {\"url\":\"http(s)://...\"} required");
        return;
    }
    if (s_upload_active || s_url_job[0] || ota_arduino_in_progress()) {
        send_error(request, 409, "another update is in progress");
        return;
    }
    strlcpy(s_url_job, url, sizeof(s_url_job));
    request->send(202, CT_JSON, "{\"status\":\"scheduled\"}");
}

// ---------------------------------------------------------------------------------------------
// Registration / loop
// ---------------------------------------------------------------------------------------------

void ota_register_routes(AsyncWebServer& server) {
    // The update page lives on /system#firmware (GET /update answers 301 there, see
    // ConnectivityManager::setup_webserver). Firmware upload. "/u" is the URL the stock
    // WiFiManager portal posts to - keep it as an alias so the same curl command works
    // against both firmwares.
    server.on("/update", HTTP_POST, handle_update_request, handle_update_upload);
    server.on("/u", HTTP_POST, handle_update_request, handle_update_upload);

    server.on("/api/system/info", HTTP_GET, handle_system_info);

    server.on("/api/system/boot_partition", HTTP_POST, handle_boot_partition, nullptr, collect_small_body);
    server.on("/api/system/update_from_url", HTTP_POST, handle_update_from_url, nullptr, collect_small_body);

    server.on("/api/system/restart", HTTP_POST, [](AsyncWebServerRequest* request) {
        REQUIRE_AUTH(request);
        if (s_upload_active) {
            send_error(request, 409, "firmware update in progress");
            return;
        }
        StaticJsonDocument<64> doc;
        doc["ok"] = true;
        doc["message"] = "restarting";
        send_json(request, 200, doc);
        schedule_restart(500);
    });

    server.on("/api/system/flash", HTTP_GET, handle_flash_download);
    server.addHandler(new PartitionDownloadHandler());
}

bool ota_update_in_progress() {
    return s_upload_active;
}

bool ota_other_slot(char* label, size_t label_len, bool select, const char** err) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    esp_app_desc_t desc;
    if (!other || !running || other->address == running->address) { *err = "no other app slot"; return false; }
    if (!partition_has_image_magic(other) || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        *err = "other slot holds no valid image";
        return false;
    }
    strlcpy(label, other->label, label_len);
    if (!select) return true;
    if (s_upload_active) { *err = "firmware update in progress"; return false; }
    if (esp_ota_set_boot_partition(other) != ESP_OK) { *err = "esp_ota_set_boot_partition failed"; return false; }
    Serial.printf("[OTA] Boot partition set to %s (recovery)\n", other->label);
    return true;
}

void ota_loop() {
    // Compute the sketch size / MD5 once, on the main task, so /api/system/info never verifies
    // and hashes the whole image (~1.1 MB) inside the async_tcp task. The MD5 is computed here
    // instead of via ESP.getSketchMD5() so that a request arriving during the computation can
    // neither race the verification nor poison the core's static cache (see s_sketch_md5).
    if (s_sketch_size == 0) {
        uint32_t size = ESP.getSketchSize();
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (size > 0 && running) {
            MD5Builder md5;
            md5.begin();
            uint8_t* buf = (uint8_t*)malloc(SPI_FLASH_SEC_SIZE);
            if (buf) {
                uint32_t off = 0;
                bool ok = true;
                while (off < size && ok) {
                    size_t n = size - off < SPI_FLASH_SEC_SIZE ? size - off : SPI_FLASH_SEC_SIZE;
                    ok = esp_partition_read(running, off, buf, n) == ESP_OK;
                    if (ok) { md5.add(buf, n); off += n; }
                }
                free(buf);
                if (ok) {
                    md5.calculate();
                    md5.getChars(s_sketch_md5);
                    s_sketch_size = size;   // published last: handlers see size + md5 together
                }
            }
        }
    }

    // LAN push-OTA (ArduinoOTA); no-op unless ota_arduino_begin() succeeded.
    ota_arduino_handle();

    // OTA from a URL: first mark the update active so the state machine draws the OTA card
    // (one 1 s tick), then fetch and flash - blocking on this task, restart when done.
    if (s_url_job[0] && !s_restart_pending) {
        if (!s_url_start_ms) {
            s_url_start_ms = millis() + 1500;
            s_upload_active = true;
            indication_overlay(LED_OVL_OTA, true);
        } else if ((int32_t)(millis() - s_url_start_ms) >= 0) {
            char url[RELAY_URL_MAX];
            strlcpy(url, s_url_job, sizeof(url));
            bool ok = ota_update_from_url_now(url);
            s_url_job[0] = '\0';
            s_url_start_ms = 0;
            if (ok) schedule_restart(1500);
        }
    }

    if (s_restart_pending && (int32_t)(millis() - s_restart_at_ms) >= 0) {
        s_restart_pending = false;
        peers_flush();   // planned restart: persist the peer table (docs/MULTI_DEVICE.md "The peer table")
        Serial.println("[OTA] Restarting...");
        Serial.flush();
        ESP.restart();
    }
}
