#pragma once

// Display (SSD1680)
#define PIN_EPD_CLK      13
#define PIN_EPD_MOSI     14
#define PIN_EPD_CS       15
#define PIN_EPD_DC       27
#define PIN_EPD_RST      26
#define PIN_EPD_BUSY     18
#define PIN_EPD_PWR      19 // EP_3V3_EN, LOW = ON
// The panel is write-only, MISO is not wired. The SPI driver still needs an
// input pin: passing -1 makes Arduino-ESP32 fall back to the VSPI default
// MISO = GPIO19 (= EPD_PWR!) and GPIO12 is the MTDI strapping pin, so a
// free GPIO is used as a dummy input instead. GPIO 16 is free on both units
// (docs/HARDWARE.md "Other GPIO", pull probe), not a strapping pin,
// bonded on the WROOM-32E (WROVER modules use it for PSRAM, this one does
// not). GPIO 4 - the dummy MISO until 0318603 - turned out to be the sense
// enable of the rev B measurement network (PIN_BAT_SENSE_EN below).
#define PIN_SPI_MISO_DUMMY 16

// LEDs (Active LOW)
#define PIN_LED_BLUE     21
#define PIN_LED_GREEN    22
#define PIN_LED_RED      23

// Audio
#define PIN_AMP_EN       17 // HIGH = ON
#define PIN_AUDIO_DAC    25

// Power sensing (docs/HARDWARE.md "Power sensing").
// Two ADC1 dividers, identical on both board revisions once
// the sense network is enabled:
//   GPIO 32 - the cell (charger output); "vsys" in the API for compatibility
//   GPIO 33 - the system rail after the OR-ing element: VBUS with a cable,
//             cell minus a diode drop on battery
// GPIO 4 enables the network on rev B (high, or the internal pull-up;
// low / floating = GPIO 32/33 tied to 3.3 V, GPIO 34-39 follow the bus).
// No function on rev A. Driven HIGH while awake on both revisions, LOW
// before deep sleep (not held). The stock firmware wrote it high without
// ever enabling the output, so it never saw a cell on rev B.
#define PIN_BAT_CELL     32
#define PIN_USB_VIN      33
#define PIN_BAT_SENSE_EN 4
