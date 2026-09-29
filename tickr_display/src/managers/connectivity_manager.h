#pragma once
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include "config_manager.h"
#include "web_auth.h"
#include "../logic/source.h"
#include <HTTPClient.h>

struct PullClient;

typedef std::function<void(const String& payload)> DataCallback;

class ConnectivityManager {
public:
    ConnectivityManager();
    // Starts the web server, connects to WiFi with the credentials saved in
    // NVS and - when there are none or the connect fails on USB power - runs
    // the setup portal (AP "TickrDisplay", see wifi_portal.h) for up to 180 s
    // of inactivity, blocking. Then starts MQTT. `onData` is invoked
    // synchronously by fetch_pull_data() (battery mode, main task). Payloads
    // arriving over HTTP/MQTT are queued and executed by the main loop.
    // low_power: battery mode. With saved credentials the portal is never
    // opened; a 20 s connect attempt is made instead. Returns true when WiFi
    // is connected (MQTT is only started then).
    bool init(DataCallback onData, bool low_power = false);
    // True when WiFi credentials were found in NVS during init().
    bool wifiCredentialsSaved() const { return _has_creds; }
    void loop();
    AppConfig& getConfig();
    // One pull of the configured target (docs/TICKERS.md "What the Ticker source does"): the ticker
    // source when source_kind is `ticker` (fetch + extract + render), else the
    // Pull URL (a payload body, handed to onData). False = nothing configured
    // or the fetch failed (last_error says why).
    bool fetch_pull_data();
    // A pull target is configured (ticker source, or a Pull URL).
    bool pullConfigured() const;
    // "ticker" | "url" | "none", and the ticker's symbol ("" otherwise) - for /api/screen/state.
    const char* pullSourceStr() const;
    const char* tickerSymbol() const;

    // True once after POST /config changed the Pull URL / ticker source or the
    // interval (the USB pull scheduler fetches at once, docs/TICKERS.md "Fetch schedule and errors"); clears the flag.
    bool pullReconfigured() { bool f = _pull_reconfigure; _pull_reconfigure = false; return f; }

    bool mqttConnected() { return mqtt.connected(); }
    bool tlsInsecure() const { return _tls_insecure_used; }
    // status.last_error: the latest failure of any source; a successful pull
    // clears a "pull: ..." entry, the others stay until overwritten.
    const char* lastError() const { return _last_error; }
    void setLastError(const char* msg);

private:
    AsyncWebServer server;
    WiFiClient espClient;
    PubSubClient mqtt;
    DataCallback _onData;
    AppConfig _config;
    bool _has_creds = false;

    // MQTT state
    char _mqtt_client_id[24] = "";
    char _mqtt_status_topic[80] = "";
    unsigned long _mqtt_next_attempt = 0;
    unsigned long _mqtt_backoff_ms = 5000;
    bool _mqtt_configured = false;
    volatile bool _mqtt_reconfigure = false;
    volatile bool _pull_reconfigure = false;

    bool _tls_insecure_used = false;
    char _last_error[96] = "";

    // POST /api/source/test: the paused request and the plan to fetch (main task).
    volatile bool _test_pending = false;
    bool _test_custom = false;
    SourcePlan _test_plan = {};
    AsyncWebServerRequestPtr _test_req;

    bool wait_for_sta(uint32_t timeout_ms);
    bool on_wifi_connected(bool low_power);
    void setup_webserver();
    void setup_mqtt();
    void connect_mqtt();
    void mqtt_callback(char* topic, byte* payload, unsigned int length);
    void build_mqtt_status_topic();

    // HTTP handlers
    void handle_config_api(AsyncWebServerRequest* request);
    void handle_config_post(AsyncWebServerRequest* request);
    void handle_status(AsyncWebServerRequest* request);
    void handle_identity_get(AsyncWebServerRequest* request);
    void handle_identity_post(AsyncWebServerRequest* request);
    void handle_screen_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
    void handle_screen_done(AsyncWebServerRequest* request);

    // Validates + enqueues a screen payload. Returns HTTP status code, error text in `err`.
    int enqueue_payload(const char* data, size_t len, uint8_t src, char* err, size_t err_len);

    // Pull plumbing (connectivity_manager.cpp, "HTTP pull"): the transport for
    // one GET (plain / TLS per mode), the bounded GET itself, the ticker fetch
    // and the source test.
    bool open_client(struct PullClient& pc, bool https, uint8_t tls_mode);
    bool http_get_body(WiFiClient& client, const char* url, char* buf, size_t* got);
    bool fetch_source(const SourcePlan& plan, bool custom, SourceResult* res, char* err, size_t err_len, uint32_t* ms);
    bool fetch_ticker();
    void handle_source_test(AsyncWebServerRequest* request);
    void run_source_test();
    void parse_source_params(AsyncWebServerRequest* request, uint8_t* kind, SourceSpec& tk, String& err);
};
