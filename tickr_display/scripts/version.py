"""PlatformIO pre-script: firmware version define + distributable image copy.

* Adds ``-DFIRMWARE_VERSION="<version>"`` to the build. The version comes from
  (in order): the ``FIRMWARE_VERSION`` environment variable, ``git describe
  --tags --always --dirty --match "v*"``, or ``"unknown"``.
* After ``firmware.bin`` is produced, copies it to
  ``dist/tickrdisplay-<version><custom_bin_suffix>.bin`` inside the project dir
  (``custom_bin_suffix`` is a per-environment option in platformio.ini).

Registered in platformio.ini via ``extra_scripts = pre:scripts/version.py``.
"""

import os
import shutil
import subprocess

Import("env")  # noqa: F821 - provided by SCons/PlatformIO


def _git_describe(cwd):
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty", "--match", "v*"],
            cwd=cwd,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    if not out:
        return None
    if not out.startswith("v"):
        # No reachable v* tag: `--always` yields a bare (possibly -dirty) SHA.
        out = "0.0.0-g" + out
    return out


def get_version():
    explicit = os.environ.get("FIRMWARE_VERSION", "").strip()
    if explicit:
        return explicit
    return _git_describe(env.subst("$PROJECT_DIR")) or "unknown"


VERSION = get_version()
print("Firmware version: %s" % VERSION)
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(VERSION))])


def copy_to_dist(source, target, env):  # pylint: disable=unused-argument
    firmware_bin = str(target[0])
    dist_dir = os.path.join(env.subst("$PROJECT_DIR"), "dist")
    os.makedirs(dist_dir, exist_ok=True)
    suffix = env.GetProjectOption("custom_bin_suffix", "") or ""
    dst = os.path.join(dist_dir, "tickrdisplay-%s%s.bin" % (VERSION, suffix))
    shutil.copy2(firmware_bin, dst)
    print("Firmware image: %s (%d bytes)" % (dst, os.path.getsize(dst)))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_to_dist)
