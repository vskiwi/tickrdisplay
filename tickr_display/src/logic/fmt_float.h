#pragma once
// Float -> decimal text without the C library's floating-point printf.
//
// The firmware is linked against the ESP32 ROM "nano" printf (docs/DEVELOPMENT.md
// "Build flags"), which has no %f / %g / %e support - a float conversion
// anywhere in the image would call an unset ROM hook. These two helpers cover
// what the firmware needs; they use integer arithmetic only, are exact for
// every finite float (the value is expanded as m * 5^s / 10^s) and round
// half-to-even on exact ties like the C library does, so their output is
// byte-identical to snprintf("%g") / snprintf("%.<frac>f") on the host
// (test/test_fmt_float).
//
// Both return false (and write an empty string) when the text does not fit.
#include <stdint.h>
#include <stddef.h>

// "%g": up to 6 significant digits, trailing zeros removed, exponent style
// (1.234e-05) below 1e-4 and from 1e6. Non-finite: "nan", "inf", "-inf".
bool fmt_float_g(float v, char* out, size_t n);

// "%.<frac>f": fixed notation with `frac` (0..9) fraction digits.
// Non-finite: "nan", "inf", "-inf".
bool fmt_float_fixed(float v, uint8_t frac, char* out, size_t n);
