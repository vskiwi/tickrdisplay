#include "power_api.h"
#include "web_auth.h"
#include "../hal/hal_power.h"
#include "../hal/hal_power_probe.h"
#include "../hal/hal_pins.h"
#include <Arduino.h>

static const char CT_JSON[] = "application/json";
static AppConfig* s_cfg = nullptr;

static int channel_json(char* out, size_t cap, const char* name, const PowerChannelRaw& c) {
    return snprintf(out, cap,
        "\"%s\":{\"pin\":%u,\"raw\":%u,\"raw_min\":%u,\"raw_max\":%u,\"pin_mv\":%lu,\"mv\":%lu}",
        name, c.pin, c.raw, c.raw_min, c.raw_max, (unsigned long)c.pin_mv, (unsigned long)c.mv);
}

static const char* sense_str(PowerSense s) {
    return s == POWER_SENSE_USB ? "usb" : s == POWER_SENSE_BATTERY ? "battery" : "invalid";
}

// {"uptime_s":..,"board":"A|B|?","rule":"rail|diff|none","sense_en":1,"boot_vin_raw":2560,
//  "boot_rail_min_mv":4420,"boot_rail_max_mv":4690,"boot_diff_min_mv":180,"boot_diff_max_mv":262,
//  "atten_db":11,"divider":"21/10","cell_divider":"189/100",
//  "cal":"efuse_vref","vref_mv":1100,"adc_max_mv":3150,"sat_raw":4080,"floor_raw":8,
//  "vsys":{pin,raw,raw_min,raw_max,pin_mv,mv},"vin":{...},"diff_mv":262,
//  "power":{"source":"usb|battery|unknown","sense":"usb|battery|invalid","mode":"auto|usb|battery",
//           "rail_on_mv":4500,"rail_off_mv":4350,"diff_on_mv":80,"diff_off_mv":-30},
//  "aux":[{"pin":34,"raw":..,"pin_mv":..,"level":0},...]}
// vsys = GPIO 32 (the cell, mv = pin_mv x cell_divider), vin = GPIO 33 (the
// rail, mv = pin_mv x divider); diff_mv = vin.pin_mv - vsys.pin_mv. The
// decision uses vin.mv on rule "rail" (rev A) and diff_mv on rule "diff"
// (rev B) - docs/HARDWARE.md "Power sensing".
static void handle_power_raw(AsyncWebServerRequest* request) {   // public: ADC readings only
    PowerRaw r;
    power_read_raw(&r);

#ifdef TICKR_POWER_PROBE
    char buf[1664];   // + boot_power_hint, boot_vin and up to 10 adc2_boot entries
#else
    char buf[1152];   // ~900 B with the field widths below
#endif
    size_t n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
        "{\"uptime_s\":%lu,\"board\":\"%s\",\"rule\":\"%s\",\"sense_en\":%u,\"boot_vin_raw\":%u,"
        "\"boot_rail_min_mv\":%lu,\"boot_rail_max_mv\":%lu,\"boot_diff_min_mv\":%ld,\"boot_diff_max_mv\":%ld,"
        "\"atten_db\":%u,\"divider\":\"%u/%u\",\"cell_divider\":\"%u/%u\","
        "\"cal\":\"%s\",\"vref_mv\":%u,\"adc_max_mv\":%lu,\"sat_raw\":%u,\"floor_raw\":%u,",
        (unsigned long)(millis() / 1000), board_profile_str(r.board), power_rule_str(r.board), r.sense_en,
        r.boot_vin_raw, (unsigned long)r.boot_rail_min_mv, (unsigned long)r.boot_rail_max_mv,
        (long)r.boot_diff_min_mv, (long)r.boot_diff_max_mv, r.atten_db,
        r.divider_num, r.divider_den, r.cell_num, r.cell_den, r.cal, r.vref_mv,
        (unsigned long)r.adc_max_mv, r.sat_raw, r.floor_raw);
    n += channel_json(buf + n, sizeof(buf) - n, "vsys", r.vsys);
    n += snprintf(buf + n, sizeof(buf) - n, ",");
    n += channel_json(buf + n, sizeof(buf) - n, "vin", r.vin);
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"diff_mv\":%ld,\"power\":{\"source\":\"%s\",\"sense\":\"%s\",\"mode\":\"%s\","
        "\"rail_on_mv\":%u,\"rail_off_mv\":%u,\"diff_on_mv\":%d,\"diff_off_mv\":%d},\"aux\":[",
        (long)r.diff_mv, power_source_label(), sense_str(r.sense), config_power_source_str((uint8_t)r.mode),
        r.rail_on_mv, r.rail_off_mv, (int)r.diff_on_mv, (int)r.diff_off_mv);
    for (int i = 0; i < POWER_AUX_PINS && n < sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"pin\":%u,\"raw\":%u,\"pin_mv\":%lu,\"level\":%u}",
                      i ? "," : "", r.aux[i].pin, r.aux[i].raw, (unsigned long)r.aux[i].pin_mv, r.aux[i].level);
    }
#ifdef TICKR_POWER_PROBE
    // Boot-time snapshot (hal_power_probe.h): the ADC1 detector's verdict at
    // the start of setup() and the free ADC2 channels, readable only then;
    // plus the GPIO 4 state so a reading can be tied to the enable test.
    if (n < sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - n,
            "],\"gpio4_mode\":\"%s\",\"boot_power_hint\":\"%s\","
            "\"boot_vin\":{\"pin\":%u,\"raw\":%u,\"pin_mv\":%lu},\"adc2_boot\":[",
            power_probe_gpio4_mode(), power_probe_boot_hint(), PIN_USB_VIN, power_probe_boot_vin_raw(),
            (unsigned long)power_probe_boot_vin_pin_mv());
    }
    for (size_t i = 0; i < power_probe_adc2_count() && n < sizeof(buf); i++) {
        const PowerProbeAdc2* a = power_probe_adc2(i);
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"gpio\":%u,\"ch\":%d,\"ok\":%s,\"raw\":%u,\"raw_min\":%u,\"raw_max\":%u,\"mv\":%lu}",
            i ? "," : "", a->gpio, a->ch, a->ok ? "true" : "false", a->raw, a->raw_min, a->raw_max,
            (unsigned long)a->mv);
    }
#endif
    if (n < sizeof(buf)) n += snprintf(buf + n, sizeof(buf) - n, "]}");
    if (n >= sizeof(buf)) {   // cannot happen with the field widths above; fail loudly rather than truncate
        request->send(500, CT_JSON, "{\"error\":\"buffer\"}");
        return;
    }
    AsyncWebServerResponse* resp = request->beginResponse(200, CT_JSON, buf);
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
}

static void handle_power_source(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    if (!request->hasParam("mode", true)) {
        request->send(400, CT_JSON, "{\"error\":\"mode=auto|usb|battery required\"}");
        return;
    }
    String mode = request->getParam("mode", true)->value();
    if (mode != "auto" && mode != "usb" && mode != "battery") {
        request->send(400, CT_JSON, "{\"error\":\"mode must be auto, usb or battery\"}");
        return;
    }
    uint8_t v = config_power_source_parse(mode.c_str());
    power_set_mode((PowerMode)v);
    bool saved = false;
    if (s_cfg) {
        s_cfg->power_source = v;
        saved = config_save(*s_cfg);
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"mode\":\"%s\",\"saved\":%s,\"source\":\"%s\"}",
             config_power_source_str(v), saved ? "true" : "false", power_source_label());
    request->send(200, CT_JSON, body);
}

#ifdef TICKR_POWER_PROBE
static void probe_entry_json(AsyncResponseStream* out, const PowerProbeGpio& g, const PowerProbePin& p, bool first) {
    out->printf("%s{\"gpio\":%u", first ? "" : ",", p.gpio);
    if (g.probed) {
        out->printf(",\"float\":%u", g.lvl_float);
        if (g.lvl_pu != PROBE_NA) out->printf(",\"pu\":%u,\"pd\":%u", g.lvl_pu, g.lvl_pd);
    }
    out->printf(",\"verdict\":\"%s\"", power_probe_verdict(g, p));
    if (p.note) out->printf(",\"note\":\"%s\"", p.note);
    out->print("}");
}

// GET /api/power/probe - digital pull probe of every free GPIO, at boot
// (before HAL / Wi-Fi init) and right now. See hal_power_probe.h.
//   bound_high / bound_low : same level with pull-up and pull-down - tied
//                            or driven externally (charger status, VBUS ...)
//   free                   : follows the pull - nothing connected
//   level_only             : input-only pad (34-39), no pulls available
//   skipped                : pin taken by a peripheral at this stage
static void handle_power_probe(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    size_t n = power_probe_gpio_count();
    PowerProbeGpio* now = (PowerProbeGpio*)malloc(n * sizeof(PowerProbeGpio));
    if (!now) {
        request->send(500, CT_JSON, "{\"error\":\"oom\"}");
        return;
    }
    power_probe_gpio_now(now);

    AsyncResponseStream* out = request->beginResponseStream(CT_JSON);
    out->printf("{\"uptime_s\":%lu,\"settle_ms\":%u,\"boot_power_hint\":\"%s\",\"power\":\"%s\","
                "\"legend\":{\"bound_high\":\"same level (1) with pull-up and pull-down: tied/driven externally\","
                "\"bound_low\":\"same level (0) with pull-up and pull-down: tied/driven externally\","
                "\"free\":\"follows the pull: unconnected\",\"level_only\":\"input-only pad, no pulls\"},"
                "\"boot\":[",
                (unsigned long)(millis() / 1000), power_probe_settle_ms(), power_probe_boot_hint(),
                power_source_label());
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        const PowerProbePin* p = power_probe_pin(i);
        if (p->kind == PROBE_PIN_EXCLUDED) continue;
        probe_entry_json(out, *power_probe_gpio_boot(i), *p, first);
        first = false;
    }
    out->print("],\"now\":[");
    first = true;
    for (size_t i = 0; i < n; i++) {
        const PowerProbePin* p = power_probe_pin(i);
        if (p->kind == PROBE_PIN_EXCLUDED) continue;
        probe_entry_json(out, now[i], *p, first);
        first = false;
    }
    out->print("],\"excluded\":[");
    first = true;
    for (size_t i = 0; i < n; i++) {
        const PowerProbePin* p = power_probe_pin(i);
        if (p->kind != PROBE_PIN_EXCLUDED) continue;
        out->printf("%s{\"gpio\":%u,\"reason\":\"%s\"%s}", first ? "" : ",", p->gpio, p->note,
                    p->adc2_ch >= 0 ? ",\"adc2\":true" : "");
        first = false;
    }
    out->print("]}");
    free(now);
    out->addHeader("Cache-Control", "no-cache");
    request->send(out);
}

// POST /api/power/probe/gpio4?mode=high|low|pulldown|pullup|float|restore
// (query string or form body) - the GPIO 4 enable test (hal_power_probe.h):
// apply the mode, wait the stock's 150 ms, then the same 16-sample read of
// GPIO 32/33 and one pass of GPIO 34-39 as GET /api/power/raw. `restore`
// returns the pad to the firmware's state (sense enable HIGH).
// GET on the same path reports the current mode and level only.
//   {"gpio4":{"mode":"high","level":1},"settle_ms":150,
//    "gpio32":{pin,raw,raw_min,raw_max,pin_mv,mv},"gpio33":{...},
//    "aux":[{"pin":34,"raw":..,"pin_mv":..,"level":0},...]}
// mv = pin_mv x the channel's divider; pin_mv is the calibrated pad voltage.
// While GPIO 4 is low the detector reads INVALID on rev B (power: unknown).
static void handle_power_probe_gpio4(AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request);
    bool post = request->method() == HTTP_POST;
    if (post) {
        const AsyncWebParameter* p = request->hasParam("mode") ? request->getParam("mode")
                                   : request->hasParam("mode", true) ? request->getParam("mode", true) : nullptr;
        if (!p || !power_probe_gpio4_set(p->value().c_str())) {
            request->send(400, CT_JSON, "{\"error\":\"mode=high|low|pulldown|pullup|float|restore required\"}");
            return;
        }
    }
    char buf[768];
    size_t n = snprintf(buf, sizeof(buf), "{\"gpio4\":{\"mode\":\"%s\",\"level\":%u},\"settle_ms\":%u",
                        power_probe_gpio4_mode(), power_probe_gpio4_level(), (unsigned)GPIO4_SETTLE_MS);
    if (post) {
        PowerRaw r;
        power_read_raw(&r);
        n += snprintf(buf + n, sizeof(buf) - n, ",");
        n += channel_json(buf + n, sizeof(buf) - n, "gpio32", r.vsys);
        n += snprintf(buf + n, sizeof(buf) - n, ",");
        n += channel_json(buf + n, sizeof(buf) - n, "gpio33", r.vin);
        n += snprintf(buf + n, sizeof(buf) - n, ",\"aux\":[");
        for (int i = 0; i < POWER_AUX_PINS && n < sizeof(buf); i++) {
            n += snprintf(buf + n, sizeof(buf) - n, "%s{\"pin\":%u,\"raw\":%u,\"pin_mv\":%lu,\"level\":%u}",
                          i ? "," : "", r.aux[i].pin, r.aux[i].raw, (unsigned long)r.aux[i].pin_mv, r.aux[i].level);
        }
        if (n < sizeof(buf)) n += snprintf(buf + n, sizeof(buf) - n, "]");
    }
    if (n < sizeof(buf)) n += snprintf(buf + n, sizeof(buf) - n, "}");
    if (n >= sizeof(buf)) {
        request->send(500, CT_JSON, "{\"error\":\"buffer\"}");
        return;
    }
    AsyncWebServerResponse* resp = request->beginResponse(200, CT_JSON, buf);
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
}
#endif

void power_api_register_routes(AsyncWebServer& server, AppConfig* cfg) {
    s_cfg = cfg;
    server.on("/api/power/raw", HTTP_GET, handle_power_raw);
    server.on("/api/power/source", HTTP_POST, handle_power_source);
#ifdef TICKR_POWER_PROBE
    server.on("/api/power/probe", HTTP_GET, handle_power_probe);
    server.on("/api/power/probe/gpio4", HTTP_GET | HTTP_POST, handle_power_probe_gpio4);
#endif
}
