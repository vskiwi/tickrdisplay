#include "arduino_ota.h"

// Whole file is compiled only with -DTICKR_ARDUINO_OTA (see arduino_ota.h).
#ifdef TICKR_ARDUINO_OTA

#include "ota_manager.h"
#include "../hal/hal_indication.h"
#include "../hal/hal_display.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

static const uint16_t OTA_PORT = 3232;

static bool s_enabled = false;
static bool s_in_progress = false;
static char s_hostname[32] = "";
static unsigned int s_last_pct = 0;

static bool looks_like_md5(const char* s) {
    if (strlen(s) != 32) return false;
    for (const char* p = s; *p; p++) {
        if (!isxdigit((unsigned char)*p)) return false;
    }
    return true;
}

static const char* ota_error_str(ota_error_t e) {
    switch (e) {
        case OTA_AUTH_ERROR:    return "auth failed";
        case OTA_BEGIN_ERROR:   return "begin failed (no OTA partition / image too large?)";
        case OTA_CONNECT_ERROR: return "connect failed";
        case OTA_RECEIVE_ERROR: return "receive failed";
        case OTA_END_ERROR:     return "end failed (image verification)";
        default:                return "unknown";
    }
}

bool ota_arduino_begin(const char* password, bool usb_powered) {
    if (s_enabled) return true;

    if (!password || !*password) {
        Serial.println("[ArduinoOTA] disabled: no ota_password configured (set it on /config to enable LAN push-OTA)");
        return false;
    }
    if (!usb_powered) {
        Serial.println("[ArduinoOTA] disabled: not on USB power");
        return false;
    }
    if (WiFi.getMode() != WIFI_STA || !WiFi.isConnected()) {
        Serial.println("[ArduinoOTA] disabled: WiFi is not connected in STA mode");
        return false;
    }

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_hostname, sizeof(s_hostname), "tickrdisplay-%02x%02x%02x", mac[3], mac[4], mac[5]);

    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(s_hostname);   // also registers mDNS <hostname>.local / _arduino._tcp
    ArduinoOTA.setMdnsEnabled(true);
    ArduinoOTA.setRebootOnSuccess(true);  // restart happens inside handle() -> main loop context
    if (looks_like_md5(password)) {
        ArduinoOTA.setPasswordHash(password);
    } else {
        ArduinoOTA.setPassword(password);
    }

    ArduinoOTA.onStart([]() {
        s_in_progress = true;
        s_last_pct = 0;
        Serial.printf("[ArduinoOTA] start (%s)\n", ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
        indication_overlay(LED_OVL_OTA, true); // blue while flashing (docs/DEVICE_UI.md "LED and sound")
        // The transfer blocks the main loop inside handle(), so the state
        // machine cannot draw the OTA card - draw it here (main task).
        display_set_card(CARD_OTA, DS_AGE_UNKNOWN);
        display_show_base();
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // The whole transfer runs inside ArduinoOTA.handle() on the main task,
        // which is subscribed to the task watchdog (30 s) - keep feeding it.
        esp_task_wdt_reset();
        if (total == 0) return;
        unsigned int pct = progress * 100u / total;
        if (pct >= s_last_pct + 10 || pct == 100) {
            s_last_pct = pct - (pct % 10);
            Serial.printf("[ArduinoOTA] %u%%\n", pct);
        }
    });
    ArduinoOTA.onEnd([]() {
        s_in_progress = false;
        Serial.println("[ArduinoOTA] done, rebooting");
        indication_overlay(LED_OVL_OTA, false);
        indication_flash(0, 255, 0, 3000);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        s_in_progress = false;
        Serial.printf("[ArduinoOTA] error %d: %s\n", (int)error, ota_error_str(error));
        indication_overlay(LED_OVL_OTA, false);
    });

    ArduinoOTA.begin();
    s_enabled = true;
    Serial.printf("[ArduinoOTA] listening on %s:%u (mDNS %s.local), auth %s\n",
                  WiFi.localIP().toString().c_str(), OTA_PORT, s_hostname,
                  looks_like_md5(password) ? "md5" : "password");
    return true;
}

void ota_arduino_handle() {
    if (!s_enabled) return;
    // The Update object is shared with the HTTP /update handler; don't let a push-OTA session
    // start while an HTTP upload is being written.
    if (ota_update_in_progress()) return;
    ArduinoOTA.handle();
}

bool ota_arduino_enabled() { return s_enabled; }
const char* ota_arduino_hostname() { return s_hostname; }
uint16_t ota_arduino_port() { return OTA_PORT; }
bool ota_arduino_in_progress() { return s_in_progress; }

#endif // TICKR_ARDUINO_OTA
