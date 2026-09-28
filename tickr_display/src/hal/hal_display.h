#pragma once
#include <Arduino.h>
#include "../logic/status_policy.h"   // DisplayStatus (the badge snapshot)
#include "../logic/ui_strings.h"      // ScreenState
#include "../logic/device_state.h"    // ScreenCard (the condition cards)
#include "../logic/ticker.h"          // TickerFields (the ticker look)
// GxEPD2 is deliberately NOT included here: its headers carry a global
// `#pragma GCC diagnostic ignored "-Wunused-parameter"` that would silence
// warnings in every translation unit pulling this header. The panel driver
// object is private to hal_display.cpp.

// Badge snapshot (docs/DEVICE_UI.md "Screens"). main.cpp collects it
// once a second and stores it here; every *base* frame (content or the
// WAITING card) draws the badges from it into its bottom-right corner.
// Storing never refreshes; `folded` counts a change that will only appear
// with the next refresh (refreshes_skipped in /api/screen/state).
void display_set_status(const DisplayStatus& s, bool folded);
// One refresh of the base frame with the stored snapshot (partial under the
// refresh policy) - the badge
// policy's "at most one badge-only refresh per minute". Returns false (and
// draws nothing) while a service or temporary frame is up: the restore
// of that frame carries the badges anyway.
bool display_refresh_badges();
// Milliseconds since the last panel refresh of any kind.
uint32_t display_ms_since_refresh();

// initial_clear: let GxEPD2 do its "clear to white" full refresh first (cold
// boot with unknown controller RAM). Pass false after deep sleep (the panel
// still shows the last image) and when a boot frame is drawn right away -
// that frame is a full refresh itself, so the clear would only add a second one.
// rtc_trusted: the reset kind kept RTC memory (deep-sleep wake, software
// restart) - only then may the CRC-checked copy of the shown frame seed a
// partial refresh (docs/DEVICE_UI.md "E-ink refresh rules"); false = first frame full.
// battery: the battery flow (one frame per wake, the wake counter rules).
void display_init(bool initial_clear, bool rtc_trusted, bool battery);
void display_power(bool on);

// Content (a payload's title/value): stored and drawn with one refresh (kind by the refresh policy),
// badges included. Title may be "".
void display_show_message(const char* title, const char* message);
// Content with the ticker fields (docs/TICKERS.md "What the screen shows"): when t->ticker
// is set the frame is the ticker layout (change + triangle, fitted price,
// sparkline, age line); NULL or t->ticker == false is the Text look above.
// The fields are copied; one refresh (partial for the same layout kind).
void display_show_content(const char* title, const char* message, const TickerFields* t);
// T_stale for the ticker's age line (docs/DEVICE_UI.md "E-ink refresh rules": 3 x refresh interval,
// min 10 min - device_stale_ms()). Past it the line reads "stale N min".
void display_set_stale_ms(uint32_t ms);
// True exactly once when the shown ticker's age line crosses T_stale: the caller (the badge poll in main.cpp) folds it into the badge
// policy as BADGE_STALE, so the marker gets one refresh of its own under
// the one-per-minute rule. Re-armed by new content; a ticker with a
// pass-through `time` or a Text frame never fires.
bool display_stale_crossed();
// Base frame: the condition card when one is set (below), else the stored
// content, or the WAITING card ("Ready - choose content at http://<ip>/")
// when no payload has arrived on this boot. One refresh (kind by the refresh policy).
void display_show_base();
// Forget the stored content (the base frame becomes the WAITING card).
void display_clear_content();
// Condition cards (docs/DEVICE_UI.md "Screens"): OFFLINE, OTA, BATTERY EMPTY
// (and the ON-BATTERY transition). Setting one makes it the base frame -
// display_show_base(), the restore after a service frame and the badge
// refresh all draw it - until CARD_NONE is set or new content arrives
// (display_show_message() clears it; logic/device_state decides whether it
// comes back). Setting never refreshes; the caller does. `age_s` is the age
// of the content the panel still shows when none is stored (battery wake);
// DS_AGE_UNKNOWN = no age line. Ignored while content is stored. For
// CARD_POWER_BATTERY it is the refresh interval in minutes of the battery
// flow the device is about to restart into ("updates every N min"; 0 or
// DS_AGE_UNKNOWN = no second line).
void display_set_card(ScreenCard card, uint32_t age_s);
ScreenCard display_card();
// The kind of frame on the panel and the number of content frames drawn on
// this boot (the state machine's inputs).
ScreenState display_current_state();
uint32_t display_content_seq();
// Set-up portal card: "Wi-Fi setup" / "Join Wi-Fi: <ap>" / "open http://<ip>".
void display_show_setup_instruction(const char* ap_name, const char* ip_addr);

// Boot frames (docs/WEB_UI.md "Recovery mode", power-cycle recovery). One full refresh,
// no badges, stored content untouched.
enum BootFrame : uint8_t {
    BOOT_FRAME_SPLASH,        // "TickrDisplay <version>" + how to reach recovery
    BOOT_FRAME_RESTART_N,     // "Restart <position> of <of>" - series in progress
    BOOT_FRAME_USB_ONLY,      // series complete on battery power: recovery needs USB
};
void display_show_boot_frame(BootFrame kind, const char* version, unsigned position, unsigned of);
// Recovery-mode card: "Recovery mode" / "Join Wi-Fi: <ap>" / "open <url>"
// / `note` (9 pt, may be ""). A temporary frame like the identify number
// (display_temp_hold() / display_temp_end()).
void display_show_recovery(const char* ap_name, const char* url, const char* note);

// Pairing screen (docs/MULTI_DEVICE.md "Screen, state machine, limits"): one refresh (partial) with the
// 6-digit code (18 pt), the check word, the window and the device name;
// `from_group` (may be NULL/empty) is the group the device would leave.
// The stored title/message are kept so the content can be brought back
// with exactly one more refresh (display_temp_end() / display_loop()).
void display_show_pairing(const char* code, const char* chk, const char* name, const char* from_group, unsigned expires_s);
// Temporary frames (no code on them, so /api/screen.* serve them): the
// pairing outcome ("Paired: Shelf", "Pairing failed", "Pairing cancelled")
// and the identify number (POST /api/screen/identify). After
// drawing one, call display_temp_hold(ms); display_loop() (main loop)
// restores the base frame when the hold expires, display_temp_end() at once.
// New content, a pairing screen or display_show_base() replace them.
void display_show_pairing_result(const char* big, const char* detail);
void display_show_identify(unsigned n, const char* name);
void display_temp_hold(uint32_t ms);
// Turns the frame currently on the panel (e.g. a boot frame) into a
// temporary one with the given hold - test builds use it to keep a boot
// frame readable through /api/screen.* after the network is up.
void display_temp_adopt(uint32_t ms);
void display_temp_end();
bool display_temp_active();
void display_loop();
// True between display_show_pairing() and the next base redraw. The
// screen API hides the frame while this is set (the code must not leak).
bool display_overlay_active();

// Controller deep sleep + rail off + data lines released. Call before
// power_deep_sleep(). After this the panel needs display_init() again.
void display_prepare_sleep();

// Shadow framebuffer (docs/API.md "Screen"): 296x128, 1 bpp, rows
// top-down, 37 bytes per row, MSB first, 1 = black - exactly what the panel
// shows after the last refresh. The buffer is static and lives for the whole
// uptime; it may be read from another task (a reader can observe a frame
// mid-redraw - compare display_render_seq() before and after if that matters).
const uint8_t* display_frame_buffer();
// Incremented on every panel refresh.
uint32_t display_render_seq();

struct DisplayState {
    const char*   title;
    const char*   value;
    ScreenState   state;             // what kind of frame is on the panel
    uint32_t      render_seq;
    uint32_t      rendered_at_s;     // uptime seconds of the last refresh
    uint32_t      refreshes_full;
    uint32_t      refreshes_partial; // whole-frame differential refreshes
    uint32_t      refreshes_skipped; // badge changes folded into a later refresh + identical frames not drawn
    uint32_t      last_full_s;       // uptime seconds of the last full refresh (0 = none on this boot)
    uint8_t       partials_since_full; // the forced-full counter (RTC, survives sleep)
    bool          has_content;       // a payload was shown on this boot
    uint32_t      content_age_s;     // seconds since it was shown ("stale_s")
    ScreenCard    card;              // condition card set as the base frame ("card")
    DisplayStatus status;
    const TickerFields* ticker;      // the ticker fields when the content is a ticker frame, else NULL ("layout")
    uint32_t      ticker_age_s;      // total age of the quote: payload age_s + time on the panel ("age_s")
};
void display_get_state(DisplayState* out);
