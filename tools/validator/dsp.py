# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""WAV loading helpers.

The STFT and HPF biquad that used to live here have moved into the native
validator library — see tools/validator/native.py. They are no longer
maintained on the Python side because every hand-maintained shadow of the
C++ DSP is one more place silent drift can hide.
"""
from __future__ import annotations

import wave
from typing import Tuple

import numpy as np


# Mirrors src/app/Config.hpp. Re-exported here so existing call sites that
# imported NFFT/HOP from .dsp keep working.
NFFT = 4096
HOP = 512


def load_wav_float(path: str) -> Tuple[int, np.ndarray]:
    """Load a mono 16-bit WAV as (sample_rate, float32 in [-1, 1)).

    If the file is multi-channel, only the first channel is kept (matching
    the C++ AlsaAudioSource convention).
    """
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        nch = w.getnchannels()
        raw = w.readframes(w.getnframes())
    d = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if nch > 1:
        d = d[::nch].copy()
    return sr, d
