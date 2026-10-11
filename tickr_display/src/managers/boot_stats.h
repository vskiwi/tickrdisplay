#pragma once
// Boot and crash counters for GET /api/status (docs/API.md "Status and
// configuration"): how often the device booted since it was last powered
// on, how many of those boots followed a crash (panic, a watchdog, a
// brownout) and which kind the last one was, plus the main task's stack
// high-water mark. The counters live in RTC slow memory (RTC_NOINIT_ATTR,
// magic + CRC) so that a crash reboot does not erase its own trace; only a
// power-on / EN-pin reset starts them from zero.
#include <stdint.h>
#include <esp_system.h>

struct BootStats {
    uint32_t boots;            // boots since power-on, deep-sleep wake-ups not counted
    uint32_t crashes;          // of those: panic / interrupt or task watchdog / brownout
    uint8_t  crash_reason;     // esp_reset_reason_t of the last crash, 0 = none
    uint32_t loop_stack_min;   // bytes of the main (loop) task stack never used so far
};

// First thing in setup(), on the loop task (records its handle for the stack figure).
void boot_stats_init(esp_reset_reason_t reason);

// Any task (GET /api/status runs on async_tcp).
void boot_stats_get(BootStats* out);
