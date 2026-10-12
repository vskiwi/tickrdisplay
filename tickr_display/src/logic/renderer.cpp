#include "renderer.h"
#include "../hal/hal_display.h"
#include "../hal/hal_indication.h"

void renderer_init() {
    // nothing special yet
}

void renderer_play_sound(PayloadSound sound, const char* rtttl) {
    switch (sound) {
        case SOUND_BEEP:
            indication_beep(2000, 100);
            break;
        case SOUND_DOUBLE_BEEP:
            indication_beep(2000, 100);
            delay(100);
            indication_beep(2000, 100);
            break;
        case SOUND_LONG_BEEP:
            indication_beep(2000, 500);
            break;
        case SOUND_RTTTL:
            if (rtttl) indication_play_rtttl(rtttl);
            break;
        case SOUND_UNKNOWN:
            Serial.println("Renderer: unknown sound ignored");
            break;
        case SOUND_NONE:
        default:
            break;
    }
}

static uint8_t s_led_rule = LED_RULE_OFF;

void renderer_set_led_rule(uint8_t rule) {
    s_led_rule = rule;
}

uint8_t renderer_led_rule() {
    return s_led_rule;
}

void renderer_apply(const ScreenPayload& p) {
    if (p.has_volume) {
        indication_set_volume(p.volume);
    }
    // Order volume -> brightness -> LED -> sound -> screen (docs/API.md "Payload format"):
    // the brightness is in place before the colour is written.
    if (p.has_brightness) {
        indication_set_brightness(p.brightness);
    }
    uint8_t r, g, b;
    // The LED rule follows the first tile of a tile frame (docs/API.md "Tile layouts").
    const bool tiles = payload_is_tiles(p);
    const bool has_dir = tiles ? (p.tiles[0].has && p.tiles[0].has_dir) : p.has_dir;
    const TickerDir dir = tiles ? p.tiles[0].dir : p.dir;
    if (ticker_led_decide(s_led_rule, p.has_led, p.r, p.g, p.b, has_dir, dir, &r, &g, &b)) {
        indication_led_rgb(r, g, b);
    }
    renderer_play_sound(p.sound, p.rtttl);

    if (tiles) {
        // Tiles win over the ticker and text looks. The frame (~270 B) is
        // static: the loop task's stack is tight (main.cpp) and this runs there.
        static TilesFrame s_frame;
        payload_tiles_frame(p, &s_frame);
        Serial.printf("Rendering tiles: %s, %u cells\n", tile_layout_str(s_frame.layout), (unsigned)s_frame.n);
        display_show_tiles(&s_frame, p.has_text ? p.title : nullptr);
        return;
    }
    bool ticker = payload_is_ticker(p);
    if (p.has_text || ticker) {
        Serial.printf("Rendering%s: %s - %s %s\n", ticker ? " ticker" : "", p.title, p.value, p.change);
        TickerFields t;
        memset(&t, 0, sizeof(t));
        t.ticker = ticker;
        if (ticker) {
            strlcpy(t.change, p.change, sizeof(t.change));
            t.dir = p.has_dir ? p.dir : 0;
            t.age_s = p.has_age ? p.age_s : 0;    // relative age counts from the receipt
            strlcpy(t.time, p.time, sizeof(t.time));
            strlcpy(t.short_label, p.short_label, sizeof(t.short_label));
            t.spark_n = ticker_spark_scale(p.spark, p.spark_n, t.spark, TICKER_SPARK_H);
        }
        display_show_content(p.title, p.value, &t);
    }
}

bool renderer_process_payload(const char* json, size_t len) {
    // ScreenPayload is ~1.7 KB; keep it off the (8 KB) loop task stack.
    ScreenPayload* p = (ScreenPayload*)malloc(sizeof(ScreenPayload));
    if (!p) {
        Serial.println("Renderer: out of memory");
        return false;
    }
    char err[96];
    PayloadResult r = payload_parse(json, len, p, err, sizeof(err));
    if (r != PAYLOAD_OK) {
        Serial.printf("Renderer: payload rejected (%s): %s\n", payload_result_str(r), err);
        free(p);
        return false;
    }
    if (err[0]) {
        Serial.printf("Renderer: warning: %s\n", err);
    }
    renderer_apply(*p);
    free(p);
    return true;
}

bool renderer_process_payload(const String& json_payload) {
    return renderer_process_payload(json_payload.c_str(), json_payload.length());
}
