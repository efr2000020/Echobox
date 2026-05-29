"""DSP primitives shared by the offline detector and the GUI.

Mirrors the C++ pipeline so that detection results obtained here match what
the production binary would produce:
  * FFT: real-input, nfft=4096, hop=512, Hann window, magnitude / nfft.
  * Pre-FFT HPF: a single biquad (Butterworth-Q) designed identically to
    src/dsp/core/BiquadFilter.hpp.
"""
from __future__ import annotations

import wave
from typing import Tuple

import numpy as np

try:
    from scipy.signal import lfilter as _lfilter
    _HAVE_SCIPY = True
except ImportError:  # pragma: no cover
    _HAVE_SCIPY = False


# Defaults match src/main.cpp / src/app/Config.hpp.
NFFT = 4096
HOP = 512


# --- WAV loading ------------------------------------------------------------

def load_wav_float(path: str) -> Tuple[int, np.ndarray]:
    """Load a mono 16-bit WAV as (sample_rate, float32 in [-1, 1)).

    If the file is multi-channel, only the first channel is kept (matching
    the C++ FileAudioSource convention).
    """
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        nch = w.getnchannels()
        raw = w.readframes(w.getnframes())
    d = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if nch > 1:
        d = d[::nch].copy()
    return sr, d


def load_wav_int16(path: str) -> Tuple[int, np.ndarray]:
    """Load a mono 16-bit WAV as (sample_rate, int16 array).

    The int16 form is what the ESP32 streamer sends on the wire — keeping
    the data in native format avoids a redundant float<->int16 round-trip.
    """
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        nch = w.getnchannels()
        raw = w.readframes(w.getnframes())
    d = np.frombuffer(raw, dtype=np.int16)
    if nch > 1:
        d = d[::nch].copy()
    return sr, d


# --- STFT -------------------------------------------------------------------

def hann_window(n: int) -> np.ndarray:
    """0.5 * (1 - cos(2π i / (n-1))); matches src/dsp/core/Window.hpp."""
    return np.hanning(n).astype(np.float32)


def stft_mags(samples: np.ndarray,
              nfft: int = NFFT,
              hop: int = HOP) -> np.ndarray:
    """Magnitude STFT identical (to numerical noise) to KissFftEngine.

    Returns an array of shape (n_frames, n_bins) with n_bins == nfft/2 + 1.
    """
    win = hann_window(nfft)
    n_samples = len(samples)
    if n_samples < nfft:
        return np.empty((0, nfft // 2 + 1), dtype=np.float32)

    n_frames = (n_samples - nfft) // hop + 1
    out = np.empty((n_frames, nfft // 2 + 1), dtype=np.float32)
    inv_n = 1.0 / nfft
    for i in range(n_frames):
        start = i * hop
        seg = samples[start:start + nfft] * win
        out[i] = (np.abs(np.fft.rfft(seg)) * inv_n).astype(np.float32)
    return out


# --- High-pass biquad -------------------------------------------------------

def design_biquad_hpf(cutoff_hz: float,
                      sample_rate: float,
                      q: float = 0.707
                      ) -> Tuple[float, float, float, float, float]:
    """Returns (b0, b1, b2, a1, a2), normalized by a0 like the C++ side."""
    if sample_rate <= 0:
        raise ValueError("sample_rate must be positive")
    w0 = 2.0 * np.pi * cutoff_hz / sample_rate
    alpha = np.sin(w0) / (2.0 * q)
    cos_w0 = np.cos(w0)
    a0 = 1.0 + alpha
    b0 = (1.0 + cos_w0) / 2.0 / a0
    b1 = -(1.0 + cos_w0) / a0
    b2 = (1.0 + cos_w0) / 2.0 / a0
    a1 = -2.0 * cos_w0 / a0
    a2 = (1.0 - alpha) / a0
    return b0, b1, b2, a1, a2


def apply_biquad(samples: np.ndarray, coeffs) -> np.ndarray:
    """Apply a normalized biquad. Uses scipy.signal.lfilter when available."""
    b0, b1, b2, a1, a2 = coeffs
    if _HAVE_SCIPY:
        return _lfilter([b0, b1, b2], [1.0, a1, a2], samples).astype(np.float32)

    out = np.empty_like(samples, dtype=np.float32)
    x1 = x2 = y1 = y2 = 0.0
    for i, x0 in enumerate(samples):
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return out
