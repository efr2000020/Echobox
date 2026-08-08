# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Base-detector SNR-gap diagnostic (Prong 1).

For each **detector_never_fired** missed pass — a pass BatDetect2 flagged
where the recorder's band-energy detector never triggered — measure the
**peak SNR** in the pass window at current tunables, expressed as a
ratio (magnitudes / running noise floor) matching what
``BandEnergyDetector::processFrame`` computes on-device.

The output answers the auditor's fork:
    - **just below** (peak SNR sitting near ``m_bandSnrThreshold`` = 12.0
      ratio ≈ 21.6 dB above floor): a threshold nudge could recover real
      calls.
    - **far below**: fundamental energy-detector-vs-CNN sensitivity gap;
      not fixable with a knob.

Design: instead of instrumenting the C++ detector and re-running replay,
we reproduce the SNR math in Python on the same C++ STFT (via
``tools.validator.native.stft``). Every step below traces to a line in
``src/dsp/algorithms/BandEnergyDetector/BandEnergyDetector.cpp``:

    - band edges → BAND_EDGES_HZ[] = [20, 45, 80, 130, 192] kHz
    - per-bin ratio → mag / max(noiseFloor, minAbsFloor)  (line 154)
    - per-band top-K mean → std::nth_element + sum/K       (lines 167-170)
    - best band = argmax(band SNR)                         (line 172)
    - noise-floor EMA → alpha_rise 0.995 / alpha_fall 0.9  (lines 181-186)
    - warmup → 40 frames                                   (m_warmupFramesLimit)

No behaviour change to shipping code; no replay re-run.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import numpy as np
import pandas as pd


# --- constants (traced to BandEnergyDetector.hpp / defaults) ----------------

BAND_EDGES_HZ = (20000.0, 45000.0, 80000.0, 130000.0, 192000.0)
"""BandEnergyDetector.hpp:131. Four sub-bands defined by five edges. The
active set is the intersection with [freq_lo_hz, freq_hi_hz]."""

ALPHA_RISE       = 0.995
ALPHA_FALL       = 0.9
TOP_K            = 8
WARMUP_FRAMES    = 40
MIN_ABS_FLOOR    = 1e-6
BAND_SNR_THRESHOLD_RATIO = 12.0
"""Shipping default. This is a MAGNITUDE RATIO, not decibels — 12.0 ratio
= 20 * log10(12) ≈ 21.6 dB above the noise floor. Every report expresses
the gap both ways so the reader doesn't misread one as the other."""


def ratio_to_db(x: float) -> float:
    """Magnitude ratio → dB. ``x <= 0`` → -inf."""
    if x <= 0.0:
        return float("-inf")
    return 20.0 * float(np.log10(x))


# --- SNR trace over one file ------------------------------------------------

@dataclass
class SnrTrace:
    """Per-frame peak-band SNR trace for one source WAV.

    ``frame_ms[i]`` is the source-time centre of frame ``i``; ``best_band_snr[i]``
    is the ratio at that frame; ``warmedup[i]`` mirrors the C++ warmup gate.
    """
    frame_ms:      np.ndarray
    best_band_snr: np.ndarray
    warmed_up:     np.ndarray
    hop_size:      int
    sample_rate:   int


def _compute_bin_edges(fft_size: int, sample_rate: int,
                       freq_lo_hz: float, freq_hi_hz: float
                       ) -> List[Tuple[int, int]]:
    """Return [(lo_bin, hi_bin)] per band, matching BandEnergyDetector's
    intersect logic (BandEnergyDetector.cpp:48-61)."""
    bin_resolution = sample_rate / fft_size
    n_bins = fft_size // 2 + 1
    user_lo = max(0.0, freq_lo_hz)
    user_hi = min(sample_rate / 2.0, freq_hi_hz)
    bands: List[Tuple[int, int]] = []
    for i in range(len(BAND_EDGES_HZ) - 1):
        band_lo = max(BAND_EDGES_HZ[i], user_lo)
        band_hi = min(BAND_EDGES_HZ[i + 1], user_hi)
        if band_hi <= band_lo:
            continue
        lo = int(band_lo / bin_resolution)
        hi = int(band_hi / bin_resolution)
        if hi > n_bins:
            hi = n_bins
        if lo >= n_bins:
            break
        if hi > lo + 1:
            bands.append((lo, hi))
    return bands


def compute_snr_trace(mags: np.ndarray, sample_rate: int, hop_size: int,
                      freq_lo_hz: float, freq_hi_hz: float
                      ) -> SnrTrace:
    """Vectorised per-frame ``bestBandSnr`` matching the C++ math.

    ``mags`` is shape ``(n_frames, n_bins)`` — magnitudes from a real STFT.
    """
    n_frames, n_bins = mags.shape
    fft_size = 2 * (n_bins - 1)
    bands = _compute_bin_edges(fft_size, sample_rate, freq_lo_hz, freq_hi_hz)

    noise_floor = np.zeros(n_bins, dtype=np.float64)
    best_snr    = np.zeros(n_frames, dtype=np.float64)

    # Per-frame loop — noise-floor EMA is stateful across frames.
    for f in range(n_frames):
        m = mags[f].astype(np.float64)
        floor = np.maximum(noise_floor, MIN_ABS_FLOOR)
        snr_per_bin = m / floor

        best = 0.0
        for lo, hi in bands:
            band_snr = snr_per_bin[lo:hi]
            k = min(TOP_K, hi - lo)
            if k <= 0:
                continue
            # np.partition gives the top-k in O(n); mean of top-k = band SNR.
            top_k = np.partition(band_snr, -k)[-k:]
            band_avg = float(top_k.mean())
            if band_avg > best:
                best = band_avg
        best_snr[f] = best

        # Update noise floor per bin: alpha_rise when mag > floor (slow
        # attack, so a real signal doesn't pull the floor up), alpha_fall
        # when mag <= floor (fast release when the room quiets down).
        alpha = np.where(m > noise_floor, ALPHA_RISE, ALPHA_FALL)
        noise_floor = alpha * noise_floor + (1.0 - alpha) * m

    frame_ms = np.arange(n_frames, dtype=np.float64) * hop_size * 1000.0 / sample_rate
    warmed_up = np.arange(n_frames) >= WARMUP_FRAMES
    return SnrTrace(frame_ms=frame_ms, best_band_snr=best_snr,
                    warmed_up=warmed_up,
                    hop_size=hop_size, sample_rate=sample_rate)


# --- per-pass gap extraction ------------------------------------------------

@dataclass
class MissedPassSnr:
    """One never-fired pass's SNR gap."""
    basename:               str
    pass_start_ms:          float
    pass_end_ms:            float
    n_detections_in_pass:   int
    peak_snr_ratio:         float
    peak_snr_db:            float
    gap_ratio:              float    # peak / threshold (< 1 = below)
    gap_db:                 float    # peak_dB - threshold_dB


def extract_pass_snr(trace: SnrTrace,
                     pass_start_ms: float, pass_end_ms: float
                     ) -> float:
    """Return the max ``best_band_snr`` in the window; 0.0 if the window
    is entirely inside warmup or off the trace end."""
    lo_idx = int(np.searchsorted(trace.frame_ms, pass_start_ms, side="left"))
    hi_idx = int(np.searchsorted(trace.frame_ms, pass_end_ms,   side="right"))
    if hi_idx <= lo_idx:
        return 0.0
    valid = trace.warmed_up[lo_idx:hi_idx]
    snrs  = trace.best_band_snr[lo_idx:hi_idx]
    if not valid.any():
        return 0.0
    return float(snrs[valid].max())


# --- distribution report ----------------------------------------------------

@dataclass
class SnrGapSummary:
    """Weighted percentiles + bin counts for the distribution."""
    n_passes:               int
    n_calls_total:          int
    threshold_ratio:        float
    threshold_db:           float
    just_below_calls:       int    # gap ratio in [0.5, 1.0] i.e. peak in [6, 12] roughly half-threshold
    far_below_calls:        int    # gap ratio < 0.5
    ratio_p10:              float
    ratio_p25:              float
    ratio_p50:              float
    ratio_p75:              float
    ratio_p90:              float


def _weighted_percentile(values: np.ndarray, weights: np.ndarray,
                         q: float) -> float:
    """Weighted percentile — the auditor asked for calls-weighted."""
    if len(values) == 0:
        return float("nan")
    order = np.argsort(values)
    v = values[order]
    w = weights[order].astype(np.float64)
    cum = np.cumsum(w)
    if cum[-1] <= 0:
        return float("nan")
    target = q * cum[-1] / 100.0
    idx = int(np.searchsorted(cum, target, side="left"))
    idx = min(idx, len(v) - 1)
    return float(v[idx])


def summarise(passes: List[MissedPassSnr],
              threshold_ratio: float = BAND_SNR_THRESHOLD_RATIO
              ) -> SnrGapSummary:
    """Pool the per-pass gap distribution, weighted by
    ``n_detections_in_pass`` (calls-weighted per the plan)."""
    if not passes:
        return SnrGapSummary(0, 0, threshold_ratio,
                             ratio_to_db(threshold_ratio),
                             0, 0, *([float("nan")] * 5))
    ratios  = np.array([p.gap_ratio            for p in passes])
    weights = np.array([p.n_detections_in_pass for p in passes])
    n_calls = int(weights.sum())

    just_below = int(weights[(ratios >= 0.5) & (ratios < 1.0)].sum())
    far_below  = int(weights[ratios < 0.5].sum())

    return SnrGapSummary(
        n_passes=len(passes),
        n_calls_total=n_calls,
        threshold_ratio=threshold_ratio,
        threshold_db=ratio_to_db(threshold_ratio),
        just_below_calls=just_below,
        far_below_calls=far_below,
        ratio_p10=_weighted_percentile(ratios, weights, 10),
        ratio_p25=_weighted_percentile(ratios, weights, 25),
        ratio_p50=_weighted_percentile(ratios, weights, 50),
        ratio_p75=_weighted_percentile(ratios, weights, 75),
        ratio_p90=_weighted_percentile(ratios, weights, 90),
    )


def render_md(config_label: str, summary: SnrGapSummary) -> str:
    """Emit the snr_gap_distribution.md body for one config."""
    from .score import HEADLINE_CAVEATS
    lines: List[str] = []
    lines.append(f"# SNR-gap distribution on detector_never_fired passes — {config_label}")
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(
        "**Answer to Prong 1's fork question.** For each of the "
        f"**{summary.n_passes} detector_never_fired passes** "
        f"(carrying **{summary.n_calls_total} BatDetect2 calls** "
        f"total; calls-weighted below), we measured the **peak "
        f"per-frame `bestBandSnr` in the pass window**, matching "
        "``BandEnergyDetector::processFrame`` line-for-line. The gap "
        "= peak / threshold; gap < 1 = below threshold; gap < 0.5 = "
        "far below (well under half-threshold in linear ratio).")
    lines.append("")
    lines.append("**Threshold reminder.** The shipping "
                 "``band_snr_threshold`` is a **magnitude ratio**, not "
                 "decibels — `12.0 ratio ≈ 21.6 dB` above the running "
                 "noise floor. The distribution below reports the ratio "
                 "directly; dB conversion is `20·log₁₀(ratio)`.")
    lines.append("")
    lines.append(f"- threshold ratio: {summary.threshold_ratio:.2f}  "
                 f"({summary.threshold_db:.1f} dB)")
    lines.append(f"- passes in bucket: {summary.n_passes}")
    lines.append(f"- calls in bucket: {summary.n_calls_total}")
    lines.append("")
    lines.append("## Calls-weighted peak-SNR-gap distribution")
    lines.append("")
    lines.append("| percentile | gap ratio | equivalent peak SNR (ratio) | equivalent peak SNR (dB) |")
    lines.append("|---|---:|---:|---:|")
    for q_label, ratio in (("p10", summary.ratio_p10),
                           ("p25", summary.ratio_p25),
                           ("p50", summary.ratio_p50),
                           ("p75", summary.ratio_p75),
                           ("p90", summary.ratio_p90)):
        peak_ratio = ratio * summary.threshold_ratio
        peak_db    = ratio_to_db(peak_ratio) if peak_ratio > 0 else float("nan")
        peak_db_s  = f"{peak_db:.1f}" if np.isfinite(peak_db) else "-inf"
        lines.append(
            f"| {q_label} | {ratio:.3f} | {peak_ratio:.3f} | {peak_db_s} |")
    lines.append("")
    lines.append("## Fork answer (calls-weighted)")
    lines.append("")
    if summary.n_calls_total > 0:
        pct_just = 100.0 * summary.just_below_calls / summary.n_calls_total
        pct_far  = 100.0 * summary.far_below_calls  / summary.n_calls_total
        lines.append(
            f"- **just below** (gap ratio 0.5 ≤ x < 1.0, roughly half-"
            f"to-full threshold): **{summary.just_below_calls}** calls "
            f"({pct_just:.1f}%)")
        lines.append(
            f"- **far below** (gap ratio < 0.5): "
            f"**{summary.far_below_calls}** calls ({pct_far:.1f}%)")
        lines.append("")
        if pct_far > 60:
            verdict = ("**Far-below dominates.** The base energy "
                       "detector is not narrowly missing these calls; "
                       "they sit well under half-threshold in linear "
                       "ratio. A threshold nudge cannot recover them "
                       "cheaply — this is a fundamental "
                       "energy-detector-vs-CNN sensitivity gap. Do "
                       "NOT propose an ``snr_threshold`` nudge as the "
                       "recovery lever. The recommendation is a "
                       "smarter base detector (learned classifier or "
                       "spectrographic shape metric), which is a "
                       "roadmap-scale decision, not a knob.")
        elif pct_just > 60:
            verdict = ("**Just-below dominates.** A material share of "
                       "detector_never_fired calls sit in the "
                       "half-to-full threshold range. A modest "
                       "``snr_threshold`` nudge could recover them at "
                       "some cricket-FP cost — proceed to Prong 3a "
                       "bench sweep to quantify the trade.")
        else:
            verdict = ("**Mixed.** Neither bucket dominates cleanly. "
                       "A threshold nudge would help a chunk of the "
                       "loss but leave a comparable chunk untouched. "
                       "Report both figures; recommend the bench "
                       "sweep only if the customer values the "
                       "recoverable share above the added output "
                       "volume it would generate.")
        lines.append(verdict)
    lines.append("")
    return "\n".join(lines)


# --- CSV row shape ---------------------------------------------------------

def passes_to_df(passes: Iterable[MissedPassSnr]) -> pd.DataFrame:
    return pd.DataFrame([{
        "basename": p.basename,
        "pass_start_ms": p.pass_start_ms,
        "pass_end_ms":   p.pass_end_ms,
        "n_detections_in_pass": p.n_detections_in_pass,
        "peak_snr_ratio": p.peak_snr_ratio,
        "peak_snr_db":    p.peak_snr_db,
        "gap_ratio":      p.gap_ratio,
        "gap_db":         p.gap_db,
    } for p in passes])
