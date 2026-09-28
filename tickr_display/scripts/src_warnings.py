"""PlatformIO post-script: strict compiler warnings for project sources only.

Two things happen here, both limited to files under ``src/`` (``projenv``):

1. The flags listed in the ``custom_src_warnings`` option of the active
   environment (e.g. ``-Wall -Wextra``) are appended. Third-party libraries and
   the Arduino framework keep PlatformIO's default flags.
2. Every include directory that is not part of this project (framework SDK,
   ``.pio/libdeps``) is turned from ``-I`` into ``-isystem`` so that warnings
   triggered inside those headers (e.g. unused parameters in
   ``ESPAsyncWebServer.h`` or ``soc_memory_types.h``) are not reported while
   compiling our translation units. Relative search order is preserved.

Registered in platformio.ini via ``extra_scripts = post:scripts/src_warnings.py``.
"""

import os

Import("env", "projenv")  # noqa: F821 - provided by SCons/PlatformIO

_project_dir = env.subst("$PROJECT_DIR")
_own_dirs = [os.path.join(_project_dir, d) for d in ("src", "include", "lib")]


def _is_project_path(path):
    path = os.path.abspath(path)
    return any(path == d or path.startswith(d + os.sep) for d in _own_dirs)


_keep, _system = [], []
for _entry in projenv.get("CPPPATH", []):
    _path = projenv.subst(str(_entry))
    (_keep if _is_project_path(_path) else _system).append(_entry if _is_project_path(_path) else _path)

projenv.Replace(CPPPATH=_keep)
for _path in _system:
    projenv.Append(CCFLAGS=["-isystem", _path])

_flags = (env.GetProjectOption("custom_src_warnings", "") or "").split()
if _flags:
    projenv.Append(CCFLAGS=_flags)
