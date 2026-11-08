#include "command_queue.h"
#include "../log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include "renderer.h"
#include "payload.h"
#include "../hal/hal_indication.h"
#include "../hal/hal_display.h"
#include "screen_bmp.h"

static QueueHandle_t s_queue = nullptr;
static volatile uint32_t s_dropped = 0;

void command_queue_init() {
    if (s_queue) return;
    s_queue = xQueueCreate(COMMAND_QUEUE_DEPTH, sizeof(Command));
    if (!s_queue) {
        Serial.println("CommandQueue: xQueueCreate failed!");
    }
}

bool command_queue_push(CommandType type, CommandSource src,
                        const char* str, size_t len, size_t max_len,
                        uint8_t a, uint8_t b, uint8_t c) {
    if (!s_queue) return false;
    if (str && len > max_len) return false;
    if (len > 0xFFFF) return false;

    Command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.src = src;
    cmd.a = a; cmd.b = b; cmd.c = c;

    if (str) {
        cmd.str = (char*)malloc(len + 1);
        if (!cmd.str) return false;
        memcpy(cmd.str, str, len);
        cmd.str[len] = '\0';
        cmd.len = (uint16_t)len;
    }

    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        free(cmd.str);
        s_dropped++;
        return false;
    }
    return true;
}

bool command_queue_pop(Command* out) {
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

void command_free(Command* cmd) {
    if (!cmd) return;
    free(cmd->str);
    cmd->str = nullptr;
    cmd->len = 0;
}

unsigned command_queue_depth() {
    if (!s_queue) return 0;
    return (unsigned)uxQueueMessagesWaiting(s_queue);
}

uint32_t command_queue_dropped() {
    return s_dropped;
}

const char* command_source_str(CommandSource src) {
    switch (src) {
        case SRC_HTTP:     return "http";
        case SRC_MQTT:     return "mqtt";
        case SRC_INTERNAL: return "internal";
    }
    return "?";
}

void command_execute(Command* cmd) {
    if (!cmd) return;
    const char* s = cmd->str ? cmd->str : "";

    switch (cmd->type) {
        case CMD_NONE:
            break;

        case CMD_PAYLOAD:
            LOGV("[%s] payload: %s\n", command_source_str(cmd->src), s);
            renderer_process_payload(s, cmd->len);
            break;

        // The test endpoints write the temporary test layer of hal_indication
        // (10 s, then the derived colour is back); the base colour is derived
        // from the content only (docs/DEVICE_UI.md "LED and sound").
        case CMD_LED_RGB:
            indication_test_rgb(cmd->a, cmd->b, cmd->c);
            break;
        case CMD_LED_SET_R:
            indication_test_channel(0, cmd->a);
            break;
        case CMD_LED_SET_G:
            indication_test_channel(1, cmd->a);
            break;
        case CMD_LED_SET_B:
            indication_test_channel(2, cmd->a);
            break;
        case CMD_LED_BLINK:
            indication_test_rgb(cmd->a ? 255 : 0, cmd->b ? 255 : 0, cmd->c ? 255 : 0, 500);
            break;

        case CMD_VOLUME:
            indication_set_volume(cmd->a);
            break;

        case CMD_PLAY_RTTTL:
            indication_play_rtttl(s);
            break;

        case CMD_BEEP:
            renderer_play_sound((PayloadSound)cmd->a, nullptr);
            break;

        case CMD_SCREEN_STATUS:
            // Test hook (/api/test/screen_refresh): the text as content
            // without a title - the brand is on the splash only.
            display_show_message("", s);
            break;

        case CMD_SCREEN_RAW: {
            const bool has_led = cmd->len == SCREEN_FRAME_LEN + 3;
            if (cmd->len != SCREEN_FRAME_LEN && !has_led) break;
            const uint8_t* px = (const uint8_t*)cmd->str;
            const uint8_t* rgb = px + SCREEN_FRAME_LEN;
            LOGV("[%s] raw frame%s\n", command_source_str(cmd->src), has_led ? " + led" : "");
            renderer_apply_raw(px, has_led, has_led ? rgb[0] : 0, has_led ? rgb[1] : 0, has_led ? rgb[2] : 0);
            break;
        }

        case CMD_SCREEN_MESSAGE: {
            // "title\nmessage"
            const char* nl = strchr(s, '\n');
            if (nl) {
                char title[PAYLOAD_TITLE_MAX];
                size_t tl = (size_t)(nl - s);
                if (tl >= sizeof(title)) tl = sizeof(title) - 1;
                memcpy(title, s, tl);
                title[tl] = '\0';
                display_show_message(title, nl + 1);
            } else {
                display_show_message("", s);
            }
            break;
        }
    }
    command_free(cmd);
}
