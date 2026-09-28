#pragma once
// Serial log diet (docs/DEVELOPMENT.md "Build flags").
//
// The release image keeps error lines and one line per state transition
// (boot, power, cards, refreshes, pull results). Informational lines -
// progress, addresses, timings, payload dumps - go through LOGV / LOGVLN
// and are compiled only with -DTICKR_LOG_VERBOSE (on in `tickr_dev`).
// The `if (0)` form keeps the arguments "used" for the compiler (no
// unused-variable warnings, printf format checking stays) while the
// optimizer drops the call and its string literal.
#include <Arduino.h>

#ifdef TICKR_LOG_VERBOSE
#define LOGV(...)   Serial.printf(__VA_ARGS__)
#define LOGVLN(s)   Serial.println(s)
#else
#define LOGV(...)   do { if (0) Serial.printf(__VA_ARGS__); } while (0)
#define LOGVLN(s)   do { if (0) Serial.println(s); } while (0)
#endif
