#include "wifi_link.h"
#include "../log.h"
#include "../logic/wifi_supervisor.h"
#include "../logic/hostname.h"
#include "../hal/hal_display.h"
#include <WiFi.h>
#include <esp_system.h>
#include <esp_rom_crc.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Counters in RTC slow memory. RTC_NOINIT_ATTR (not RTC_DATA_ATTR, see
// hal_display.cpp): never initialised by the loader, so the values live
// through ESP.restart() and deep sleep; a power-on leaves random memory,
// which the magic + CRC catch - and the reset kind zeroes them anyway.
// ---------------------------------------------------------------------------
#define WIFI_RTC_MAGIC 0x57494C31u   // "WIL1": bump when the layout changes

struct WifiRtcStats {
    uint32_t magic;
    uint32_t disconnects;
    uint32_t reconnects;
    uint32_t restarts;
    uint8_t  last_reason;
    uint8_t  pad[3];
    uint32_t crc;
};
RTC_NOINIT_ATTR static WifiRtcStats s_rtc;

static uint32_t rtc_crc() {
    return esp_rom_crc32_le(0, (const uint8_t*)&s_rtc, offsetof(WifiRtcStats, crc));
}
static void rtc_commit() {
    s_rtc.magic = WIFI_RTC_MAGIC;
    s_rtc.crc = rtc_crc();
}
static void rtc_load() {
    esp_reset_reason_t r = esp_reset_reason();
    bool warm = r == ESP_RST_SW || r == ESP_RST_DEEPSLEEP;
    if (!warm || s_rtc.magic != WIFI_RTC_MAGIC || s_rtc.crc != rtc_crc()) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        rtc_commit();
    }
}

// ---------------------------------------------------------------------------
// Driver events. The handler runs on the Wi-Fi event task: it only records
// into plain volatiles and prints one line; the main task folds them into
// the RTC record (wifi_link_loop / wifi_link_stats), so the CRC is never
// torn by two writers.
// ---------------------------------------------------------------------------
static volatile bool     s_had_ip = false;       // the link had an IP since the last disconnect
static volatile uint32_t s_evt_drops = 0;        // disconnects while s_had_ip (pending fold)
static volatile uint8_t  s_evt_reason = 0;

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        s_had_ip = true;
        Serial.printf("WiFi: link up, IP %s\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        if (!s_had_ip) return;                   // the core's retry loop, or our own disconnect() while down
        s_had_ip = false;
        s_evt_reason = reason;
        s_evt_drops = s_evt_drops + 1;
        Serial.printf("WiFi: link lost, reason %u (%s)\n", reason,
                      WiFi.disconnectReasonName((wifi_err_reason_t)reason));
    } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
        // Still associated but the address is gone (a lease not renewed):
        // counted as a loss with reason 0 - there was no deauthentication.
        if (!s_had_ip) return;                   // follows a disconnect 2 min later: already counted
        s_had_ip = false;
        s_evt_reason = 0;
        s_evt_drops = s_evt_drops + 1;
        Serial.println("WiFi: link lost, IP address gone (still associated)");
    }
}

// Main task only (one writer of the RTC record).
static void fold_events() {
    uint32_t drops = s_evt_drops;
    if (!drops) return;
    s_evt_drops = s_evt_drops - drops;
    s_rtc.disconnects += drops;
    s_rtc.last_reason = s_evt_reason;
    rtc_commit();
}

// ---------------------------------------------------------------------------
// Supervisor
// ---------------------------------------------------------------------------
static WifiSupervisor s_sup;
static bool     s_armed = false;
static uint32_t s_last_poll_ms = 0;

static WifiLinkState link_state() {
    switch (WiFi.status()) {
        case WL_CONNECTED:       return WIFI_LINK_UP;
        case WL_IDLE_STATUS:     return WIFI_LINK_ASSOCIATED;   // connecting, or the IP was lost
        case WL_DISCONNECTED:
        case WL_CONNECTION_LOST: return WIFI_LINK_DOWN;
        default:                 return WIFI_LINK_GAVE_UP;      // WL_CONNECT_FAILED, WL_NO_SSID_AVAIL, WL_NO_SHIELD
    }
}

void wifi_link_begin(const char* device_name) {
    rtc_load();
    uint8_t mac[6];
    WiFi.macAddress(mac);                 // esp_read_mac() while the driver is still off
    char host[HOSTNAME_MAX_LEN + 1];
    hostname_build(device_name, mac, host, sizeof(host));
    WiFi.setHostname(host);               // stored; the core hands it to the netif when the STA starts
    WiFi.onEvent(on_wifi_event);
    LOGV("WiFi: hostname %s\n", host);
}

void wifi_link_arm() {
    s_sup = WifiSupervisor();
    s_armed = true;
    s_last_poll_ms = millis();
}

void wifi_link_loop(bool hold) {
    fold_events();
    if (!s_armed) return;
    uint32_t now = millis();
    if (now - s_last_poll_ms < 1000) return;
    s_last_poll_ms = now;

    WifiLinkState link = link_state();
    bool recovered = false;
    WifiSupervisorAction act = wifi_sup_poll(&s_sup, now, link, hold, &recovered);
    if (recovered) {
        s_rtc.reconnects++;
        rtc_commit();
        Serial.printf("WiFi: link back after %u own attempt(s)\n", (unsigned)s_sup.attempts);
    }
    if (act == WIFI_SUP_RECONNECT) {
        Serial.printf("WiFi: down %lu s (status %d) - reconnecting, attempt %u\n",
                      (unsigned long)wifi_sup_down_s(&s_sup, now), (int)WiFi.status(), (unsigned)s_sup.attempts);
        WiFi.disconnect();                // keeps the radio and the NVS credentials
        WiFi.begin();                     // the saved network, DHCP client restarted
    } else if (act == WIFI_SUP_RESTART) {
        Serial.printf("WiFi: down %lu s - restarting\n", (unsigned long)wifi_sup_down_s(&s_sup, now));
        s_rtc.restarts++;
        rtc_commit();
        Serial.flush();
        display_prepare_sleep();          // panel powered down cleanly; the RTC frame copy seeds the next partial
        delay(50);
        ESP.restart();
    }
}

// Any task (GET /api/status runs on async_tcp): reads only, the pending
// events are added without folding them.
void wifi_link_stats(WifiLinkStats* out) {
    uint32_t pending = s_evt_drops;
    out->disconnects = s_rtc.disconnects + pending;
    out->reconnects = s_rtc.reconnects;
    out->restarts = s_rtc.restarts;
    out->last_reason = pending ? s_evt_reason : s_rtc.last_reason;
    out->down_s = s_armed ? wifi_sup_down_s(&s_sup, millis()) : 0;
}
