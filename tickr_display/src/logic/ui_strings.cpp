#include "ui_strings.h"

const char* screen_state_str(ScreenState s) {
    static const char* const kNames[SCREEN_STATE_COUNT] = {
        "boot", "setup", "waiting", "content", "pairing", "recovery", "ota", "identify",
        "offline", "battery_empty", "power_to_battery",
    };
    return s < SCREEN_STATE_COUNT ? kNames[s] : "unknown";
}
