"""Locates and loads the HRL shared library."""
import ctypes
import os
import sys

_NAMES = {
    "win32": ["hrldll.dll", "libhrldll.dll", "hrl.dll"],
    "darwin": ["libhrldll.dylib", "libhrl.dylib"],
}
_DEFAULT = ["libhrldll.so", "libhrl.so"]


def load_library():
    """HRL_LIBRARY (full path) > the package folder > the current folder >
    the system search path."""
    explicit = os.environ.get("HRL_LIBRARY")
    if explicit:
        return _load(explicit)
    names = _NAMES.get(sys.platform, _DEFAULT)
    here = os.path.dirname(os.path.abspath(__file__))
    if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(here)  # dependencies shipped next to the DLL (MinGW runtime...)
    for folder in (here, os.getcwd()):
        for n in names:
            path = os.path.join(folder, n)
            if os.path.exists(path):
                return _load(path)
    last = None
    for n in names:
        try:
            return _load(n)
        except OSError as e:
            last = e
    raise OSError("HRL shared library not found (set HRL_LIBRARY to its path): %s" % last)


def _load(path):
    if sys.platform == "win32":
        return ctypes.CDLL(path, winmode=0) if sys.version_info >= (3, 8) else ctypes.CDLL(path)
    return ctypes.CDLL(path)
