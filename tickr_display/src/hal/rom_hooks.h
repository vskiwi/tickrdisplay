#pragma once
// Hooks into the ROM / IDF runtime that the flash-size flags of platformio.ini
// rely on (docs/DEVELOPMENT.md "Build flags"):
//
// * the ROM "nano" printf has no floating-point conversions; the hook it would
//   call for %f / %g / %e is left unset by the IDF, so a stray one would jump
//   to address 0. rom_hooks_init() installs a stub that consumes the argument
//   and prints "?" instead. Real float formatting: src/logic/fmt_float.h.
// * core dumps to flash are compiled out through `-Wl,--wrap`; the no-op
//   replacements live in rom_hooks.cpp.
//
// Call rom_hooks_init() first thing in setup().
void rom_hooks_init();
