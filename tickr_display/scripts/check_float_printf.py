#!/usr/bin/env python3
"""Fail the build on a printf format the ROM printf cannot render.

The firmware links the ESP32 ROM "nano" printf (``-Wl,-T,esp32.rom.newlib-nano.ld``
in platformio.ini, docs/DEVELOPMENT.md "Build flags"). That build of newlib has
no floating-point conversions (``%f %F %e %E %g %G %a %A``) and no 64-bit
integer conversions (``%lld %llu %llx`` ...). A float conversion goes through a
hook the IDF leaves unset (src/hal/rom_hooks.cpp installs a stub that prints
``?``), a 64-bit one silently prints garbage. This script scans the string
literals of every source under ``src/`` (the generated headers excluded) and
fails the build on the first hit, pointing at ``src/logic/fmt_float.h`` for
floats.

Standalone: ``python scripts/check_float_printf.py`` (exit 1 on a hit).
"""

import os
import re
import sys

# A conversion spec: % [flags/width/precision] [length] conversion.
# ``%%`` is skipped by the leading ``(?<!%)`` + the explicit ``%%`` alternative.
_SPEC = re.compile(r"%%|%([-+ #0-9.*]*)(hh|h|ll|l|j|z|t|L)?([a-zA-Z])")
_FLOAT = set("fFeEgGaA")
_PCT_ENCODED = re.compile(r"^%[0-9A-Fa-f]{2}$")   # "%2F" in a URL, not a format
_EXTS = (".c", ".cc", ".cpp", ".h", ".hpp", ".ino")


def _strip_block_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _string_literals(line):
    """The bodies of the string literals of one line; a ``//`` outside a
    literal ends the line (a ``//`` inside one, e.g. a URL, does not)."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c == '"':
            j = i + 1
            while j < n and line[j] != '"':
                j += 2 if line[j] == "\\" else 1
            out.append(line[i + 1:j])
            i = j + 1
        elif c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        else:
            i += 1
    return out


def scan_text(text):
    """Yield (line_no, spec) for every offending conversion in C/C++ source text."""
    text = _strip_block_comments(text)
    for no, line in enumerate(text.split("\n"), 1):
        for lit in _string_literals(line):
            for m in _SPEC.finditer(lit):
                if m.group(0) == "%%":
                    continue
                if _PCT_ENCODED.match(m.group(0)):
                    continue
                length, conv = m.group(2), m.group(3)
                if conv in _FLOAT or length == "ll":
                    yield no, m.group(0)


def scan_dir(src_dir):
    hits = []
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [d for d in dirs if d != "generated"]
        for name in sorted(files):
            if not name.endswith(_EXTS):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for no, spec in scan_text(f.read()):
                    hits.append((path, no, spec))
    return hits


def check(project_dir):
    hits = scan_dir(os.path.join(project_dir, "src"))
    for path, no, spec in hits:
        print("[check_float_printf] %s:%d: '%s' is not supported by the ROM printf "
              "(floats: src/logic/fmt_float.h; no 64-bit integer formats)"
              % (os.path.relpath(path, project_dir), no, spec), file=sys.stderr)
    return 0 if not hits else 1


def _cli():
    project_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    rc = check(project_dir)
    if rc == 0:
        print("[check_float_printf] ok - no %f/%g/%e/%lld formats in src/")
    return rc


try:
    Import("env")  # noqa: F821 - only defined when run by SCons/PlatformIO
except NameError:
    sys.exit(_cli())
else:
    if check(env.subst("$PROJECT_DIR")) != 0:  # noqa: F821
        raise SystemExit("check_float_printf: unsupported printf format in src/ (see above)")
