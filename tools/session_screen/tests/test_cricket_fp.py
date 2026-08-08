# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Cricket-FP characterisation tests.

The trade-off math is the critical pin: at any threshold T, "leakers
reclaimed" = count(FP.max_bandwidth_khz < T), "TP lost" = count(TP.max_bandwidth_khz < T).
A mis-sign or wrong aggregation would mislead the auditor's tuning call,
so this test locks the counts against a hand-computed fixture.
"""
from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from tools.session_screen import cricket_fp as cfp


# --- feature extraction ------------------------------------------------------

def _write_sidecar(dir_: Path, name: str, events: list) -> Path:
    dir_.mkdir(parents=True, exist_ok=True)
    p = dir_ / f"{name}.json"
    p.write_text(json.dumps({
        "format_version": 1,
        "recording": {"wav_path": f"{name}.wav"},
        "detector": {"tunables": {"min_bandwidth_khz": 0.9}},
        "events": events,
    }))
    (dir_ / f"{name}.wav").write_bytes(b"")   # placeholder
    return p


def test_load_clip_features_picks_the_most_passing_event(tmp_path: Path) -> None:
    """A clip is kept if *any* event passes the gate. So for a threshold
    simulation on ``min_bandwidth_khz``, the relevant per-clip stat is
    ``max(bandwidth_khz)`` — the widest event, most likely to pass a
    stricter gate. Same shape for the other three features."""
    _write_sidecar(tmp_path, "a", [
        {"bandwidth_khz": 0.5, "drift_khz": 10.0,
         "path_ratio": 2.0, "mono_fraction": 0.4, "gate_rejected": True},
        {"bandwidth_khz": 3.0, "drift_khz": 2.0,
         "path_ratio": 0.5, "mono_fraction": 0.9, "gate_rejected": False},
    ])
    df = cfp.load_clip_features(tmp_path, ["a.wav"])
    row = df.iloc[0]
    assert row["max_bandwidth_khz"] == pytest.approx(3.0)
    assert row["min_drift_khz"]     == pytest.approx(2.0)
    assert row["min_path_ratio"]    == pytest.approx(0.5)
    assert row["max_mono_fraction"] == pytest.approx(0.9)
    assert row["n_events"] == 2


def test_load_clip_features_handles_missing_sidecar(tmp_path: Path) -> None:
    """A missing sidecar yields an empty-feature row rather than raising
    — a corrupt/deleted sidecar shouldn't kill a whole run."""
    df = cfp.load_clip_features(tmp_path, ["does_not_exist.wav"])
    assert len(df) == 1
    assert df.iloc[0]["n_events"] == 0


# --- trade-off table --------------------------------------------------------

def test_bandwidth_tradeoff_counts_below_threshold() -> None:
    """Threshold T = 1.5 kHz. FP with max_bw ∈ {0.5, 1.0, 2.0} → 2 reclaimed
    (0.5, 1.0). TP with max_bw ∈ {1.2, 3.0} → 1 lost (1.2)."""
    fp = pd.DataFrame([
        {"clip_wav": "a", "max_bandwidth_khz": 0.5},
        {"clip_wav": "b", "max_bandwidth_khz": 1.0},
        {"clip_wav": "c", "max_bandwidth_khz": 2.0},
    ])
    tp = pd.DataFrame([
        {"clip_wav": "x", "max_bandwidth_khz": 1.2},
        {"clip_wav": "y", "max_bandwidth_khz": 3.0},
    ])
    td = cfp.bandwidth_tradeoff_table(fp, tp, nudges_khz=(1.5,),
                                      baseline_khz=0.9)
    baseline_row = td[td["min_bandwidth_khz"] == 0.9].iloc[0]
    nudged_row   = td[td["min_bandwidth_khz"] == 1.5].iloc[0]
    # Baseline reclaims the one FP with max_bw < 0.9 (fixture: {0.5}).
    # In practice that shouldn't happen — a clip whose widest event is
    # below the current threshold would have been rejected on the live
    # run — so if this row is non-zero in real data it flags either a
    # sidecar inconsistency or a gate state we didn't account for.
    assert baseline_row["n_fp_reclaimed"] == 1
    assert baseline_row["n_tp_lost"] == 0
    # Nudged threshold reclaims 2 FP (0.5, 1.0), loses 1 TP (1.2).
    assert nudged_row["n_fp_reclaimed"] == 2
    assert nudged_row["n_tp_lost"] == 1


def test_bandwidth_tradeoff_zero_denominator_is_nan_not_zero() -> None:
    fp = pd.DataFrame(columns=["clip_wav", "max_bandwidth_khz"])
    tp = pd.DataFrame([{"clip_wav": "x", "max_bandwidth_khz": 3.0}])
    td = cfp.bandwidth_tradeoff_table(fp, tp, nudges_khz=(1.5,),
                                      baseline_khz=0.9)
    assert td.iloc[-1]["n_fp_total"] == 0
    # An empty denominator should be NaN, not 0.0 — matches the plan's
    # honesty rule ("saw 0 of 0" is not a rate).
    import math
    assert math.isnan(td.iloc[-1]["fp_reclaimed_frac"])
