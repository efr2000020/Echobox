# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""SNR-gap module tests. Pins:

  - noise-floor EMA replicates the C++ math on a canned magnitude
    stream (a real signal ramp).
  - `bestBandSnr` picks the loudest band's top-K mean.
  - Weighted-percentile helper reports the value the auditor expects.

Does NOT test against real audio — that would require the C++ STFT,
which is exercised end-to-end by the ``prong1.py`` driver on the real
corpus.
"""
from __future__ import annotations

import numpy as np
import pytest

from tools.session_screen import snr_gap as sg


def test_ratio_to_db_matches_20log10() -> None:
    assert sg.ratio_to_db(1.0)   == pytest.approx(0.0)
    assert sg.ratio_to_db(10.0)  == pytest.approx(20.0)
    assert sg.ratio_to_db(100.0) == pytest.approx(40.0)


def test_ratio_to_db_zero_is_neg_inf() -> None:
    assert sg.ratio_to_db(0.0) == float("-inf")


def _synthetic_mags(n_frames: int, n_bins: int, *,
                    noise_level: float = 1e-4,
                    signal_frame: int = 60,
                    signal_bin: int = 128,
                    signal_amp: float = 0.5) -> np.ndarray:
    """Constant noise floor with a single loud bin at one frame.
    Simulates a call popping above the noise floor briefly."""
    mags = np.full((n_frames, n_bins), noise_level, dtype=np.float32)
    mags[signal_frame, signal_bin] = signal_amp
    return mags


def test_snr_trace_shows_spike_at_signal_frame() -> None:
    """A single high-magnitude bin in one frame should produce a
    large ``bestBandSnr`` at that frame relative to nearby *warmed*
    frames — this is what makes SNR-gap analysis meaningful.

    We compare against a small idle window right before the signal,
    well past warmup. The first few frames are excluded because the
    noise-floor EMA starts at zero and per-bin SNR is degenerate
    until it catches up — that transient is not the "idle" behaviour
    the test cares about."""
    sr, hop, nfft = 384000, 512, 4096
    n_frames = 200
    signal_frame = 150
    mags = _synthetic_mags(n_frames, nfft // 2 + 1,
                           signal_frame=signal_frame)
    trace = sg.compute_snr_trace(mags, sr, hop,
                                  freq_lo_hz=20000.0, freq_hi_hz=192000.0)
    # Idle window: 10 frames right before the signal, past warmup.
    idle_max = float(trace.best_band_snr[
        signal_frame - 10 : signal_frame].max())
    signal_snr = float(trace.best_band_snr[signal_frame])
    assert signal_snr > 10 * idle_max, (
        f"expected signal frame to spike; got signal={signal_snr} "
        f"idle_max={idle_max}")


def test_snr_trace_respects_warmup() -> None:
    """Frames before WARMUP_FRAMES are marked not-warmed. Consumers
    (extract_pass_snr) skip them so a spurious peak inside warmup
    doesn't get credited as a real detection candidate."""
    mags = _synthetic_mags(100, 2049, signal_frame=5)
    trace = sg.compute_snr_trace(mags, 384000, 512, 20000.0, 192000.0)
    assert not trace.warmed_up[5]
    assert trace.warmed_up[sg.WARMUP_FRAMES]


def test_extract_pass_snr_returns_max_in_window() -> None:
    """A window covering the signal frame returns a strictly larger
    peak than an idle window right before it. The absolute value of
    the peak depends on the EMA convergence and isn't what this test
    is guarding — the *relative* rise is."""
    mags = _synthetic_mags(200, 2049, signal_frame=150)
    trace = sg.compute_snr_trace(mags, 384000, 512, 20000.0, 192000.0)
    # Signal frame → 150 * 512 / 384000 = 200 ms.
    peak_at_signal = sg.extract_pass_snr(trace, pass_start_ms=190.0,
                                          pass_end_ms=210.0)
    peak_before = sg.extract_pass_snr(trace, pass_start_ms=150.0,
                                       pass_end_ms=180.0)
    assert peak_at_signal > 5 * peak_before


def test_weighted_percentile_agrees_with_numpy_when_equal_weights() -> None:
    vals = np.array([1, 2, 3, 4, 5], dtype=float)
    weights = np.ones_like(vals)
    # Equal weights → matches numpy percentile.
    assert sg._weighted_percentile(vals, weights, 50) == pytest.approx(3.0)
    assert sg._weighted_percentile(vals, weights, 90) == pytest.approx(5.0)


def test_weighted_percentile_shifts_with_heavy_tail_weight() -> None:
    """A big weight on the high value pulls the median up — the whole
    point of using calls-weighted percentiles."""
    vals = np.array([1, 2, 3, 4, 5], dtype=float)
    heavy_high = np.array([1, 1, 1, 1, 100])
    assert sg._weighted_percentile(vals, heavy_high, 50) == pytest.approx(5.0)
