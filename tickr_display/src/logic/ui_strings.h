#pragma once
// On-device strings and screen-state names - ONE table, English only
// (a second language is added here, nowhere else).
//
// Rules (docs/DEVICE_UI.md "Card texts and layout notes"): a screen carries an instruction, never a
// status word; the brand "TickrDisplay" appears on the splash only;
// an address appears only where the user has to type it (WAITING, SETUP,
// RECOVERY). Card lines are 18 pt / 12 pt; the fonts are Latin-1 only.
// Widths below are FreeSansBold pixel widths measured with the font tables
// (panel 296 px, text area 286 px).
#include <stdint.h>

// --- Splash / boot frames (docs/WEB_UI.md "Recovery mode") ----------------------
#define UI_BRAND                    "TickrDisplay"                     // splash only
// One action, no count: a reader of "3x" flips the switch three times
// before the counter has run and the device sees nothing.
// The 2nd/3rd frames carry the count.
#define UI_SPLASH_RECOVERY          "Recovery: switch off/on now"      // 9 pt
#define UI_RESTART_N_FMT            "Restart %u of %u"
#define UI_RESTART_HINT_1           "Switch off again within 20 s"
#define UI_RESTART_HINT_2           "to enter recovery mode"
#define UI_RECOVERY_USB_ONLY        "Recovery: USB only"
#define UI_RECOVERY_USB_HINT_1      "Connect USB power, then"
#define UI_RECOVERY_USB_HINT_2_FMT  "switch off/on %ux again"

// --- WAITING card (replaces "Online" / "Setup Required") -------------------
#define UI_WAITING_TITLE            "Ready"                            // 18 pt, 103 px
#define UI_WAITING_HINT             "choose content at"                // 12 pt, 206 px
#define UI_URL_FMT                  "http://%s/"                       // 12 pt, 220 px with a 13-char IP

// --- Access-point cards: set-up portal and recovery mode -------------------
#define UI_SETUP_TITLE              "Wi-Fi setup"                      // 18 pt, 189 px
#define UI_RECOVERY_TITLE           "Recovery mode"                    // 18 pt, 258 px
#define UI_AP_JOIN_FMT              "Join Wi-Fi: %s"                   // 12 pt, 273 px with "TickrDisplay"
#define UI_AP_OPEN_FMT              "open http://%s"                   // 12 pt, 277 px with 192.168.244.1
#define UI_RECOVERY_NOTE_STARTING   "Access point starting..."         // 9 pt note
#define UI_RECOVERY_NOTE_EXIT_FMT   "Exits after %lu min without clients" // 9 pt, 279 px

// --- Condition cards: glyph on top, 18 pt line, 12 pt line, centred -------
// Every line must fit 286 px in its font: "No Wi-Fi connection" (340 px) and
// "Updating firmware" (311 px) do not at 18 pt, hence the shorter titles.
#define UI_BATTERY_EMPTY            "Battery empty"                    // 18 pt, 233 px
#define UI_BATTERY_EMPTY_HINT       "connect USB power"                // 12 pt, 230 px
#define UI_OFFLINE_TITLE            "No Wi-Fi"                         // 18 pt, 142 px
#define UI_OFFLINE_AGE_MIN_FMT      "last update %u min ago"           // 12 pt, 262 px (2 digits)
#define UI_OFFLINE_AGE_H_FMT        "last update %u h ago"             // 12 pt, 234 px
#define UI_OTA_TITLE                "Updating"                         // 18 pt, 152 px
#define UI_OTA_HINT                 "keep the power on"                // 12 pt, 215 px
// The transition card. The second line is drawn only when the runtime
// mode switch will follow (docs/DEVICE_UI.md "Power-mode switch"): it is the promise the
// battery flow keeps, not a status word.
#define UI_POWER_BATTERY            "On battery"                       // 18 pt, 177 px
#define UI_POWER_BATTERY_HINT_FMT   "updates every %u min"             // 12 pt, 244 px (2 digits), 257 px (3)

// --- Badges -----------------------------------------------------------------
#define UI_BATT_PCT_FMT             "%u%%"                             // 9 pt next to the battery glyph

// --- Ticker frame (docs/TICKERS.md "What the screen shows"): the age line, 9 pt --
// Relative age of the quote (payload age_s + time since it arrived); the
// "stale" form once the age passed T_stale (docs/DEVICE_UI.md "E-ink refresh rules"). Under
// a minute the line reads "just now": "0 s ago" looks like a bug on a
// battery device whose frame is always fresh at wake.
#define UI_AGE_JUST_NOW             "just now"
#define UI_AGE_MIN_FMT              "%u min ago"
#define UI_AGE_H_FMT                "%u h ago"
#define UI_AGE_D_FMT                "%u d ago"
#define UI_STALE_MIN_FMT            "stale %u min"
#define UI_STALE_H_FMT              "stale %u h"
#define UI_STALE_D_FMT              "stale %u d"

// --- Screen state, GET /api/screen/state "state" (contract with the web) ---
// Additive only: new frames are appended, existing names never change.
enum ScreenState : uint8_t {
    SCREEN_BOOT = 0,     // splash / restart-N / USB-only frames
    SCREEN_SETUP,        // captive portal card
    SCREEN_WAITING,      // no content yet: "Ready - choose content at http://ip/"
    SCREEN_CONTENT,      // a payload (title/value) is on the panel
    SCREEN_PAIRING,      // pairing code or outcome frame
    SCREEN_RECOVERY,     // recovery-mode card
    SCREEN_OTA,          // OTA card: "Updating / keep the power on"
    SCREEN_IDENTIFY,     // the identify number
    SCREEN_OFFLINE,      // OFFLINE card: "No Wi-Fi / last update N min ago"
    SCREEN_BATTERY_EMPTY,// EMPTY card, drawn once before the 60-min sleeps
    SCREEN_POWER_BATTERY,// ON-BATTERY transition card
    SCREEN_STATE_COUNT
};
// "boot" | "setup" | "waiting" | "content" | "pairing" | "recovery" | "ota" |
// "identify" | "offline" | "battery_empty" | "power_to_battery"
const char* screen_state_str(ScreenState s);
