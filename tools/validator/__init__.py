"""Echobox validator package.

Two front-ends:

  * ``python -m validator``       — PyQt6 GUI (offline detection + ALSA Loopback).
  * ``python -m validator.cli``   — batch CLI (describe / run / score / grid /
                                    overlay / diagnose / loopback).

Both call into ``validator.native``, a ctypes wrapper around the C++
``libechobox_validator.so``. The wrapper discovers detection algorithms by
scanning a plugin directory at import time, so this package contains no
algorithm-specific code — adding a new detector in C++ exposes it to the GUI
and CLI automatically.
"""

__version__ = "0.1.0"
