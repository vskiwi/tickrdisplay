# Unit tests

Host-side tests, run with `pio test -e native` (Unity framework, ASan/UBSan on).

* One directory per module: `test/test_<module>/test_main.cpp`.
* The native environment compiles only the pure-logic sources under
  `src/logic/` (see `build_src_filter` in `platformio.ini`); nothing that
  includes `Arduino.h` or the HAL can be tested here.
* `test_sanity/` checks that the runner works and documents how ArduinoJson
  treats the README payload.

See `docs/DEVELOPMENT.md` for how to add a test.
