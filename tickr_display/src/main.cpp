#include <Arduino.h>
#include "log.h"
#include <WiFi.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include "hal/hal_display.h"
#include "hal/hal_power.h"
#include "hal/hal_power_probe.h"
#include "hal/hal_indication.h"
#include "hal/rom_hooks.h"
#include "managers/connectivity_manager.h"
#include "managers/ota_manager.h"
#include "managers/arduino_ota.h"
#include "managers/peer_manager.h"
#include "managers/pairing_manager.h"
#include "managers/relay_manager.h"
#include "managers/screen_api.h"
#include "managers/recovery_manager.h"
#include "managers/wifi_portal.h"
#include "managers/config_manager.h"
#include "logic/renderer.h"
#include "logic/command_queue.h"
#include "logic/battery.h"
#include "logic/status_policy.h"
#include "logic/device_state.h"
#include "logic/pull_scheduler.h"
#include "logic/relay.h"
#include <esp_task_wdt.h>
#include <esp_system.h>

// Task watchdog: covers the main loop (command execution: e-ink refresh ~3 s,
// melodies <= 15 s). The Wi-Fi setup portal blocks up to 180 s of inactivity,
// so loop() is only subscribed after connectivity init.
#define WDT_TIMEOUT_S 30

ConnectivityManager cm;

// Battery-mode retry policy
#define BACKOFF_CAP_MIN       60   // exponential backoff ceiling (minutes)
#define LOW_BATT_SLEEP_MIN    60   // sleep while the cell is below BATTERY_LOW_MV

// RTC slow memory survives deep sleep (and ESP.restart()), not power-off.
RTC_DATA_ATTR static uint32_t rtc_fail_count = 0;     // consecutive WiFi / pull failures
RTC_DATA_ATTR static uint32_t rtc_wake_count = 0;
RTC_DATA_ATTR static uint8_t  rtc_low_batt_shown = 0; // "Battery empty" card already drawn
// Age of the content on the panel across sleeps (docs/DEVICE_UI.md "E-ink refresh
// rules"): minutes slept since the last successful frame, 0 = none this discharge.
RTC_DATA_ATTR static uint32_t rtc_slept_min = 0;
RTC_DATA_ATTR static uint8_t  rtc_content_shown = 0;
// Relay for sleepers (docs/MULTI_DEVICE.md "Relay for sleeping members"): the relay that
// answered last (skips the broadcast probe while it still answers) and
// whether the WAITING card was drawn on this discharge while nothing was parked.
RTC_DATA_ATTR static uint32_t rtc_relay_ip = 0;
RTC_DATA_ATTR static uint8_t  rtc_waiting_shown = 0;
// Runtime USB -> battery mode switch (docs/DEVICE_UI.md "Power-mode switch"):
// the restart into the battery flow leaves this marker (+1 when content was
// on the panel) so the next boot knows it was asked for - it then skips the
// clear-to-white (the panel keeps the content until the first battery
// frame) and, if the detector says USB again, locks the switch until a power
// cycle instead of looping.
#define POWER_RESTART_MAGIC 0x50575200u            // "PWR" + has_content bit
RTC_DATA_ATTR static uint32_t rtc_power_restart = 0;
static bool power_switch_locked = false;           // the last switch landed back in USB mode

static DisplayStatus shown_status;   // badge snapshot the base frame carries
static PullScheduler pull_sched;     // USB-mode pull timer (logic/pull_scheduler.h)

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wakeup";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// Reset cause as the power-cycle recovery counter sees it (recovery_counter.h):
// only a power-on / EN-pin / brownout reset is "the user at the switch".
// tickr_dev (-DTICKR_RECOVERY_TEST) also counts software restarts so the
// series can be driven remotely with POST /api/system/restart.
static RecoveryResetKind recovery_reset_kind(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return RECOVERY_RST_POWERON;
        case ESP_RST_EXT:       return RECOVERY_RST_EXT;
        case ESP_RST_BROWNOUT:  return RECOVERY_RST_BROWNOUT;
#ifdef TICKR_RECOVERY_TEST
        case ESP_RST_SW:        return RECOVERY_RST_POWERON;
#else
        case ESP_RST_SW:        return RECOVERY_RST_SW;
#endif
        case ESP_RST_DEEPSLEEP: return RECOVERY_RST_DEEPSLEEP;
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return RECOVERY_RST_CRASH;
        default:                return RECOVERY_RST_OTHER;
    }
}

// Logs physical flash size (JEDEC ID) vs. what the bootloader header and the
// partition table assume. The chip is 8 MB but the stock bootloader and the
// stock 4 MB table we build against only address the lower half; a table that
// reached beyond the configured size would fail at runtime (OTA slot,
// LittleFS). Diagnostics only, no behaviour change.
static void log_flash_info() {
    uint32_t physical   = ESP.getFlashChipSize();     // from JEDEC ID
    uint32_t configured = spi_flash_get_chip_size();  // bootloader image header
    uint32_t part_end   = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        uint32_t end = p->address + p->size;
        if (end > part_end) part_end = end;
    }
    esp_partition_iterator_release(it);

    LOGV("Flash: physical %u KB, configured %u KB, partitions end at %u KB\n",
                  physical / 1024, configured / 1024, part_end / 1024);
    if (part_end > configured) {
        Serial.println("WARNING: partition table exceeds the flash size in the bootloader header;"
                       " writes beyond it (OTA/LittleFS) will fail. Reflash the bootloader with the right flash size.");
    }
    if (part_end > physical) {
        Serial.println("WARNING: partition table exceeds the physical flash size!");
    }
}

// Builds the badge snapshot (logic/status_policy.h). `prev` supplies the
// battery hysteresis: the displayed percentage only moves when the real one
// drifted by at least BATTERY_DISPLAY_STEP_PCT. The Wi-Fi bars are plain
// thresholds - they are reported by /api/screen/state only, never drawn.
static DisplayStatus collect_status(const DisplayStatus* prev) {
    DisplayStatus s;
    s.wifi_connected = WiFi.isConnected();
    s.wifi_bars = wifi_rssi_to_bars(s.wifi_connected ? WiFi.RSSI() : 0, s.wifi_connected);
    if (s.wifi_connected) {
        strlcpy(s.ip, WiFi.localIP().toString().c_str(), sizeof(s.ip));
    }
    s.usb = power_get_source() == POWER_USB;
    // No glyph at all when the detector has no information in auto mode
    // (docs/DEVICE_UI.md "Screens", POWER UNKNOWN); an override is a fact the user asserted.
    bool unknown = power_get_mode() == POWER_MODE_AUTO && power_get_sense() == POWER_SENSE_INVALID;
    s.power = unknown ? BADGE_POWER_UNKNOWN : (s.usb ? BADGE_POWER_USB : BADGE_POWER_BATTERY);
    s.batt_known = power_battery_measurable();   // false on board ? only: outline without a percentage
    if (s.usb) {
        s.batt_pct = 100; // the bolt is drawn; the charging cell's level is not shown
    } else if (!s.batt_known) {
        s.batt_pct = 0;
    } else {
        uint8_t raw = battery_mv_to_percent(power_get_battery_mv());
        if (prev && !prev->usb && !battery_level_changed(prev->batt_pct, raw, BATTERY_DISPLAY_STEP_PCT)) {
            s.batt_pct = prev->batt_pct;
        } else {
            s.batt_pct = battery_percent_quantize(raw, BATTERY_DISPLAY_STEP_PCT);
        }
    }
    return s;
}

static void refresh_shown_status() {
    shown_status = collect_status(&shown_status);
    display_set_status(shown_status, false);
}

static void battery_sleep_minutes(uint32_t minutes) {
    Serial.printf("Entering Deep Sleep for %u minutes (fails=%u)\n", minutes, rtc_fail_count);
    rtc_slept_min += minutes;
    display_prepare_sleep();
    power_deep_sleep((uint64_t)minutes * 60ULL * 1000000ULL);
}

// A failed wake-up (docs/DEVICE_UI.md "E-ink refresh rules"): no frame on the
// first miss (a router rebooting is common and the panel keeps the last
// content), the OFFLINE card with the age of the shown data on the 2nd,
// then every 4th; the LED blinks red either way. One frame per wake at most.
static void battery_fail(uint32_t interval_min) {
    rtc_fail_count++;
    if (device_offline_card_due(rtc_fail_count)) {
        display_set_card(CARD_OFFLINE, rtc_content_shown ? rtc_slept_min * 60u : DS_AGE_UNKNOWN);
        display_show_base();
    }
    indication_led(true, false, false);
    delay(300);
    indication_led(false, false, false);
    battery_sleep_minutes(backoff_minutes(interval_min, rtc_fail_count, BACKOFF_CAP_MIN));
}

void setup() {
    rom_hooks_init();   // before the first printf: the ROM printf's %f hook (rom_hooks.h)
    Serial.begin(115200);
    // small delay for serial to stabilize
    delay(500);
    Serial.println("\n=== TickrDisplay Booting ===");
    Serial.printf("Reset reason: %s (%d)\n", reset_reason_str(esp_reset_reason()), (int)esp_reset_reason());

#ifdef TICKR_POWER_PROBE
    // Dev-only power-sensing probe (hal_power_probe.h): ADC2 scan + GPIO
    // pull probe. Must run before any HAL touches a pin and before Wi-Fi
    // (ADC2 is unusable once the radio is on).
    power_probe_boot();
#endif

    command_queue_init();

    power_init();
    bool woke = power_woke_from_deep_sleep();
    // A software restart asked for by the runtime mode switch: the RTC
    // counters start a new discharge, but the content on the panel is known.
    bool power_restart = !woke && esp_reset_reason() == ESP_RST_SW &&
                         (rtc_power_restart & ~1u) == POWER_RESTART_MAGIC;
    bool restart_had_content = power_restart && (rtc_power_restart & 1u);
    rtc_power_restart = 0;
    // The copy of the shown frame in RTC memory (hal_display, docs/DEVICE_UI.md
    // "E-ink refresh rules") may seed a partial refresh only after a deep-sleep wake or a
    // software restart (OTA, the power-mode restart); power-on, EN pin,
    // brownout, panic and watchdog resets start with a full refresh.
    esp_reset_reason_t reset = esp_reset_reason();
    bool rtc_trusted = reset == ESP_RST_DEEPSLEEP || reset == ESP_RST_SW;
    if (!woke) {
        // Cold boot / reset: RTC state is stale or random.
        rtc_fail_count = 0;
        rtc_wake_count = 0;
        rtc_low_batt_shown = 0;
        rtc_slept_min = 0;
        rtc_content_shown = restart_had_content ? 1 : 0;
        rtc_relay_ip = 0;
        rtc_waiting_shown = 0;
    }
    rtc_wake_count++;
    Serial.printf("Boot: %s, wake #%u\n", woke ? "deep-sleep wake" : power_restart ? "power-mode restart" : "cold", rtc_wake_count);
    // Power-cycle recovery counter (NVS): decided before anything can block.
    bool enter_recovery = recovery_boot(recovery_reset_kind(esp_reset_reason()));
    log_flash_info();

    indication_init();

    // The power-source override (AppConfig::power_source) and the cell
    // divider must be known before the USB/battery decision below, i.e.
    // before cm.init() loads the configuration: mount the FS and read it
    // once here (cm.init() reads the file again; LittleFS.begin() on a
    // mounted FS is a no-op).
    {
        config_init();
        AppConfig early;
        config_load(early);
        power_set_mode((PowerMode)early.power_source);
        power_set_cell_divider(early.adc_cell_num, early.adc_cell_den);
        relay_begin();   // /relay, /peers directories + the nonce store
    }

    PowerSource pwr = power_get_source();
    Serial.printf("Power Source: %s (board %s, mode %s, sense %d), cell=%u mV, rail=%u mV\n",
                  power_source_label(), power_board_str(), config_power_source_str((uint8_t)power_get_mode()),
                  (int)power_get_sense(), power_get_battery_mv(), power_get_vin_mv());
    if (power_restart && pwr != POWER_BATTERY) {
        // The boot detector (peak-hold, leaning to USB in the hold band)
        // disagrees with the runtime reading that asked for this restart. A
        // second attempt would loop every grace period: the switch stays off
        // until a power cycle; the device runs as USB.
        power_switch_locked = true;
        Serial.println("Power: restart landed in USB mode - switch locked until a power cycle");
    }

    // Deep-discharge protection: below BATTERY_LOW_MV draw the "Battery empty"
    // card once, then sleep without WiFi until USB shows up or the cell recovers.
    // The cell is GPIO 32 on both revisions (docs/HARDWARE.md "Power sensing");
    // only an unrecognised board (?) has no channel - there the latch never
    // arms and the hardware (charger cut-off / brownout) is the only guard.
    if (pwr == POWER_BATTERY && power_battery_measurable() &&
        battery_is_low(power_get_battery_mv(), rtc_low_batt_shown)) {
        Serial.println("Low battery - sleeping");
        if (!rtc_low_batt_shown) {
            display_init(!woke, rtc_trusted, true);
            display_set_card(CARD_BATTERY_EMPTY, DS_AGE_UNKNOWN);   // EMPTY card, once per discharge
            display_show_base();
            rtc_low_batt_shown = 1;
        }
        battery_sleep_minutes(LOW_BATT_SLEEP_MIN);
    }
    rtc_low_batt_shown = 0;

    // Boot frame (power-cycle recovery, docs/WEB_UI.md "Recovery mode"). It replaces
    // GxEPD2's cold-boot "clear to white" refresh, so a USB boot still costs
    // two refreshes (frame + WAITING card). On battery every refresh is energy:
    // the plain splash is skipped there and a frame appears only once a
    // series has started (position >= 2). Recovery itself is USB-only - the
    // battery flow would deep-sleep under the AP.
    bool battery = (pwr == POWER_BATTERY);
    uint8_t pos = recovery_position();
    BootFrame frame = BOOT_FRAME_SPLASH;
    bool draw_frame = false;
    if (enter_recovery) { draw_frame = true; frame = battery ? BOOT_FRAME_USB_ONLY : BOOT_FRAME_SPLASH; }
    else if (pos >= 2)  { draw_frame = true; frame = BOOT_FRAME_RESTART_N; }
    else if (pos == 1 && !battery) { draw_frame = true; }
    // No clear-to-white after a power-mode restart either: the panel keeps
    // the content and the first battery frame replaces it (one refresh).
    display_init(!woke && !draw_frame && !power_restart, rtc_trusted, battery);
    refresh_shown_status();
    if (enter_recovery && !battery) {
        recovery_set_pending(true);
        display_show_recovery(WIFI_PORTAL_AP_SSID, WIFI_PORTAL_AP_IP, UI_RECOVERY_NOTE_STARTING);
    } else if (draw_frame) {
        display_show_boot_frame(frame, TICKR_FW_VERSION, pos, RECOVERY_THRESHOLD);
#ifdef TICKR_RECOVERY_TEST
        // Test builds keep the boot frame 20 s so it can be fetched through
        // /api/screen.bmp once the STA link is up (the WAITING card follows).
        display_temp_adopt(20000);
#endif
    }

    renderer_init();

    // Callback for new data
    auto onData = [](const String& payload) {
        renderer_process_payload(payload);
    };

    // Initialize connectivity (starts the web server, connects to WiFi; may
    // block in the setup portal while somebody is configuring, else 180 s).
    // In battery mode with saved credentials this never opens the portal and
    // returns false on failure.
    bool connected = cm.init(onData, battery);
    refresh_shown_status();
    // T_stale of the ticker's age line: 3 x the refresh interval, min 10 min.
    display_set_stale_ms(device_stale_ms((uint32_t)cm.getConfig().refresh_interval_min));
    screen_api_set_source(cm.pullSourceStr(), cm.tickerSymbol());   // /api/screen/state source / symbol

    // Group credentials (docs/MULTI_DEVICE.md "Groups and pairing"): HMAC port + beacon tags,
    // before the first beacon in either power mode.
    pairing_begin(&cm.getConfig());

    // From here on the main task must keep looping: arm the task watchdog.
    // Battery-mode branches below either deep-sleep or restart well within
    // WDT_TIMEOUT_S (fetch_pull_data() has a 10 s HTTP timeout).
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    if (battery) {
        LOGVLN("Running in Battery Mode");
        // The refresh interval (default 1 hour), raised to 15 min while the 2x2
        // grid fetches up to four sources per wake (docs/TICKERS.md "Several
        // tickers on one panel: the 2x2 grid").
        int interval = (int)cm.batteryIntervalMin();

        if (!connected) {
            if (!cm.wifiCredentialsSaved()) {
                // First setup: the portal timed out with nothing to connect
                // to. Re-open it (restart) so the user can still configure.
                Serial.println("No WiFi credentials - restarting into setup portal");
                ESP.restart();
            }
            battery_fail(interval);
        }
        rtc_fail_count = 0;

        // One discovery beacon per wake-up with the planned sleep (docs/MULTI_DEVICE.md "Relay for sleeping members");
        // no listener - the device is asleep again in a few seconds.
        peers_begin(cm.getConfig().device_name, TICKR_FW_VERSION, true, (uint32_t)interval * 60U);
        peers_send_beacon_now();

        bool hasUrl = cm.pullConfigured();   // a Pull URL or the ticker source
        RelaySource src = relay_pick_source(hasUrl, pairing_has_group());
        if (src == RELAY_SRC_PULL_URL) {
            if (!cm.fetch_pull_data()) {
                Serial.println("Failed to fetch data");
                battery_fail(interval);
            }
            LOGVLN("Data fetched successfully");
            rtc_slept_min = 0;          // the age line of a later OFFLINE card counts from here
            rtc_content_shown = 1;
            indication_led(false, true, false);
            delay(300);
            indication_led(false, false, false);
            battery_sleep_minutes(interval);
        } else if (src == RELAY_SRC_RELAY) {
            // Group member without a Pull URL (docs/MULTI_DEVICE.md "Relay for sleeping members"): probe for a
            // relay, fetch what the panel parked for us, render, upload the frame. A
            // pending OTA job flashes and restarts inside relay_client_run().
            RelayClientResult rc = relay_client_run(&rtc_relay_ip, (uint32_t)interval);
            LOGV("Relay: result %d (0 no relay, 1 nothing pending, 2 shown, 3 failed)\n", (int)rc);
            if (rc == RELAY_RC_NO_RELAY || rc == RELAY_RC_FAILED) battery_fail(interval);   // back-off, OFFLINE card on the 2nd miss
            if (rc == RELAY_RC_SHOWN) {
                rtc_slept_min = 0;
                rtc_content_shown = 1;
                indication_led(false, true, false);
                delay(300);
                indication_led(false, false, false);
            } else if (!rtc_content_shown && !rtc_waiting_shown) {
                display_show_base();    // nothing parked yet: the WAITING card once per discharge
                rtc_waiting_shown = 1;
            }
            battery_sleep_minutes(interval);
        } else {
            // WAITING card ("Ready - choose content at http://ip/"): the
            // device stays awake here, so the address works.
            Serial.println("No Pull URL configured - staying awake for config");
            display_show_base();
            // Treat as USB mode to allow configuration
        }
    } else {
        LOGVLN("Running in USB Mode");
        if (!connected) {
            // Portal timed out on USB power: just retry, we are not draining a cell.
            Serial.println("WiFi not connected - restarting");
            ESP.restart();
        }
        // First base frame: the WAITING card until a payload arrives
        // (docs/DEVICE_UI.md "Screens"). The recovery card and a held boot frame
        // (test build) take that refresh; the WAITING card is what the
        // restore draws when they end. A deep-sleep wake that found USB is
        // the battery -> USB mode switch (docs/DEVICE_UI.md "Power-mode switch"): the panel
        // keeps the last battery frame and the scheduled pull (3 s) draws
        // the content with the bolt - one frame, no WAITING card, no ON-USB card.
        bool hasUrl = cm.pullConfigured();
        if (recovery_pending()) {
            recovery_start_if_pending();
        } else if (woke && hasUrl) {
            LOGVLN("Power: woke on USB - the next pull draws the content");
        } else if (!display_temp_active()) {
            display_show_base();
        }

        // Multi-device discovery: UDP beacons + peer table (docs/MULTI_DEVICE.md "How it works").
        peers_begin(cm.getConfig().device_name, TICKR_FW_VERSION, false, 0);

        // USB-mode scheduled pull (docs/TICKERS.md "Fetch schedule and errors"): a Pull URL is fetched
        // every refresh interval (+-10 % jitter, back-off on failure) instead
        // of only on battery wake-ups. Disabled without a URL; POST /config
        // arms it (pullReconfigured() in loop()).
        pull_sched_start(&pull_sched, millis(), (uint32_t)cm.getConfig().refresh_interval_min,
                         hasUrl, esp_random());

        // LAN push-OTA (ArduinoOTA, port 3232): USB power only - on battery the
        // device deep-sleeps right after the pull and must not keep a listener
        // around. No-op without a configured ota_password, and compiled out
        // entirely unless -DTICKR_ARDUINO_OTA (env tickr_dev).
        ota_arduino_begin(cm.getConfig().ota_password, true);
    }
}

unsigned long last_status_check = 0;
static BadgePolicy badge_policy;
static uint32_t badge_seen_seq = 0;        // render_seq at the previous badge poll
static DeviceState device_state;           // condition cards (logic/device_state.h)
static uint8_t  power_switch_shown = PSW_COUNT;   // last logged PowerSwitch

// Restart into the battery flow (docs/DEVICE_UI.md "Power-mode switch"): the grace
// passed on a debounced battery reading and nothing blocks. setup() reads
// the marker (no clear-to-white; a USB verdict there locks the switch).
static void power_mode_restart() {
    Serial.println("Power: grace over on battery - restarting into battery mode");
    Serial.flush();
    rtc_power_restart = POWER_RESTART_MAGIC | (display_content_seq() ? 1u : 0u);
    display_prepare_sleep();
    delay(50);
    ESP.restart();
}

// The service frame on the panel, for the state machine (a transitional
// card due under one is dropped) and for the LED overlays (docs/DEVICE_UI.md "LED and sound").
static ServiceLevel service_level() {
    if (display_overlay_active()) return SERVICE_PAIRING;
    if (!display_temp_active()) return SERVICE_NONE;
    switch (display_current_state()) {
        case SCREEN_IDENTIFY: return SERVICE_IDENTIFY;
        case SCREEN_PAIRING:  return SERVICE_PAIRING;
        default:              return SERVICE_AP;   // recovery card, held boot frame
    }
}

// Screen state machine, once a second after the badge poll (docs/DEVICE_UI.md
// "State machine"). The machine says which card should be the base frame; the panel is
// refreshed once when that changes - unless a service frame is up, whose
// restore then draws the card (the OTA card outranks identify / pairing and
// replaces them at once). Returns true when it refreshed the panel.
static bool device_state_tick(const DisplayStatus& cur) {
    DeviceInputs in;
    in.now_ms = millis();
    in.wifi_connected = cur.wifi_connected;               // raw link; the machine's timers are >= 60 s
    in.power = shown_status.power;                        // debounced by the badge policy (5 s)
    in.batt_known = shown_status.batt_known;
    in.batt_pct = shown_status.batt_pct;
    in.content_seq = display_content_seq();
    in.ota_active = ota_update_in_progress() || ota_arduino_in_progress();
    ServiceLevel svc = service_level();
    in.service = (uint8_t)svc;
    // Runtime mode switch: only in `auto` (an override is the user's
    // word, never restart against it), only with a Pull URL (the battery
    // flow has nothing to wake for otherwise - it would stay awake anyway),
    // never again after a switch that landed back in USB mode. Recovery
    // mode and the portal hold a due restart until they end.
    in.switch_allowed = power_get_mode() == POWER_MODE_AUTO && cm.pullConfigured() && !power_switch_locked;
    in.hold = recovery_active() || wifi_portal_active();
    DeviceOutputs out;
    device_state_poll(&device_state, in, &out);

    // LED language (docs/DEVICE_UI.md "LED and sound"): rows 5, 7, 8 follow the panel / the machine; the
    // recovery breathe follows the mode. OTA, set-up and pairing set theirs
    // where they start.
    indication_overlay(LED_OVL_IDENTIFY, svc == SERVICE_IDENTIFY);
    indication_overlay(LED_OVL_RECOVERY, recovery_active());
    indication_overlay(LED_OVL_BATT_LOW, out.batt_low);
    indication_overlay(LED_OVL_OFFLINE, out.led_offline);
    if (out.power_flapping) Serial.println("Power: flapping - badge only, no card, long grace");

    // One log line per switch transition; the API mirrors it.
    screen_api_set_power_switch(out.power_switch, out.power_grace_s);
    if (out.power_switch != power_switch_shown) {
        power_switch_shown = out.power_switch;
        Serial.printf("Power: switch %s (restart into battery mode in %lu s)\n",
                      power_switch_str(out.power_switch), (unsigned long)out.power_grace_s);
    }
    if (out.power_restart) power_mode_restart();   // never returns

    if (out.card == display_card()) return false;
    Serial.printf("Card: %s -> %s\n", screen_card_str(display_card()), screen_card_str(out.card));
    // The ON-BATTERY card promises "updates every N min" only when the
    // restart into the battery flow will follow (docs/DEVICE_UI.md "Power-mode switch").
    uint32_t hint = DS_AGE_UNKNOWN;
    if (out.card == CARD_POWER_BATTERY && out.power_switch == PSW_GRACE)
        hint = cm.batteryIntervalMin();   // the battery flow's interval (default 60, floor 15 with a grid)
    display_set_card(out.card, hint);
    if (svc == SERVICE_NONE || (out.card == CARD_OTA && svc != SERVICE_AP)) {
        // The frame carries today's facts: the link that held 30 s must not
        // wait another 60 s of badge debounce for its crossed glyph to go
        // (that would be a second refresh).
        shown_status = cur;
        display_set_status(shown_status, false);
        badge_policy = BadgePolicy();
        display_show_base();
        // LED rule: the transitional card flashes its colour once (amber, 0.5 s).
        if (out.card == CARD_POWER_BATTERY) indication_flash(255, 120, 0, 500);
        return true;
    }
    return false;   // the restore of the service frame draws it
}

void loop() {
    esp_task_wdt_reset();
    cm.loop();

    // Deferred restart after web OTA / boot-partition change, sketch MD5 warm-up
    // and ArduinoOTA.handle() (the latter is a no-op unless enabled above).
    ota_loop();
    peers_loop();
    pairing_loop();      // key agreement + pairing screens run here, never on async_tcp
    screen_api_loop();   // identify frame (POST /api/screen/identify)
    recovery_loop();     // power-cycle counter window, recovery AP timeout / actions
    display_loop();      // restores the content when a temporary frame's hold expires
    indication_loop();   // LED overlay patterns (blink / breathe / pulse)
    esp_task_wdt_reset();

    // Execute at most one queued network command per iteration so MQTT/status
    // housekeeping keeps running between long (display/sound) operations.
    Command cmd;
    if (command_queue_pop(&cmd)) {
        command_execute(&cmd);
        esp_task_wdt_reset();
    }

    // USB-mode pull: fetch the Pull URL when the scheduler says so; a
    // changed URL / interval fetches at once. A miss (or no link) backs off
    // and keeps the last frame - no card, and never a red LED (red = "down").
    if (cm.pullReconfigured()) {
        const AppConfig& c = cm.getConfig();
        display_set_stale_ms(device_stale_ms((uint32_t)c.refresh_interval_min));
        pull_sched_config(&pull_sched, millis(), (uint32_t)c.refresh_interval_min, cm.pullConfigured());
        screen_api_set_source(cm.pullSourceStr(), cm.tickerSymbol());
    }
    if (pull_sched_due(&pull_sched, millis())) {
        if (!WiFi.isConnected()) {
            pull_sched_defer(&pull_sched, millis(), PULL_DEFER_NO_LINK_MS);      // the OFFLINE machine tells the user
        } else if (display_overlay_active()) {
            pull_sched_defer(&pull_sched, millis(), PULL_DEFER_SERVICE_MS);      // the pairing code must stay readable
        } else {
            bool ok = cm.fetch_pull_data();
            pull_sched_done(&pull_sched, millis(), ok);
            // The reason ("http 451", "connect/tls failed") is the pull's last_error;
            // the per-step lines of the HTTP path are verbose-only (log.h).
            const char* why = cm.lastError();
            if (strncmp(why, "pull: ", 6) == 0) why += 6;
            Serial.printf("Pull: %s, next in %lu s (fails=%u)\n", ok ? "ok" : why,
                          (unsigned long)((pull_sched.next_ms - millis()) / 1000), pull_sched.fails);
            esp_task_wdt_reset();
        }
    }

    // Badges (logic/status_policy.h, docs/DEVICE_UI.md "E-ink refresh rules"): poll once a second.
    // A cosmetic change (battery step, IP) is stored and appears with the
    // next content refresh; a state change (Wi-Fi up/down, power source) is
    // debounced and then costs at most one refresh per minute of its
    // own; the ticker's stale crossing is owed one refresh under the same
    // rule. The bars are stored silently for the API.
    if (millis() - last_status_check > 1000) {
        last_status_check = millis();

        DisplayStatus cur = collect_status(&shown_status);
        uint32_t seq = display_render_seq();
        bool refreshed = seq != badge_seen_seq;
        badge_seen_seq = seq;
        uint8_t change = badge_change(shown_status, cur);
        // The ticker's stale crossing: once per content, one full
        // refresh under the badge minute rule, so "stale N min" appears even
        // when nothing else ever refreshes (a dead proxy on USB).
        if (display_stale_crossed()) {
            change |= BADGE_STALE;
            LOGVLN("Ticker: stale - one refresh owed");
        }
        BadgeAction act = badge_poll(&badge_policy, change, display_ms_since_refresh(), refreshed);
        if (act == BADGE_KEEP) {
            if (cur.wifi_bars != shown_status.wifi_bars) {
                shown_status.wifi_bars = cur.wifi_bars;
                display_set_status(shown_status, false);
            }
        } else {
            LOGV("Badges: bat=%u%% power=%u wifi=%d change=%u%s\n",
                          cur.batt_pct, cur.power, cur.wifi_connected, change,
                          act == BADGE_REFRESH ? " -> refresh" : " (folded)");
            shown_status = cur;
            display_set_status(shown_status, act == BADGE_ADOPT);
        }
        // Cards first: a card change is one refresh that carries the
        // adopted badges anyway, so the badge refresh of the same poll is
        // folded into it (never two refreshes in one second).
        bool drew = device_state_tick(cur);
        if (act == BADGE_REFRESH && !drew) display_refresh_badges();
    }

    delay(10);
}
