#include "boot_stats.h"
#include <Arduino.h>
#include <esp_rom_crc.h>
#include <string.h>

#define BOOT_RTC_MAGIC 0x424F4F31u   // "BOO1": bump when the layout changes

struct BootRtc {
    uint32_t magic;
    uint32_t boots;
    uint32_t crashes;
    uint8_t  crash_reason;
    uint8_t  pad[3];
    uint32_t crc;
};
RTC_NOINIT_ATTR static BootRtc s_rtc;
static TaskHandle_t s_loop_task = nullptr;

static uint32_t rtc_crc() {
    return esp_rom_crc32_le(0, (const uint8_t*)&s_rtc, offsetof(BootRtc, crc));
}

static bool is_crash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

void boot_stats_init(esp_reset_reason_t reason) {
    s_loop_task = xTaskGetCurrentTaskHandle();
    // A power-on / EN reset leaves random RTC memory (the CRC catches it) and
    // starts a new count anyway; every other reset kind keeps the record.
    bool power_on = reason == ESP_RST_POWERON || reason == ESP_RST_EXT;
    if (power_on || s_rtc.magic != BOOT_RTC_MAGIC || s_rtc.crc != rtc_crc()) {
        memset(&s_rtc, 0, sizeof(s_rtc));
    }
    if (reason != ESP_RST_DEEPSLEEP) s_rtc.boots++;
    if (is_crash(reason)) {
        s_rtc.crashes++;
        s_rtc.crash_reason = (uint8_t)reason;
    }
    s_rtc.magic = BOOT_RTC_MAGIC;
    s_rtc.crc = rtc_crc();
}

void boot_stats_get(BootStats* out) {
    out->boots = s_rtc.boots;
    out->crashes = s_rtc.crashes;
    out->crash_reason = s_rtc.crash_reason;
    // portSTACK_TYPE is a byte on this port: the high-water mark is in bytes.
    out->loop_stack_min = s_loop_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_loop_task) * sizeof(StackType_t) : 0;
}
