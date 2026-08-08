# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Gate-loosening sweep tests.

Pin the real OR-gate semantics (bandwidth OR drift-clause) + the
NET-vs-baseline math + the two-recommendation contract.
"""
from __future__ import annotations

import pandas as pd
import pytest

from tools.session_screen import gate_recovery as gr


def test_or_gate_bandwidth_branch_alone_passes() -> None:
    tune = gr.GateTunables()  # shipping defaults
    # bw >= 0.9, other features fail. Should still pass on Path A.
    assert tune.passes(bw_khz=1.5, drift_khz=0.0, path_ratio=99.0, mono_frac=0.1)


def test_or_gate_drift_branch_alone_passes() -> None:
    tune = gr.GateTunables()
    # bw fails, but drift + path + mono all pass. Path B.
    assert tune.passes(bw_khz=0.2, drift_khz=10.0, path_ratio=1.0, mono_frac=0.9)


def test_or_gate_neither_branch_fails() -> None:
    tune = gr.GateTunables()
    # bw fails AND (drift fails OR path fails OR mono fails).
    assert not tune.passes(bw_khz=0.3, drift_khz=2.0,
                           path_ratio=1.0, mono_frac=0.9)
    assert not tune.passes(bw_khz=0.3, drift_khz=10.0,
                           path_ratio=5.0, mono_frac=0.9)
    assert not tune.passes(bw_khz=0.3, drift_khz=10.0,
                           path_ratio=1.0, mono_frac=0.2)


def test_or_gate_loosened_bandwidth_admits_more() -> None:
    """Loosening min_bandwidth_khz should admit clips that fail Path B."""
    tight = gr.GateTunables(min_bandwidth_khz=0.9)
    loose = gr.GateTunables(min_bandwidth_khz=0.5)
    # bw=0.6, drift-clause failing → tight rejects, loose accepts.
    assert not tight.passes(bw_khz=0.6, drift_khz=2.0,
                            path_ratio=1.0, mono_frac=0.9)
    assert loose.passes(bw_khz=0.6, drift_khz=2.0,
                        path_ratio=1.0, mono_frac=0.9)


def _clip(bw_events, source="s.wav"):
    """Build a ClipRecord where each event has ``bandwidth_khz=bw`` and
    everything else set so ONLY Path A matters (drift branch always fails)."""
    return gr.ClipRecord(
        source_basename=source, clip_wav="clip.wav",
        clip_start_ms=0.0, clip_end_ms=200.0, clip_duration_ms=200.0,
        events=[{"bandwidth_khz": bw, "drift_khz": 0.0,
                 "path_ratio": 99.0, "mono_fraction": 0.0}
                for bw in bw_events],
    )


def test_clip_passes_if_any_event_passes_or() -> None:
    """A clip is kept when at least one of its events passes the OR
    clause; the recorder aggregates per-clip that way (see the
    ``m_batLikeEventsSinceBoot`` counter in
    ``BandEnergyDetector.cpp:461``)."""
    tune = gr.GateTunables()
    passing = _clip([0.3, 0.4, 1.5])   # 1.5 kHz passes
    failing = _clip([0.3, 0.4, 0.5])
    assert gr._clip_would_pass_or(passing.events, tune)
    assert not gr._clip_would_pass_or(failing.events, tune)


def test_pick_recommended_returns_both_picks() -> None:
    """Contract: two picks always returned — one efficient (material
    only), one max-recovery."""
    df = pd.DataFrame([
        {"knob": "baseline", "value": 0.0,
         "n_reclaimed_clips_raw": 0, "n_calls_recovered_raw": 0,
         "kb_added_raw": 0, "n_cricket_leaks_raw": 0,
         "n_reclaimed_clips_net": 0, "n_calls_recovered_net": 0,
         "kb_added_net": 0.0, "n_cricket_leaks_net": 0,
         "calls_per_kb_net": None},
        # Tiny high-efficiency: below the material floor.
        {"knob": "path", "value": 2.0,
         "n_reclaimed_clips_raw": 3, "n_calls_recovered_raw": 5,
         "kb_added_raw": 100, "n_cricket_leaks_raw": 0,
         "n_reclaimed_clips_net": 3, "n_calls_recovered_net": 5,
         "kb_added_net": 100.0, "n_cricket_leaks_net": 0,
         "calls_per_kb_net": 0.05},
        # Material row at ok efficiency.
        {"knob": "bw", "value": 0.6,
         "n_reclaimed_clips_raw": 500, "n_calls_recovered_raw": 800,
         "kb_added_raw": 50000, "n_cricket_leaks_raw": 20,
         "n_reclaimed_clips_net": 500, "n_calls_recovered_net": 800,
         "kb_added_net": 50000.0, "n_cricket_leaks_net": 20,
         "calls_per_kb_net": 0.016},
        # Aggressive: most recovery but worse efficiency.
        {"knob": "bw", "value": 0.3,
         "n_reclaimed_clips_raw": 4000, "n_calls_recovered_raw": 6000,
         "kb_added_raw": 600000, "n_cricket_leaks_raw": 200,
         "n_reclaimed_clips_net": 4000, "n_calls_recovered_net": 6000,
         "kb_added_net": 600000.0, "n_cricket_leaks_net": 200,
         "calls_per_kb_net": 0.010},
    ])
    picks = gr.pick_recommended(df)
    # Efficient pick must SKIP the 5-call row (below material floor)
    # and land on the 800-call material row.
    assert picks["max_efficiency"]["n_calls_recovered_net"] == 800
    # Max-recovery picks the biggest raw net calls.
    assert picks["max_recovery"]["n_calls_recovered_net"] == 6000
