# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Pass-diagnosis bucketing tests.

Pins the priority order (gate_discarded > detector_never_fired >
clip_window_chopping) and the "no clips at all" default.
"""
from __future__ import annotations

import json
import pandas as pd
import pytest

from tools.session_screen import pass_diagnosis as pd_


def _truth(file, *, detections=()):
    dets = [{"start_time_s": st, "end_time_s": et,
             "low_freq_hz": 20000, "high_freq_hz": 100000,
             "confidence": c, "species": sp}
            for (st, et, sp, c) in detections]
    return {
        "file": file, "duration_s": 60.0,
        "bat_present": bool(dets),
        "n_detections": len(dets),
        "top_species": dets[0]["species"] if dets else "",
        "top_confidence": dets[0]["confidence"] if dets else 0.0,
        "detections_json": json.dumps(dets),
        "resample_hz": 256000, "model_hash": "abc", "error": "",
    }


def _clip(source, *, start_ms, end_ms, kept=True):
    return {
        "source_file": source, "clip_wav": f"{source}.clip.wav",
        "kept": kept, "rejected_reason": "" if kept else "sweep",
        "rejected_mode": "", "clip_start_ms": float(start_ms),
        "clip_end_ms": float(end_ms), "clip_duration_ms": float(end_ms - start_ms),
        "n_events": 1, "n_gate_rejected_events": 0 if kept else 1,
        "min_bandwidth_khz": 0.0, "max_drift_khz": 0.0,
        "tunable_min_bw_khz": 0.9, "frames_processed": 0,
        "no_clips": False, "error": "",
    }


def _no_clip(source):
    return {
        "source_file": source, "clip_wav": "", "kept": False,
        "rejected_reason": "", "rejected_mode": "",
        "clip_start_ms": 0.0, "clip_end_ms": 0.0, "clip_duration_ms": 0.0,
        "n_events": 0, "n_gate_rejected_events": 0,
        "min_bandwidth_khz": 0.0, "max_drift_khz": 0.0,
        "tunable_min_bw_khz": 0.0, "frames_processed": 0,
        "no_clips": True, "error": "",
    }


def test_gate_discarded_when_rejected_clip_overlaps_missed_pass() -> None:
    truth = pd.DataFrame([_truth("a.wav",
        detections=[(1.0, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([
        _clip("a.wav", start_ms=1000, end_ms=1200, kept=False),  # rejected
    ])
    summ, rows = pd_.diagnose_missed_passes(truth, replay)
    assert summ.n_missed == 1
    assert summ.n_gate   == 1
    assert rows.iloc[0]["bucket"] == pd_.GATE_DISCARDED


def test_detector_never_fired_when_no_clips_at_all() -> None:
    truth = pd.DataFrame([_truth("a.wav",
        detections=[(1.0, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([_no_clip("a.wav")])
    summ, rows = pd_.diagnose_missed_passes(truth, replay)
    assert summ.n_missed == 1
    assert summ.n_no_fire == 1
    assert rows.iloc[0]["bucket"] == pd_.DETECTOR_NEVER_FIRED


def test_clip_window_chopping_when_clip_within_1s_of_pass() -> None:
    truth = pd.DataFrame([_truth("a.wav",
        detections=[(1.0, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([
        # Clip lands 400ms before the pass — within 1s chopping threshold.
        _clip("a.wav", start_ms=400, end_ms=600, kept=True),
    ])
    summ, rows = pd_.diagnose_missed_passes(truth, replay)
    assert summ.n_missed == 1
    assert summ.n_chopping == 1
    assert rows.iloc[0]["bucket"] == pd_.CLIP_WINDOW_CHOPPING
    # Nearest clip distance should be ~400 ms (pass starts at 1000, clip ends at 600).
    assert rows.iloc[0]["nearest_clip_ms"] == pytest.approx(400.0)


def test_detector_missed_when_clips_are_far_away() -> None:
    """The refined bucketing: clips exist in the source but the nearest
    is > 1s from the pass edges. That's not chopping — it's the base
    detector not firing on this specific pass, even though it fired
    elsewhere in the file."""
    truth = pd.DataFrame([_truth("a.wav",
        detections=[(1.0, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([
        # Clips 4s + 7s away from the pass at 1000-1200 ms.
        _clip("a.wav", start_ms=5000, end_ms=5200, kept=True),
        _clip("a.wav", start_ms=8000, end_ms=8200, kept=False),
    ])
    summ, rows = pd_.diagnose_missed_passes(truth, replay)
    assert summ.n_missed == 1
    assert summ.n_no_fire == 1
    assert rows.iloc[0]["bucket"] == pd_.DETECTOR_NEVER_FIRED


def test_gate_priority_over_no_fire_when_both_signals_present() -> None:
    """A pass with a rejected-clip overlap AND a nearby gap-window is
    still classified as gate-discarded — the plan's precedence rule
    prefers the tunable lever. Detector-never-fired only applies when
    NO clip overlaps the pass window."""
    truth = pd.DataFrame([_truth("a.wav",
        detections=[(1.0, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([
        _clip("a.wav", start_ms=1100, end_ms=1300, kept=False),  # overlaps + rejected
        _clip("a.wav", start_ms=8000, end_ms=8200, kept=False),
    ])
    summ, rows = pd_.diagnose_missed_passes(truth, replay)
    assert summ.n_gate == 1
    assert summ.n_no_fire == 0


def test_caught_pass_does_not_land_in_diagnostics() -> None:
    truth = pd.DataFrame([_truth("a.wav",
        detections=[(1.0, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([
        _clip("a.wav", start_ms=1000, end_ms=1200, kept=True),
    ])
    summ, rows = pd_.diagnose_missed_passes(truth, replay)
    assert summ.n_caught == 1
    assert summ.n_missed == 0
    assert rows.empty
