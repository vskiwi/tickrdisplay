// E-paper HAL (SSD1680 2.9" 296x128 via GxEPD2).
//
// Refresh policy v2 - partial-first (docs/DEVICE_UI.md "E-ink refresh rules")
// -----------------------------------------------------------------------
// * Every frame request renders into the shadow canvas and asks
//   logic/refresh_policy once (present()): NONE when the rendered frame is
//   byte-identical to the shown one, DEFER when a content partial comes
//   less than RP_PARTIAL_MIN_MS after the previous partial (display_loop()
//   draws it then), PARTIAL = a whole-frame differential refresh (SSD1680
//   mode 2, 0.75 s, no inversion flash) or FULL (4.1 s, cleans ghosting).
//   Content of the same layout kind, badges / stale, service frames
//   (identify, pairing, the transitional ON-BATTERY card) and the base
//   frame back after any card go partial; condition cards, layout-kind or
//   ticker-source changes, boot / SETUP / RECOVERY frames and the forced
//   fulls (8 partials, 60 min, 6 battery wakes, 24 h hygiene) go full.
// * The differential needs controller RAM 0x26 = the image on the panel.
//   That RAM is lost when the rail is cut in deep sleep and unknown after a
//   crash, so a copy of the shown frame lives in RTC slow memory
//   (rtc_prev: 4 736 B + counters + CRC-32; survives deep sleep and
//   ESP.restart(), not a power cycle) and is rewritten into 0x26 before
//   every partial (nextPageToPrevious(), ~10 ms). The copy is trusted only
//   when its CRC matches AND main.cpp vouches for the reset kind (deep-sleep
//   wake or software restart); otherwise the first frame is a full and the
//   counters restart. There is no windowed status-bar path: the
//   differential is always the whole frame.
// * The status facts are BADGES drawn into the bottom-right corner
//   of every base frame (content or WAITING) by the same drawing pass:
//   crossed Wi-Fi arcs when the link is down, a lightning bolt on USB, a
//   battery outline with fill + "NN%" on battery (outline only when the
//   level is unknown), nothing when the power source is unknown. A badge
//   change costs nothing while the content refreshes anyway; the policy in
//   logic/status_policy.h decides when a badge alone may cause one full
//   refresh (debounced, at most one per minute) - main.cpp runs it.
// * CONDITION CARDS (docs/DEVICE_UI.md "Screens"): OFFLINE, OTA, BATTERY EMPTY are
//   drawn as the base frame while set (display_set_card): glyph on top,
//   18 pt + 12 pt lines, no badges. logic/device_state decides when they
//   come and go; main.cpp sets the card and refreshes once per change.
// * Full-refresh hygiene: when nothing refreshed the panel for
//   FULL_REFRESH_MAX_AGE_MS, display_loop() redraws the base frame once.
// * Battery mode: each wake-up is exactly one refresh - partial when the
//   RTC copy is valid and fewer than RP_FORCE_FULL_BATT wakes were drawn
//   partial, else full (init(initial=false) avoids the extra "clear to
//   white" flash GxEPD2 does on initial=true).
// * After every drawing the controller is put into deep sleep
//   (display.hibernate()). GxEPD2 wakes it with a hardware reset on RST
//   (PIN_EPD_RST) on the next write.
//
// Shadow framebuffer (docs/API.md "Screen")
// ---------------------------------------------
// All drawing goes into a GFXcanvas1 of 296x128 in landscape orientation
// (static buffer s_frame, 4 736 B: rows top-down, 37 B/row, MSB first,
// 1 = black). The canvas is then blitted into GxEPD2 with drawBitmap(). By
// construction the canvas is exactly what the panel shows - unlike GxEPD2's
// private buffer, which only holds the last window and is cleared between
// pages - and it is served as-is by GET /api/screen/raw and /api/screen.bmp.
// Readers on the async task may see a frame mid-redraw (tearing); the
// render sequence changes on every blit, so the next poll picks up the
// settled frame.
//
// Fonts: FreeSansBold 9 / 12 / 18 pt (Adafruit GFX). Cards use 18 and 12 pt
// only (readable from two metres and in the panel thumbnail, docs/DEVICE_UI.md "The panel shows the same frame");
// 9 pt is for content titles, the recovery note and the badge percentage;
// the built-in 5x7 font is not used any more.
#include "hal_display.h"
#include "../log.h"
#include "hal_pins.h"
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <esp_rom_crc.h>
#include "../logic/screen_bmp.h"
#include "../logic/refresh_policy.h"

#define FULL_REFRESH_MAX_AGE_MS (24UL * 60UL * 60UL * 1000UL)
#define TEXT_MARGIN_X           5
#define TEXT_MAX_W              (296 - 2 * TEXT_MARGIN_X)
// Badge zone: the bottom BADGE_H rows, right-aligned; the content value is
// laid out above it.
#define BADGE_H                 16
// Name badge of a ticker frame (docs/TICKERS.md "What the screen shows"): the
// short name white on a black field in the top-left NAME_BADGE_H rows, NAME_BADGE_PAD
// px of padding, at most NAME_BADGE_MAX_W wide (the change needs the rest of the row).
#define NAME_BADGE_H            30
#define NAME_BADGE_PAD          6
#define NAME_BADGE_MAX_W        150

// Canvas colours: GFXcanvas1 sets the bit for any non-zero colour, so black
// (= bit set, drawn by drawBitmap in GxEPD_BLACK) is 1 here - the inverse of
// the GxEPD_BLACK/GxEPD_WHITE constants used on the panel object.
#define CV_BLACK 1
#define CV_WHITE 0

// SSD1680 2.9" 296x128
// Generic SSD1680 class (GDEM029T94 is common for SSD1680). Private to this file.
static GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(GxEPD2_290_T94_V2(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// Shadow framebuffer in static DRAM (no heap) wrapped by a GFXcanvas1 that
// does not own its buffer.
static uint8_t s_frame[SCREEN_FRAME_LEN];
class FrameCanvas : public GFXcanvas1 {
public:
    FrameCanvas() : GFXcanvas1(SCREEN_W, SCREEN_H, false) {
        buffer = s_frame;
        buffer_owned = false;
    }
};
static FrameCanvas canvas;

// Stored content (a payload's title/value) for the base frame.
static char _last_title[64] = "";
static char _last_message[128] = "";
static TickerFields _tk = {};                // ticker fields of the content; _tk.ticker selects the layout
static GridFrame _grid = {};                 // the cells of a grid frame (docs/TICKERS.md "Several tickers on one panel: the 2x2 grid")
static bool      _grid_on = false;           // the content is the grid (_tk.ticker is set too: age line, stale)
static uint32_t _stale_ms = DS_T_STALE_MIN_MS; // T_stale for the age line (display_set_stale_ms)
static uint32_t _content_at_ms = 0;          // when the content was last shown (stale_s)
static bool     _stale_fired = false;        // the stale crossing of this content already asked for its refresh (display_stale_crossed)
static DisplayStatus _status;                // badge snapshot
static char _last_ip[16] = "";               // last address seen - the WAITING card keeps it
static ScreenState _state = SCREEN_BOOT;
static ScreenCard _card = CARD_NONE;         // condition card as the base frame
static uint32_t _card_age_s = DS_AGE_UNKNOWN; // age of the shown content when none is stored
static uint32_t _content_seq = 0;            // content frames drawn on this boot

// Refresh bookkeeping
static bool     _initialized = false;
static bool     _battery = false;            // battery flow: one frame per wake (refresh_policy's wake counter)
static uint32_t _last_full_ms = 0;
static uint32_t _last_partial_ms = 0;        // valid while _had_partial (RP_PARTIAL_MIN_MS spacing)
static bool     _had_partial = false;
static bool     _deferred = false;           // a content frame waits for the spacing (display_loop draws it)
static volatile uint32_t _render_seq = 0;
static uint32_t _rendered_at_ms = 0;
static uint32_t _refreshes_full = 0;
static uint32_t _refreshes_partial = 0;
static uint32_t _refreshes_skipped = 0;      // badge changes folded into a later refresh + identical frames not drawn

// The frame on the panel (docs/DEVICE_UI.md "E-ink refresh rules"): the copy the differential
// refresh is computed against, in RTC slow memory so a battery wake and a
// software restart can resume with a partial. RTC_NOINIT_ATTR, not
// RTC_DATA_ATTR: `.rtc.data` is a loadable segment (4.7 KB of image, and
// the bootloader reloads it on every reset that is not a deep-sleep wake -
// esp_attr.h), `.rtc_noinit` costs no flash and is never initialized, so
// the copy also survives ESP.restart(). CRC-32 over everything before
// `crc`; a torn copy (crash mid-write) or random memory (power-on) fails it.
// Layout kind of the last BASE content frame: text / ticker / WAITING / the
// 2x2 grid - the policy's layout_changed (cards and service frames leave it
// alone, so the content back after one compares against its own kind).
enum ShownKind : uint8_t { SHOWN_NONE = 0, SHOWN_TEXT, SHOWN_TICKER, SHOWN_WAITING, SHOWN_GRID };
struct RtcFrame {
    uint8_t  px[SCREEN_FRAME_LEN];
    uint8_t  partials_since_full;
    uint8_t  base_kind;              // ShownKind
    uint8_t  condition_card;         // the shown frame is a condition card (double-full input)
    uint8_t  pad;
    uint32_t base_title;             // CRC of the ticker title: another source is a "layout change"
    uint32_t crc;
};
RTC_NOINIT_ATTR static RtcFrame rtc_prev;
static bool _prev_valid = false;             // rtc_prev is what the panel shows (CRC + reset kind, or drawn on this boot)

static uint32_t prev_crc() {
    return esp_rom_crc32_le(0, (const uint8_t*)&rtc_prev, offsetof(RtcFrame, crc));
}

static bool _overlay = false;   // pairing screen on the panel (code must not leak)
// Temporary frame (pairing outcome, identify number, recovery card): shown
// instead of the base frame, restored by display_loop() when the hold
// expires or at once by display_temp_end(). Any new content or a pairing
// screen replaces it.
static bool     _temp = false;
static uint32_t _temp_until_ms = 0;

void display_set_status(const DisplayStatus& s, bool folded) {
    _status = s;
    if (s.ip[0]) strlcpy(_last_ip, s.ip, sizeof(_last_ip));
    if (folded) _refreshes_skipped++;
}

void display_power(bool on) {
    if (on) {
        digitalWrite(PIN_EPD_PWR, LOW); // ON
        delay(10);
    } else {
        digitalWrite(PIN_EPD_PWR, HIGH); // OFF
    }
}

void display_init(bool initial_clear, bool rtc_trusted, bool battery) {
    _battery = battery;
    // The copy of the shown frame: trusted only when the reset kind kept RTC
    // memory (main.cpp), its CRC matches and GxEPD2 is not about to clear
    // the panel to white (initial_clear). Anything else: first frame full,
    // counters from zero.
    _prev_valid = !initial_clear && rtc_trusted && prev_crc() == rtc_prev.crc;
    if (!_prev_valid) {
        rtc_prev.partials_since_full = 0;
        rtc_prev.base_kind = SHOWN_NONE;
        rtc_prev.condition_card = 0;
        rtc_prev.base_title = 0;
    }
    pinMode(PIN_EPD_PWR, OUTPUT);
    display_power(true);
    delay(100);

    pinMode(PIN_EPD_RST, OUTPUT);
    digitalWrite(PIN_EPD_RST, HIGH); delay(20);
    digitalWrite(PIN_EPD_RST, LOW);  delay(20);
    digitalWrite(PIN_EPD_RST, HIGH); delay(200);

    // ORDER IS CRITICAL: SPI.begin(...) with our pins MUST run before
    // display.init(). GxEPD2 calls SPI.begin() without arguments, which is a
    // no-op if the bus is already up but would otherwise claim the VSPI
    // defaults SCK=18 (our BUSY) and MISO=19 (our EPD_PWR). MISO itself is
    // unused (write-only panel), see PIN_SPI_MISO_DUMMY.
    SPI.begin(PIN_EPD_CLK, PIN_SPI_MISO_DUMMY, PIN_EPD_MOSI, PIN_EPD_CS);

    // initial=false skips GxEPD2's "clear to white" full refresh: after deep
    // sleep the panel still shows the last image, and a boot frame drawn right
    // after init is a full refresh of its own (see the header).
    LOGV("Display Init (initial=%d)...\n", initial_clear);
    display.init(115200, initial_clear, 2, false);
    display.setRotation(3);
    canvas.fillScreen(CV_WHITE);
    canvas.setTextColor(CV_BLACK);
    canvas.setTextWrap(false);
    _initialized = true;
    LOGV("Display Init Done (previous frame %s, %u partials since full)\n",
         _prev_valid ? "valid" : "unknown", (unsigned)rtc_prev.partials_since_full);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// Selects the largest font from `fonts` (entry i at text size 2 when bit i
// of `x2_mask` is set, else 1) whose rendering of `text` fits max_w (and
// max_h when > 0); if even the smallest does not fit, the text is cut with
// "...". Leaves the font and size set on the canvas; returns the string to
// print (in buf).
static const char* fit_text(const char* text, const GFXfont* const* fonts, int nfonts,
                            int16_t max_w, int16_t max_h, uint8_t x2_mask, char* buf, size_t buflen) {
    int16_t bx, by; uint16_t bw, bh;
    for (int i = 0; i < nfonts; i++) {
        canvas.setFont(fonts[i]);
        canvas.setTextSize((x2_mask >> i) & 1 ? 2 : 1);
        canvas.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
        if ((int16_t)bw <= max_w && (max_h <= 0 || (int16_t)bh <= max_h)) return text;
    }
    // Smallest font is selected; truncate (buffer leaves room for "...").
    strlcpy(buf, text, buflen - 3);
    size_t len = strlen(buf);
    while (len > 1) {
        len--;
        strcpy(buf + len, "...");
        canvas.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
        if ((int16_t)bw <= max_w) break;
    }
    return buf;
}

// Prints `text` left-aligned at (x, baseline y) with the largest fitting font.
static void print_fitted(const char* text, int16_t x, int16_t y,
                         const GFXfont* const* fonts, int nfonts, int16_t max_w) {
    char buf[128 + 4];
    const char* s = fit_text(text, fonts, nfonts, max_w, 0, 0, buf, sizeof(buf));
    canvas.setCursor(x, y);
    canvas.print(s);
    canvas.setTextSize(1);
}

// Prints `text` centred horizontally in the panel and vertically between
// rows `top` and `bottom`, with the largest font that fits both ways.
static void print_centered_fitted(const char* text, int16_t top, int16_t bottom,
                                  const GFXfont* const* fonts, int nfonts, uint8_t x2_mask) {
    char buf[128 + 4];
    const char* s = fit_text(text, fonts, nfonts, TEXT_MAX_W, bottom - top, x2_mask, buf, sizeof(buf));
    int16_t bx, by; uint16_t bw, bh;
    canvas.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    canvas.setCursor((SCREEN_W - (int16_t)bw) / 2 - bx, (top + bottom) / 2 - by - (int16_t)bh / 2);
    canvas.print(s);
    canvas.setTextSize(1);
}

// Centred on the current font at baseline y.
static void text_centered(const char* s, int16_t y) {
    int16_t bx, by; uint16_t bw, bh;
    canvas.getTextBounds(s, 0, y, &bx, &by, &bw, &bh);
    canvas.setCursor((SCREEN_W - (int16_t)bw) / 2 - bx, y);
    canvas.print(s);
}

// ---------------------------------------------------------------------------
// Glyphs - primitives only (docs/DEVICE_UI.md "Card texts and layout notes"). Only GFX routines the image
// already links are used (fillCircle, fillRect, drawRect, drawLine, the fast
// lines): fillTriangle / drawCircleHelper / drawRoundRect would each cost
// 0.3-0.45 KB of library code (measured in the linker map). One routine
// per glyph draws the badge size and the card size (stroke 2 -> 4).
// ---------------------------------------------------------------------------

// Wi-Fi lost: dot at (cx, cy), three arcs of `stroke` px above it out to
// radius r = 6 * stroke within +-45 degrees (concentric discs, then the
// sides and the lower half erased), a slash across. Erases its own box
// (cx-r..cx+r, cy-r..cy+1), so place it on white. Badge: stroke 2 (25x14).
static void draw_wifi_lost(int16_t cx, int16_t cy, int16_t stroke) {
    const int16_t r = 6 * stroke;
    for (int16_t i = 0, k = r; k > 0; i++, k -= stroke) canvas.fillCircle(cx, cy, k, i & 1 ? CV_WHITE : CV_BLACK);
    canvas.fillRect(cx - r, cy, 2 * r + 1, r + 1, CV_WHITE);       // lower half incl. the equator
    for (int16_t d = 1; d <= r; d++) {                             // side wedges beyond 45 degrees
        canvas.drawFastHLine(cx - r, cy - d, r - d, CV_WHITE);
        canvas.drawFastHLine(cx + d + 1, cy - d, r - d, CV_WHITE);
    }
    canvas.fillCircle(cx, cy, stroke / 2, CV_BLACK);
    for (int16_t s = 0; s < stroke; s++) canvas.drawLine(cx - r * 5 / 6 + s, cy - r, cx + r / 2 + s, cy + 1, CV_BLACK);
}

// Lightning bolt in a 9x15 box at (x, y): the USB / charging mark. Two
// slanted strokes (>= 2 px) drawn as rows.
static void draw_bolt(int16_t x, int16_t y) {
    for (int16_t i = 0; i < 9; i++) {
        int16_t w = i * 6 / 8;
        canvas.drawFastHLine(x + 7 - w, y + i, w + 2, CV_BLACK);          // upper: apex top right
        canvas.drawFastHLine(x + 2, y + 6 + i, 8 - w, CV_BLACK);          // lower: tip bottom left
    }
}

// Battery w x h at (x, y) with a `stroke` px outline and a nub on the right;
// the fill is inset 2 x stroke and proportional to pct; pct < 0 = level
// unknown (outline only); `low` adds an exclamation mark. Badge: 20x10,
// stroke 1. Card: 80x40, stroke 4.
static void draw_battery(int16_t x, int16_t y, int16_t w, int16_t h, int16_t stroke, int pct, bool low) {
    canvas.fillRect(x, y, w, h, CV_BLACK);
    canvas.fillRect(x + stroke, y + stroke, w - 2 * stroke, h - 2 * stroke, CV_WHITE);
    canvas.fillRect(x + w, y + h * 3 / 10, 2 * stroke, h * 4 / 10, CV_BLACK);
    if (pct >= 0) canvas.fillRect(x + 2 * stroke, y + 2 * stroke, constrain(pct, 0, 100) * (w - 4 * stroke) / 100, h - 4 * stroke, CV_BLACK);
    if (low) {                                                     // "!" in the empty part
        int16_t bx = x + w / 2 - stroke;
        canvas.fillRect(bx, y + 2 * stroke, 2 * stroke, h / 2 - stroke, CV_BLACK);
        canvas.fillRect(bx, y + h - 3 * stroke, 2 * stroke, stroke, CV_BLACK);
    }
}

// Update arrow for the OTA card: a 6 px ring of radius 22 around (cx, cy)
// with a gap in the upper right and an arrowhead pointing into the gap.
static void draw_update_arrow(int16_t cx, int16_t cy) {
    canvas.fillCircle(cx, cy, 22, CV_BLACK);
    canvas.fillCircle(cx, cy, 16, CV_WHITE);
    canvas.fillRect(cx + 1, cy - 24, 24, 17, CV_WHITE);            // the gap
    for (int16_t d = -11; d <= 11; d++) canvas.drawFastHLine(cx, cy - 19 + d, 12 - abs(d), CV_BLACK);
}

// Empty panel: a 40x28 frame with a 3 px stroke and a short stand (WAITING).
static void draw_panel_icon(int16_t x, int16_t y) {
    canvas.fillRect(x, y, 40, 28, CV_BLACK);
    canvas.fillRect(x + 3, y + 3, 34, 22, CV_WHITE);
    canvas.fillRect(x + 14, y + 29, 12, 2, CV_BLACK);
}

// Badges in the bottom-right corner, right-aligned: [Wi-Fi lost] [NN% battery | bolt].
// Returns the x where the badges begin (the grid's age line stops before it).
static int16_t draw_badges() {
    int16_t right = SCREEN_W - TEXT_MARGIN_X;
    const int16_t y0 = SCREEN_H - BADGE_H;
    if (_status.power == BADGE_POWER_USB) {
        draw_bolt(right - 10, y0);
        right -= 10 + 8;
    } else if (_status.power == BADGE_POWER_BATTERY) {
        right -= 22;
        draw_battery(right, y0 + 3, 20, 10, 1, _status.batt_known ? (int)_status.batt_pct : -1,
                     device_battery_low(_status.power, _status.batt_known, _status.batt_pct));
        if (_status.batt_known) {
            char pct[8];
            snprintf(pct, sizeof(pct), UI_BATT_PCT_FMT, (unsigned)_status.batt_pct);
            canvas.setFont(&FreeSansBold9pt7b);
            int16_t bx, by; uint16_t bw, bh;
            canvas.getTextBounds(pct, 0, 0, &bx, &by, &bw, &bh);
            right -= 4 + (int16_t)bw;
            canvas.setCursor(right - bx, y0 + 14);
            canvas.print(pct);
        }
        right -= 8;
    }
    if (!_status.wifi_connected) { draw_wifi_lost(right - 13, y0 + 12, 2); right -= 26 + 6; }
    return right;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Change triangle in an 11x6 box with its top-left at (x, y): up / down as
// rows of growing width, flat as a 2 px dash (the fonts have
// no arrow glyphs and fillTriangle is not linked).
static void draw_dir(int16_t x, int16_t y, int8_t dir) {
    if (dir == 0) { canvas.fillRect(x, y + 2, 11, 2, CV_BLACK); return; }
    for (int16_t i = 0; i < 6; i++) {
        int16_t row = dir > 0 ? i : 5 - i;
        canvas.drawFastHLine(x + 5 - i, y + row, 2 * i + 1, CV_BLACK);
    }
}

// Sparkline: `n` points (rows 0 = bottom .. h-1 = top) as a 2 px polyline
// across a w x h box at (x, y) (docs/TICKERS.md "Sparkline history": 200 x 24, <= 48 points).
static void draw_sparkline(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t* pts, uint8_t n) {
    if (n < 2) return;
    int16_t px = 0, py = 0;
    for (uint8_t i = 0; i < n; i++) {
        int16_t cx = x + (int16_t)((int32_t)i * (w - 1) / (n - 1));
        int16_t cy = y + (h - 1) - pts[i];
        if (i) {
            canvas.drawLine(px, py, cx, cy, CV_BLACK);
            canvas.drawLine(px, py - 1, cx, cy - 1, CV_BLACK);
        }
        px = cx; py = cy;
    }
}

// Total age of the shown quote: the payload's age_s plus the time on the panel.
static uint32_t ticker_total_age_s() {
    uint32_t elapsed = _content_at_ms ? (millis() - _content_at_ms) / 1000 : 0;
    return (_tk.age_s == TICKER_AGE_UNKNOWN ? 0 : _tk.age_s) + elapsed;
}

// The short name of a ticker white on a black field at the top left: 18 pt,
// 12 pt or 9 pt - the largest that keeps the field within NAME_BADGE_MAX_W -
// centred in the field's height. Returns the field's width.
static int16_t draw_name_badge(const char* text) {
    static const GFXfont* const kBadgeFonts[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    char buf[TICKER_SHORT_MAX + 4];
    const char* s = fit_text(text, kBadgeFonts, 3, NAME_BADGE_MAX_W - 2 * NAME_BADGE_PAD, 0, 0, buf, sizeof(buf));
    int16_t bx, by; uint16_t bw, bh;
    canvas.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    int16_t w = (int16_t)bw + 2 * NAME_BADGE_PAD;
    canvas.fillRect(0, 0, w, NAME_BADGE_H, CV_BLACK);
    canvas.setTextColor(CV_WHITE);
    canvas.setCursor(NAME_BADGE_PAD - bx, (NAME_BADGE_H - (int16_t)bh) / 2 - by);
    canvas.print(s);
    canvas.setTextColor(CV_BLACK);
    return w;
}

// Content: title 9 pt at the top left, value centred in the rest of the
// height (18 pt doubled -> 18 -> 12 -> 9 pt), badges below (docs/DEVICE_UI.md "Screens").
// Ticker (docs/TICKERS.md "What the screen shows"): the change with its triangle top right, the
// price centred in rows 18-92, the sparkline bottom left (200 x 24), the age
// line (or the pass-through time) right-aligned above the badges. With a
// short name the top row is the name badge (rows 0-29), the change 12 pt
// right of it with the full label 9 pt in between only when it fits whole,
// and the price moves down to rows 32-92; without one the frame is as before.
static void draw_content() {
    static const GFXfont* const kTitleFonts[] = { &FreeSansBold9pt7b };
    static const GFXfont* const kValueFonts[] = { &FreeSansBold18pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    // The price tries 12 pt doubled before 18 pt single: "84 000.06" is 320 px
    // at 18 pt x2 but 250 px at 12 pt x2 - the "as large as fits" rule.
    static const GFXfont* const kPriceFonts[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    int16_t bx, by; uint16_t bw, bh;
    int16_t title_w = TEXT_MAX_W;
    int16_t top = 0, bottom = SCREEN_H - BADGE_H;
    const bool badge = _tk.ticker && _tk.short_label[0];
    if (badge) {
        top = 32;
        bottom = 92;
        int16_t badge_w = draw_name_badge(_tk.short_label);
        int16_t room_right = SCREEN_W - TEXT_MARGIN_X;   // where the full label must end
        if (_tk.change[0]) {
            canvas.setFont(&FreeSansBold12pt7b);
            canvas.getTextBounds(_tk.change, 0, 0, &bx, &by, &bw, &bh);
            int16_t cx = SCREEN_W - TEXT_MARGIN_X - (int16_t)bw;
            canvas.setCursor(cx - bx, (NAME_BADGE_H - (int16_t)bh) / 2 - by);
            canvas.print(_tk.change);
            draw_dir(cx - 15, (NAME_BADGE_H - 6) / 2, _tk.dir);
            room_right = cx - 15 - 6;
        }
        if (_last_title[0]) {
            // the full label only when it fits whole between badge and change
            int16_t x = badge_w + 8;
            canvas.setFont(&FreeSansBold9pt7b);
            canvas.getTextBounds(_last_title, 0, 0, &bx, &by, &bw, &bh);
            if (x + (int16_t)bw <= room_right) {
                canvas.setCursor(x - bx, (NAME_BADGE_H - (int16_t)bh) / 2 - by);
                canvas.print(_last_title);
            }
        }
    } else if (_tk.ticker) {
        top = 18;
        bottom = 92;
        canvas.setFont(&FreeSansBold9pt7b);
        if (_tk.change[0]) {
            canvas.getTextBounds(_tk.change, 0, 0, &bx, &by, &bw, &bh);
            int16_t cx = SCREEN_W - TEXT_MARGIN_X - (int16_t)bw;
            canvas.setCursor(cx - bx, 14);
            canvas.print(_tk.change);
            draw_dir(cx - 15, 5, _tk.dir);
            title_w = cx - 15 - 6 - TEXT_MARGIN_X;
        }
    }
    if (_tk.ticker) {
        canvas.setFont(&FreeSansBold9pt7b);
        char age[24];
        if (_tk.time[0]) strlcpy(age, _tk.time, sizeof(age));
        else {
            uint32_t a = ticker_total_age_s();
            ticker_age_line(a, a >= _stale_ms / 1000, age, sizeof(age));
        }
        // The sparkline (200 px from the left margin) stops 6 px before a long
        // age line ("stale 10 min", "as of 19:00") but never gets narrower than 120 px.
        int16_t spark_w = 200;
        if (age[0]) {
            canvas.getTextBounds(age, 0, 0, &bx, &by, &bw, &bh);
            int16_t age_x = SCREEN_W - TEXT_MARGIN_X - (int16_t)bw;
            canvas.setCursor(age_x - bx, 106);
            canvas.print(age);
            int16_t room = age_x - 6 - TEXT_MARGIN_X;
            if (room < spark_w) spark_w = room < 120 ? 120 : room;
        }
        draw_sparkline(TEXT_MARGIN_X, 100, spark_w, TICKER_SPARK_H, _tk.spark, _tk.spark_n);
    }
    if (_last_title[0] && !badge) {
        print_fitted(_last_title, TEXT_MARGIN_X, 14, kTitleFonts, 1, title_w);
        if (!top) top = 20;
    }
    if (_last_message[0]) {
        if (_tk.ticker) {
            // A price with an integer part of 1 000 or more drops its fraction
            // when that is what keeps the largest size: "84 014.90" is 300 px
            // at 18 pt x2 and would fall to 12 pt x2, "84 015" fits (docs/TICKERS.md
            // "What the screen shows"). Only the first step is tried both ways -
            // on the smaller steps the whole string stays. fit_text returns
            // the text itself when it fits without truncation.
            char buf[128 + 4], trimmed[32];
            const char* price = _last_message;
            if (fit_text(price, kPriceFonts, 1, TEXT_MAX_W, bottom - top, 0x1, buf, sizeof(buf)) != price
                && ticker_price_trim_round(price, trimmed, sizeof(trimmed))
                && fit_text(trimmed, kPriceFonts, 1, TEXT_MAX_W, bottom - top, 0x1, buf, sizeof(buf)) == trimmed)
                price = trimmed;
            print_centered_fitted(price, top, bottom, kPriceFonts, 5, 0x3);
        }
        else print_centered_fitted(_last_message, top, bottom, kValueFonts, 4, 0x1);
    }
}

// ---------------------------------------------------------------------------
// The 2x2 grid (docs/TICKERS.md "Several tickers on one panel: the 2x2 grid"):
// cells of GRID_CELL_W x GRID_CELL_H with 1 px separators; in a cell row A is
// the name badge (12 pt white on black, 9 pt when the field would pass
// GRID_BADGE_MAX_W) with the change 9 pt and its triangle right-aligned -
// or "?" for a source that failed this cycle - row B the price right-aligned
// (18 pt whole -> 18 pt without the fraction -> 12 -> 9, grid_fit_price),
// row C empty. The last cell carries the age line and the badges (rows
// 112-127, below its price band); it shows a ticker only with four sources.
// ---------------------------------------------------------------------------
#define GRID_BADGE_H      20
#define GRID_BADGE_PAD    4
#define GRID_BADGE_MAX_W  70
#define GRID_CELL_PAD     4
// Price band of a cell: rows GRID_PRICE_TOP .. +GRID_PRICE_H-1 (18 pt digits are
// 25 px tall); in the last cell it ends on panel row 110, a row above the age
// line and the badges (rows 112-127).
#define GRID_PRICE_TOP    21
#define GRID_PRICE_H      25

// Width of `text` in the font of step 0 / 1 / 2 = 18 / 12 / 9 pt (grid_fit_price's measure).
static int16_t grid_measure(const char* text, uint8_t font_step, void*) {
    static const GFXfont* const kF[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    canvas.setFont(kF[font_step < 3 ? font_step : 2]);
    int16_t bx, by; uint16_t bw, bh;
    canvas.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    return (int16_t)bw;
}

static void draw_grid_cell(int16_t x, int16_t y, const GridCell& c) {
    static const GFXfont* const kBadge[] = { &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    int16_t bx, by; uint16_t bw, bh;
    const int16_t right = x + GRID_CELL_W - 1 - GRID_CELL_PAD;
    // row A: the badge ...
    char buf[TICKER_SHORT_MAX + 4];
    const char* s = fit_text(c.short_label, kBadge, 2, GRID_BADGE_MAX_W - 2 * GRID_BADGE_PAD, 0, 0, buf, sizeof(buf));
    canvas.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    canvas.fillRect(x, y, (int16_t)bw + 2 * GRID_BADGE_PAD, GRID_BADGE_H, CV_BLACK);
    canvas.setTextColor(CV_WHITE);
    canvas.setCursor(x + GRID_BADGE_PAD - bx, y + (GRID_BADGE_H - (int16_t)bh) / 2 - by);
    canvas.print(s);
    canvas.setTextColor(CV_BLACK);
    // ... and the change, or "?" when this cycle missed the source
    if (!c.ok) {
        canvas.setFont(&FreeSansBold12pt7b);
        canvas.getTextBounds("?", 0, 0, &bx, &by, &bw, &bh);
        canvas.setCursor(right - (int16_t)bw - bx, y + (GRID_BADGE_H - (int16_t)bh) / 2 - by);
        canvas.print("?");
    } else if (c.change[0]) {
        canvas.setFont(&FreeSansBold9pt7b);
        canvas.getTextBounds(c.change, 0, 0, &bx, &by, &bw, &bh);
        int16_t cx = right - (int16_t)bw;
        canvas.setCursor(cx - bx, y + (GRID_BADGE_H - (int16_t)bh) / 2 - by);
        canvas.print(c.change);
        draw_dir(cx - 15, y + (GRID_BADGE_H - 6) / 2, c.dir);
    }
    // row B: the price, centred in the price band
    if (c.price[0]) {
        char trimmed[GRID_PRICE_MAX];
        const char* p;
        uint8_t step = grid_fit_price(c.price, GRID_CELL_W - 2 * GRID_CELL_PAD, grid_measure, nullptr, trimmed, sizeof(trimmed), &p);
        grid_measure(p, step <= 1 ? 0 : step - 1, nullptr);   // selects the step's font
        canvas.getTextBounds(p, 0, 0, &bx, &by, &bw, &bh);
        canvas.setCursor(right - (int16_t)bw - bx, y + GRID_PRICE_TOP + (GRID_PRICE_H - (int16_t)bh) / 2 - by);
        canvas.print(p);
    }
}

// The grid frame; `badges_x` = where the badges begin (the age line stops 6 px before it).
static void draw_grid(int16_t badges_x) {
    static const GFXfont* const kSmall[] = { &FreeSansBold9pt7b };
    canvas.drawFastVLine(GRID_CELL_W, 0, SCREEN_H, CV_BLACK);
    canvas.drawFastHLine(0, GRID_CELL_H, SCREEN_W, CV_BLACK);
    const uint8_t cells = grid_ticker_cells(_grid.n);
    for (uint8_t i = 0; i < cells; i++) {
        int16_t x, y;
        grid_cell_origin(i, &x, &y);
        draw_grid_cell(x, y, _grid.cells[i]);
    }
    char age[24];
    uint32_t a = ticker_total_age_s();
    ticker_age_line(a, a >= _stale_ms / 1000, age, sizeof(age));
    if (age[0]) {
        const int16_t x = GRID_CELL_W + 1 + GRID_CELL_PAD;
        print_fitted(age, x, SCREEN_H - 3, kSmall, 1, badges_x - 6 - x);
    }
}

// WAITING card: panel icon, "Ready" / "choose content at" / "http://<ip>/".
static void draw_waiting() {
    static const GFXfont* const kBig[] = { &FreeSansBold18pt7b };
    static const GFXfont* const kMid[] = { &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    const int16_t x = 70, w = SCREEN_W - 70 - TEXT_MARGIN_X;
    draw_panel_icon(16, 48);
    print_fitted(UI_WAITING_TITLE, x, 45, kBig, 1, w);
    print_fitted(UI_WAITING_HINT, x, 77, kMid, 2, w);
    if (_last_ip[0]) {
        char url[32];
        snprintf(url, sizeof(url), UI_URL_FMT, _last_ip);
        print_fitted(url, x, 104, kMid, 2, w);
    }
}

// One full pass of the paged API over the whole panel with the given bitmap
// (NULL = white). GxEPD2 writes RAM 0x26 and 0x24, runs the OTP waveform
// (0x22 = 0xF7) and leaves 0x26 = 0x24 = the frame. Non-const pointer on
// purpose: the RAM overload of drawBitmap, the same one the partial path uses.
static void full_pass(uint8_t* bmp) {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        if (bmp) display.drawBitmap(0, 0, bmp, SCREEN_W, SCREEN_H, GxEPD_BLACK);
    } while (display.nextPage());
}

// Blits the canvas into the panel with the given kind (PARTIAL or FULL) and
// makes it the shown frame (rtc_prev + CRC, counters).
static void blit(RefreshKind kind) {
    display_power(true);
    if (kind == REFRESH_FULL) {
        // Optional clear to white before the full that ends a long-lived card.
        if (refresh_double_full(RP_DOUBLE_FULL_AFTER_CARD != 0, rtc_prev.condition_card != 0, kind)) full_pass(nullptr);
        full_pass(s_frame);
        _last_full_ms = millis();
        _refreshes_full++;
    } else {
        // Whole-frame differential: 0x26 = the shown frame from our copy (the
        // controller's own is lost in sleep / unknown after a crash), 0x24 = the
        // new frame, 0x22 = 0xCC with the driver's partial LUT; nextPage() then
        // rewrites 0x26 = 0x24 (writeImageAgain), so 0x26 is right either way.
        display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
        display.firstPage();
        do {
            display.drawBitmap(0, 0, rtc_prev.px, SCREEN_W, SCREEN_H, GxEPD_BLACK);
        } while (display.nextPageToPrevious());
        display.firstPage();
        do {
            display.drawBitmap(0, 0, s_frame, SCREEN_W, SCREEN_H, GxEPD_BLACK);
        } while (display.nextPage());
        _last_partial_ms = millis();
        _had_partial = true;
        _refreshes_partial++;
    }
    display.hibernate();
    memcpy(rtc_prev.px, s_frame, SCREEN_FRAME_LEN);
    rtc_prev.partials_since_full = refresh_count_after(rtc_prev.partials_since_full, kind);
    rtc_prev.crc = prev_crc();
    _prev_valid = true;
    _render_seq++;
    _rendered_at_ms = millis();
}

// The one decision per frame request (docs/DEVICE_UI.md "E-ink refresh rules"): the canvas
// holds the rendered frame; `base_kind` is its layout kind for a base
// content frame (SHOWN_NONE for cards and service frames), `title` the CRC
// of a ticker's title (another source = layout change), `condition` marks
// a condition card. Asks logic/refresh_policy, blits or not, keeps the
// bookkeeping and logs one line. Returns what happened.
static RefreshKind present(uint8_t event, uint8_t base_kind, uint32_t title, bool condition) {
    RefreshInputs in;
    in.event = event;
    in.prev_valid = _prev_valid;
    in.layout_changed = base_kind != SHOWN_NONE && (base_kind != rtc_prev.base_kind || title != rtc_prev.base_title);
    in.frame_identical = _prev_valid && memcmp(s_frame, rtc_prev.px, SCREEN_FRAME_LEN) == 0;
    in.battery = _battery;
    in.partials_since_full = rtc_prev.partials_since_full;
    in.ms_since_full = millis() - _last_full_ms;
    in.ms_since_partial = _had_partial ? millis() - _last_partial_ms : 0xFFFFFFFFu;
    RefreshKind k = refresh_decide(in);
    Serial.printf("Refresh: %s (event %u, %u partials since full%s)\n", refresh_kind_str(k), event,
                  (unsigned)in.partials_since_full, in.layout_changed ? ", layout changed" : "");
    if (k == REFRESH_DEFER) { _deferred = true; return k; }
    if (base_kind != SHOWN_NONE) _deferred = false;     // a base frame drawn (or found identical) settles the debt
    if (k == REFRESH_NONE) { _refreshes_skipped++; return k; }
    blit(k);
    if (base_kind != SHOWN_NONE) {
        rtc_prev.base_kind = base_kind;
        rtc_prev.base_title = title;
    }
    rtc_prev.condition_card = condition ? 1 : 0;
    rtc_prev.crc = prev_crc();
    return k;
}

// Non-base frames (service, boot, AP cards): no layout kind, never a condition card.
static void present_frame(uint8_t event) {
    present(event, SHOWN_NONE, 0, false);
}

static void begin_frame() {
    _overlay = false;
    _temp = false;
    _temp_until_ms = 0;
    canvas.fillScreen(CV_WHITE);
}

// Condition card (docs/DEVICE_UI.md "Screens"): glyph centred in the top 48 rows, an
// 18 pt line and a 12 pt line below, no badges (the glyph is the fact).
// 18 pt because it must read from two metres and in the panel thumbnail;
// the strings of ui_strings.h are measured to fit 286 px in these fonts.
static void draw_condition_card() {
    static const GFXfont* const kBig[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b };
    static const GFXfont* const kMid[] = { &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    const int16_t cx = SCREEN_W / 2;
    const char* big = "";
    char small[40] = "";
    switch (_card) {
        case CARD_OFFLINE:
            draw_wifi_lost(cx, 54, 4);
            big = UI_OFFLINE_TITLE;
            device_offline_age_line(_content_at_ms ? (millis() - _content_at_ms) / 1000 : _card_age_s, small, sizeof(small));
            _state = SCREEN_OFFLINE;
            break;
        case CARD_OTA:
            draw_update_arrow(cx, 30);
            big = UI_OTA_TITLE;
            strcpy(small, UI_OTA_HINT);
            _state = SCREEN_OTA;
            break;
        case CARD_BATTERY_EMPTY:
            draw_battery(cx - 44, 8, 80, 40, 4, 0, false);
            big = UI_BATTERY_EMPTY;
            strcpy(small, UI_BATTERY_EMPTY_HINT);
            _state = SCREEN_BATTERY_EMPTY;
            break;
        default:   // CARD_POWER_BATTERY: the cell at its level; the second
                   // line only when the mode switch follows (age_s = the interval)
            draw_battery(cx - 44, 8, 80, 40, 4, _status.batt_known ? (int)_status.batt_pct : -1,
                         device_battery_low(_status.power, _status.batt_known, _status.batt_pct));
            big = UI_POWER_BATTERY;
            if (_card_age_s && _card_age_s != DS_AGE_UNKNOWN)
                snprintf(small, sizeof(small), UI_POWER_BATTERY_HINT_FMT, (unsigned)_card_age_s);
            _state = SCREEN_POWER_BATTERY;
            break;
    }
    print_centered_fitted(big, 58, 92, kBig, 2, 0);
    if (small[0]) print_centered_fitted(small, 94, 124, kMid, 2, 0);
}

// Base frame: the condition card, or content / the WAITING card with badges.
// `event` says why (content, badge, restore, hygiene); a card set overrides
// it: the transitional ON-BATTERY card is a service frame (partial), the
// condition cards go full.
static void draw_base(uint8_t event) {
    begin_frame();
    uint8_t kind = SHOWN_NONE;
    uint32_t title = 0;
    bool condition = false;
    if (_card != CARD_NONE) {
        draw_condition_card();
        if (_card == CARD_POWER_BATTERY) event = RP_EV_SERVICE;
        else { event = RP_EV_CONDITION; condition = true; }
    } else {
        bool has = _last_title[0] || _last_message[0] || _grid_on;
        if (_grid_on) draw_grid(draw_badges());         // the badges first: the age line fits before them
        else {
            if (has) draw_content();
            else draw_waiting();
            draw_badges();
        }
        _state = has ? SCREEN_CONTENT : SCREEN_WAITING;
        kind = has ? (_grid_on ? SHOWN_GRID : _tk.ticker ? SHOWN_TICKER : SHOWN_TEXT) : SHOWN_WAITING;
        // the title and the badge name: another name on the badge is a layout change too;
        // for the grid the set and order of the names
        if (_grid_on) title = grid_set_key(&_grid);
        else if (_tk.ticker) {
            title = esp_rom_crc32_le(0, (const uint8_t*)_last_title, strlen(_last_title));
            title = esp_rom_crc32_le(title, (const uint8_t*)_tk.short_label, strlen(_tk.short_label));
        }
    }
    present(event, kind, title, condition);
}

void display_set_card(ScreenCard card, uint32_t age_s) {
    _card = card;
    _card_age_s = age_s;
}

ScreenCard display_card() {
    return _card;
}

ScreenState display_current_state() {
    return _state;
}

uint32_t display_content_seq() {
    return _content_seq;
}

void display_temp_hold(uint32_t ms) {
    if (_temp) _temp_until_ms = millis() + ms;
}

void display_temp_adopt(uint32_t ms) {
    if (_overlay) return;
    _temp = true;
    _temp_until_ms = millis() + ms;
}

void display_temp_end() {
    if (_temp) draw_base(RP_EV_RESTORE);
}

bool display_temp_active() {
    return _temp;
}

void display_loop() {
    if (!_initialized) return;
    if (_temp && _temp_until_ms && (int32_t)(millis() - _temp_until_ms) >= 0) draw_base(RP_EV_RESTORE);
    else if (_temp || _overlay) return;
    // A content frame held by the partial spacing: draw it now.
    else if (_deferred && (millis() - _last_partial_ms) >= RP_PARTIAL_MIN_MS) draw_base(RP_EV_CONTENT);
    // Full-refresh hygiene (docs/DEVICE_UI.md "E-ink refresh rules"): a day without a full refresh.
    else if ((millis() - _last_full_ms) >= FULL_REFRESH_MAX_AGE_MS) draw_base(RP_EV_HYGIENE);
}

bool display_refresh_badges() {
    if (_overlay || _temp || _card != CARD_NONE) return false;   // cards carry no badges
    draw_base(RP_EV_BADGE);
    return true;
}

uint32_t display_ms_since_refresh() {
    return millis() - _rendered_at_ms;
}

// Identify frame (docs/API.md "Screen"): the number
// as large as the panel allows (18 pt bold scaled x4 = ~100 px, readable from
// a few metres), the device name below. No badges, no secrets.
void display_show_identify(unsigned n, const char* name) {
    char num[4];
    snprintf(num, sizeof(num), "%u", n > 99 ? 99u : n);
    canvas.fillScreen(CV_WHITE);
    canvas.setFont(&FreeSansBold18pt7b);
    canvas.setTextSize(4);
    text_centered(num, 104);
    canvas.setTextSize(1);
    canvas.setFont(&FreeSansBold9pt7b);
    text_centered(name, 125);
    _state = SCREEN_IDENTIFY;
    present_frame(RP_EV_SERVICE);
    _overlay = false;
    _temp = true;
    _temp_until_ms = 0;
}

void display_show_pairing(const char* code, const char* chk, const char* name, const char* from_group, unsigned expires_s) {
    canvas.fillScreen(CV_WHITE);
    static const GFXfont* const kSmall[] = { &FreeSansBold9pt7b };
    // <= 28 chars: FreeSansBold 9 pt fits about 30 characters in 286 px
    // (the longer "... in the panel" was truncated on the real panel).
    print_fitted("Pairing - enter this code:", TEXT_MARGIN_X, 36, kSmall, 1, TEXT_MAX_W);
    // "482 913" centred in 18 pt
    char big[8];
    snprintf(big, sizeof(big), "%.3s %.3s", code, code + 3);
    canvas.setFont(&FreeSansBold18pt7b);
    text_centered(big, 84);
    char line[80];
    if (from_group && *from_group) snprintf(line, sizeof(line), "check %s  %us  %s -> ?", chk, expires_s, from_group);
    else snprintf(line, sizeof(line), "check %s  %us  %s", chk, expires_s, name);
    print_fitted(line, TEXT_MARGIN_X, 118, kSmall, 1, TEXT_MAX_W);
    _state = SCREEN_PAIRING;
    present_frame(RP_EV_SERVICE);
    _overlay = true;
    _temp = false;          // the pairing session restores the content itself
    _temp_until_ms = 0;
}

// Outcome frame (drawn partial): same layout as the code screen, but no
// code on it, so the frame may be served by /api/screen.* (_overlay is
// cleared). The caller sets the hold with display_temp_hold().
void display_show_pairing_result(const char* big, const char* detail) {
    static const GFXfont* const kSmall[] = { &FreeSansBold9pt7b };
    static const GFXfont* const kBig[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    canvas.fillScreen(CV_WHITE);
    print_fitted("Pairing", TEXT_MARGIN_X, 36, kSmall, 1, TEXT_MAX_W);
    print_fitted(big, TEXT_MARGIN_X, 84, kBig, 3, TEXT_MAX_W);
    if (detail && *detail) print_fitted(detail, TEXT_MARGIN_X, 118, kSmall, 1, TEXT_MAX_W);
    _state = SCREEN_PAIRING;
    present_frame(RP_EV_SERVICE);
    _overlay = false;
    _temp = true;
    _temp_until_ms = 0;     // the caller sets the hold with display_temp_hold()
}

bool display_overlay_active() {
    return _overlay;
}

void display_show_message(const char* title, const char* message) {
    display_show_content(title, message, nullptr);
}

void display_set_stale_ms(uint32_t ms) {
    _stale_ms = ms;
}

bool display_stale_crossed() {
    bool shows_age = _tk.ticker && _content_at_ms != 0 && !_tk.time[0];
    return ticker_stale_crossing(&_stale_fired, shows_age, ticker_total_age_s(), _stale_ms / 1000);
}

void display_show_content(const char* title, const char* message, const TickerFields* t) {
    strlcpy(_last_title, title, sizeof(_last_title));
    strlcpy(_last_message, message, sizeof(_last_message));
    if (t && t->ticker) _tk = *t;
    else { memset(&_tk, 0, sizeof(_tk)); _tk.age_s = TICKER_AGE_UNKNOWN; }
    _grid_on = false;
    _content_at_ms = millis();
    if (!_content_at_ms) _content_at_ms = 1;
    _stale_fired = false;   // new content: its own crossing may ask once more
    _content_seq++;
    _card = CARD_NONE;      // new content replaces a card; device_state decides whether it returns
    draw_base(RP_EV_CONTENT);
}

void display_show_grid(const GridFrame* g) {
    _grid = *g;
    if (_grid.n > GRID_MAX) _grid.n = GRID_MAX;
    _grid_on = true;
    _last_title[0] = '\0';                       // the symbols, comma-separated: the shelf's Showing line
    for (uint8_t i = 0; i < _grid.n; i++) {
        if (i) strlcat(_last_title, ", ", sizeof(_last_title));
        strlcat(_last_title, _grid.cells[i].symbol, sizeof(_last_title));
    }
    _last_message[0] = '\0';
    memset(&_tk, 0, sizeof(_tk));
    _tk.ticker = true;                           // the age line and the stale crossing work as for one ticker
    _tk.age_s = g->age_s;
    _content_at_ms = millis();
    if (!_content_at_ms) _content_at_ms = 1;
    _stale_fired = false;
    _content_seq++;
    _card = CARD_NONE;
    draw_base(RP_EV_CONTENT);
}

void display_show_base() {
    draw_base(RP_EV_RESTORE);
}

void display_clear_content() {
    _last_title[0] = '\0';
    _last_message[0] = '\0';
    _tk.ticker = false;
    _grid_on = false;
    _content_at_ms = 0;
    _stale_fired = false;
}

// Boot frames: big line at the 18 pt baseline, two 9 pt lines below.
void display_show_boot_frame(BootFrame kind, const char* version, unsigned position, unsigned of) {
    static const GFXfont* const kBig[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b };
    static const GFXfont* const kSmall[] = { &FreeSansBold9pt7b };
    char big[40], l1[48], l2[48];
    if (kind == BOOT_FRAME_RESTART_N) {
        snprintf(big, sizeof(big), UI_RESTART_N_FMT, position, of);
        strcpy(l1, UI_RESTART_HINT_1);
        strcpy(l2, UI_RESTART_HINT_2);
    } else if (kind == BOOT_FRAME_USB_ONLY) {
        strcpy(big, UI_RECOVERY_USB_ONLY);
        strcpy(l1, UI_RECOVERY_USB_HINT_1);
        snprintf(l2, sizeof(l2), UI_RECOVERY_USB_HINT_2_FMT, of);
    } else {
        strcpy(big, UI_BRAND);
        snprintf(l1, sizeof(l1), "%s", version);
        strcpy(l2, UI_SPLASH_RECOVERY);
    }
    begin_frame();
    print_fitted(big, TEXT_MARGIN_X, 48, kBig, 2, TEXT_MAX_W);
    print_fitted(l1, TEXT_MARGIN_X, 88, kSmall, 1, TEXT_MAX_W);
    print_fitted(l2, TEXT_MARGIN_X, 112, kSmall, 1, TEXT_MAX_W);
    _state = SCREEN_BOOT;
    present_frame(RP_EV_BOOT);
}

// Access-point card shared by the set-up portal and recovery mode: title
// 18 pt, "Join Wi-Fi: <ap>" and "open http://<host>" 12 pt, optional 9 pt
// note. No badges (nothing to report yet), no 5x7 (docs/DEVICE_UI.md "Screens").
static void draw_ap_card(const char* title, const char* ap_name, const char* host, const char* note, ScreenState st) {
    static const GFXfont* const kBig[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b };
    static const GFXfont* const kMid[] = { &FreeSansBold12pt7b, &FreeSansBold9pt7b };
    static const GFXfont* const kSmall[] = { &FreeSansBold9pt7b };
    char line[64];
    begin_frame();
    print_fitted(title, TEXT_MARGIN_X, 34, kBig, 2, TEXT_MAX_W);
    snprintf(line, sizeof(line), UI_AP_JOIN_FMT, ap_name);
    print_fitted(line, TEXT_MARGIN_X, 66, kMid, 2, TEXT_MAX_W);
    snprintf(line, sizeof(line), UI_AP_OPEN_FMT, host);
    print_fitted(line, TEXT_MARGIN_X, 96, kMid, 2, TEXT_MAX_W);
    if (note && *note) print_fitted(note, TEXT_MARGIN_X, 122, kSmall, 1, TEXT_MAX_W);
    _state = st;
    present_frame(RP_EV_BOOT);
}

void display_show_recovery(const char* ap_name, const char* host, const char* note) {
    draw_ap_card(UI_RECOVERY_TITLE, ap_name, host, note, SCREEN_RECOVERY);
    _temp = true;
    _temp_until_ms = 0;                         // the caller sets the hold
}

void display_show_setup_instruction(const char* ap_name, const char* ip_addr) {
    display_clear_content();
    draw_ap_card(UI_SETUP_TITLE, ap_name, ip_addr, "", SCREEN_SETUP);
}

const uint8_t* display_frame_buffer() {
    return s_frame;
}

uint32_t display_render_seq() {
    return _render_seq;
}

void display_get_state(DisplayState* out) {
    out->title = _last_title;
    out->value = _last_message;
    out->state = _state;
    out->render_seq = _render_seq;
    out->rendered_at_s = _rendered_at_ms / 1000;
    out->refreshes_full = _refreshes_full;
    out->refreshes_partial = _refreshes_partial;
    out->refreshes_skipped = _refreshes_skipped;
    out->last_full_s = _refreshes_full ? _last_full_ms / 1000 : 0;
    out->partials_since_full = rtc_prev.partials_since_full;
    out->has_content = _content_at_ms != 0;
    out->content_age_s = out->has_content ? (millis() - _content_at_ms) / 1000 : 0;
    out->card = _card;
    out->status = _status;
    out->ticker = _tk.ticker ? &_tk : nullptr;
    out->ticker_age_s = _tk.ticker ? ticker_total_age_s() : 0;
    out->grid = _grid_on ? &_grid : nullptr;
}

void display_prepare_sleep() {
    if (_initialized) {
        display.hibernate(); // SSD1680 deep sleep (no-op if already asleep)
    }
    display_power(false);    // cut EP_3V3 rail
    // Release the data lines so the unpowered panel is not back-fed through
    // its IO protection diodes. BUSY is already an input.
    // TODO(hw-verify): check EP_3V3 test point reads 0 V in deep sleep.
    pinMode(PIN_EPD_CS, INPUT);
    pinMode(PIN_EPD_DC, INPUT);
    pinMode(PIN_EPD_RST, INPUT);
    pinMode(PIN_EPD_CLK, INPUT);
    pinMode(PIN_EPD_MOSI, INPUT);
}
