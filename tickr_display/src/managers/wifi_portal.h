#pragma once
//
// Minimal captive portal for first-time Wi-Fi setup (replaces WiFiManager),
// and the access point of the power-cycle recovery mode.
//
// Runs on the firmware's own AsyncWebServer plus the Arduino DNSServer:
//   * open SoftAP "TickrDisplay" on WIFI_PORTAL_AP_IP (192.168.244.1 - a
//     subnet no home router uses, so it never collides with the network the
//     device is meant to join), DNS answers every name with that address,
//     unknown URLs (incl. the OS captive-portal probes /generate_204,
//     /hotspot-detect.html, /connecttest.txt, /ncsi.txt, /fwlink,
//     /success.txt, /canonical.html ...) are redirected to the portal page,
//     which is also served as "/" - for everybody in setup mode, for the AP
//     clients only in recovery mode;
//   * GET  /wifi              portal page (gzip PROGMEM, www/src/wifi.html)
//   * GET  /api/wifi/scan     [{ssid,rssi,secure}] (async scan, 202 while running,
//                             ?rescan=1 forces a new scan)
//   * POST /api/wifi/connect  ssid=&pass= (form) -> 202; the connect itself runs
//                             on the main task (wifi_portal_tick)
//   * POST /api/wifi/forget   erase the saved credentials, disconnect the STA
//   * GET  /api/wifi/status   {portal,captive,state,ssid,ip,rssi,saved,error,...}
//   status is public; the scan is open for "captive" requests (setup
//   portal active, or recovery mode and the request arrived through the AP
//   interface) and requires the API token otherwise; connect/forget answer 403 when the
//   request is not captive - a token holder on the LAN cannot change the
//   network, only somebody at the device can (SECURITY.md).
//   Every other route of the firmware (/update in particular) stays reachable
//   in AP mode, so a device that cannot join a network can still be reflashed.
//
// Two ways the AP comes up:
//   * setup portal (wifi_portal_start): no credentials or the connect failed
//     on USB power; ConnectivityManager::init() blocks in it.
//   * recovery AP (wifi_portal_start_ap): AP+STA next to normal operation
//     (recovery_manager.cpp); the STA link, MQTT, pull and the panel go on.
//
// Credentials are handed to the Wi-Fi driver with WiFi.begin(ssid, pass) in
// its default persistent mode, i.e. they land in the same NVS record that
// WiFi.begin() reads on the next boot and that the stock firmware uses too.
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#define WIFI_PORTAL_AP_SSID "TickrDisplay"
#define WIFI_PORTAL_AP_IP   "192.168.244.1"

enum WifiPortalState : uint8_t {
    WIFI_PORTAL_IDLE = 0,     // no attempt made through the portal yet
    WIFI_PORTAL_CONNECTING,
    WIFI_PORTAL_CONNECTED,
    WIFI_PORTAL_FAILED,
};

// Register /wifi, /api/wifi/* and the catch-all redirect (onNotFound). Call
// once before server.begin(); the handlers behave according to
// wifi_portal_active() / wifi_portal_captive() at request time.
void wifi_portal_register_routes(AsyncWebServer& server);

// Whether NVS holds Wi-Fi credentials (reported as "saved" by /api/wifi/status).
void wifi_portal_set_saved_credentials(bool saved);

// Setup portal: bring up SoftAP + DNS and draw the setup screen.
void wifi_portal_start(const char* ap_ssid);
// Recovery: bring up SoftAP + DNS next to the STA link, no screen change.
void wifi_portal_start_ap(const char* ap_ssid);

// Main-task housekeeping while the AP is up: DNS, pending connect/forget
// requests, connect timeout. Call every few ms.
void wifi_portal_tick();

// Stop DNS + SoftAP and go back to plain STA mode (keeps an established STA
// connection). Ends both the setup portal and the recovery AP.
void wifi_portal_stop();

bool wifi_portal_active();      // setup portal (blocking first-time flow)
bool wifi_portal_ap_up();       // AP up in either mode
// True when the request reached us through the AP interface (the socket's
// local address is the AP address). Not a header, cannot be forged from the LAN.
bool wifi_portal_request_via_ap(AsyncWebServerRequest* request);
// "Serve the portal to this client": setup portal active, or AP up in
// recovery mode and the request came through the AP.
bool wifi_portal_captive(AsyncWebServerRequest* request);
WifiPortalState wifi_portal_state();
// Set when a portal-initiated connect succeeded (wifi_portal_tick()).
bool wifi_portal_connected();
