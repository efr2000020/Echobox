"""LiteSpectrum validator package.

Two front-ends:

  * ``python -m validator``       — PyQt6 GUI (offline detection + ESP32 stream).
  * ``python -m validator.cli``   — batch CLI (run / score / grid / overlay /
                                    diagnose / stream).

Both share the same in-process detector implementation (``validator.detector``)
which is a faithful Python port of the C++ ``BandEnergyDetector``.
"""

__version__ = "0.1.0"
