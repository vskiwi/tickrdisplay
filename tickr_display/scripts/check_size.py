#!/usr/bin/env python3
"""Fail if the firmware image does not fit into the stock OTA partition.

The stock TickrMeter bootloader/partition table is the Arduino ``default.csv``
layout for 4 MB flash: app0/app1 are 0x140000 = 1 310 720 bytes each. OTA via
the stock ``/update`` endpoint writes into those slots, so the image must never
exceed that size. A warning is printed above WARN_PERCENT of the limit.

Two ways to run it:

* automatically as a PlatformIO post-script (``extra_scripts`` in
  platformio.ini) - the build fails if the image is too large;
* standalone, e.g. in CI::

      python scripts/check_size.py dist/*.bin [--limit BYTES] [--warn-percent N]
"""

import argparse
import os
import sys

OTA_PARTITION_SIZE = 0x140000  # 1_310_720 bytes (app0/app1 in default.csv)
WARN_PERCENT = 95


def check(path, limit=OTA_PARTITION_SIZE, warn_percent=WARN_PERCENT):
    """Return 0 if ``path`` fits into ``limit`` bytes, 1 otherwise."""
    size = os.path.getsize(path)
    percent = size * 100.0 / limit
    print(
        "[check_size] %s: %s bytes = %.1f%% of the OTA partition (%s bytes), "
        "%s bytes free"
        % (path, f"{size:,}", percent, f"{limit:,}", f"{limit - size:,}")
    )
    if size > limit:
        print(
            "[check_size] ERROR: image exceeds the stock OTA partition by %s bytes. "
            "OTA through the stock /update endpoint would fail or brick the device."
            % f"{size - limit:,}",
            file=sys.stderr,
        )
        return 1
    if percent > warn_percent:
        print(
            "[check_size] WARNING: image uses more than %d%% of the OTA partition; "
            "consider trimming features or debug output." % warn_percent,
            file=sys.stderr,
        )
    return 0


def _cli(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("images", nargs="+", help="firmware .bin file(s)")
    parser.add_argument("--limit", type=int, default=OTA_PARTITION_SIZE)
    parser.add_argument("--warn-percent", type=int, default=WARN_PERCENT)
    args = parser.parse_args(argv)
    rc = 0
    for image in args.images:
        rc |= check(image, args.limit, args.warn_percent)
    return rc


try:
    Import("env")  # noqa: F821 - only defined when run by SCons/PlatformIO
except NameError:
    sys.exit(_cli(sys.argv[1:]))
else:

    def _post_action(source, target, env):  # pylint: disable=unused-argument
        # A non-zero return value makes SCons fail the build.
        return check(str(target[0]))

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _post_action)  # noqa: F821
