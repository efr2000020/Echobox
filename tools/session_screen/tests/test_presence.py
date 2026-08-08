# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Presence-recall re-bucketing tests.

The auditor's Task 1 rebuckets the existing per-clip join to file
granularity: presence TP = bat-present file with ≥1 accepted clip,
presence FN = bat-present file with 0 accepted clips. These tests pin
the math + the presence_fn_files.csv listing order.
"""
from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from tools.session_screen import presence as p


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
        "rejected_mode": "" if kept else "all",
        "clip_start_ms": float(start_ms), "clip_end_ms": float(end_ms),
        "clip_duration_ms": float(end_ms - start_ms),
        "n_events": 1, "n_gate_rejected_events": 0 if kept else 1,
        "min_bandwidth_khz": 0.0, "max_drift_khz": 0.0,
        "tunable_min_bw_khz": 0.9,
        "frames_processed": 0, "no_clips": False, "error": "",
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


# --- file-level presence ----------------------------------------------------

def test_presence_tp_needs_at_least_one_kept_clip() -> None:
    truth = pd.DataFrame([
        _truth("bat_kept.wav",   detections=[(1, 1.2, "Pip", 0.9)]),
        _truth("bat_missed.wav", detections=[(1, 1.2, "Pip", 0.9)]),
    ])
    replay = pd.DataFrame([
        _clip("bat_kept.wav",   start_ms=1000, end_ms=1200, kept=True),
        # bat_missed.wav has only a discarded clip → presence_fn
        _clip("bat_missed.wav", start_ms=1000, end_ms=1200, kept=False),
    ])
    summ = p.score_per_file(truth, replay)
    assert summ.present_tp == 1
    assert summ.present_fn == 1


def test_presence_fn_includes_files_with_zero_clips_at_all() -> None:
    truth = pd.DataFrame([
        _truth("silent_bat.wav", detections=[(1, 1.2, "Pip", 0.9)]),
    ])
    replay = pd.DataFrame([_no_clip("silent_bat.wav")])
    summ = p.score_per_file(truth, replay)
    # Zero clips on a bat-present file is the honest presence FN.
    assert summ.present_fn == 1
    assert summ.present_tp == 0


def test_presence_tn_and_fp_on_no_bat_files() -> None:
    truth = pd.DataFrame([
        _truth("quiet.wav"),               # no bat, no clips → present_tn
        _truth("crickets_leak.wav"),        # no bat, kept clip → present_fp
    ])
    replay = pd.DataFrame([
        _no_clip("quiet.wav"),
        _clip("crickets_leak.wav", start_ms=100, end_ms=300, kept=True),
    ])
    summ = p.score_per_file(truth, replay)
    assert summ.present_tn == 1
    assert summ.present_fp == 1


def test_presence_fn_files_sorts_by_detection_count_descending() -> None:
    """The human-ear queue prioritises high-signal misses."""
    truth = pd.DataFrame([
        _truth("many.wav",  detections=[(1, 1.2, "Pip", 0.9)] * 50),
        _truth("few.wav",   detections=[(1, 1.2, "Pip", 0.9)]),
        _truth("caught.wav", detections=[(1, 1.2, "Pip", 0.9)]),
    ])
    replay = pd.DataFrame([
        _no_clip("many.wav"),
        _no_clip("few.wav"),
        _clip("caught.wav", start_ms=1000, end_ms=1200, kept=True),
    ])
    fn = p.presence_fn_files(truth, replay)
    assert list(fn["basename"]) == ["many.wav", "few.wav"]


# --- pass-level presence ----------------------------------------------------

def test_pass_clustering_splits_on_gap() -> None:
    """Two detection clusters separated by >500 ms are two passes.
    Detections within 500 ms merge into one pass."""
    dets = [
        {"start_time_s": 1.00, "end_time_s": 1.02},
        {"start_time_s": 1.20, "end_time_s": 1.22},   # < 500 ms gap → same pass
        {"start_time_s": 5.00, "end_time_s": 5.02},   # > 500 ms gap → new pass
    ]
    passes = p.cluster_detections_into_passes(dets, gap_ms=500.0)
    assert len(passes) == 2
    assert passes[0][0] == pytest.approx(1000.0)
    assert passes[0][1] == pytest.approx(1220.0)


def test_pass_recall_credits_a_pass_if_any_clip_overlaps() -> None:
    truth = pd.DataFrame([_truth("a.wav", detections=[
        (1.0, 1.2, "Pip", 0.9),
        (1.15, 1.25, "Pip", 0.9),   # pass 1 (merged)
        (5.0, 5.1, "Pip", 0.9),     # pass 2
    ])])
    # Clip caught pass 1 only; pass 2 missed.
    replay = pd.DataFrame([
        _clip("a.wav", start_ms=1100, end_ms=1300, kept=True),
    ])
    pass_summ = p.score_per_pass(truth, replay)
    assert pass_summ.n_passes == 2
    assert pass_summ.caught == 1
    assert pass_summ.missed == 1


def test_pass_ignores_files_with_no_bat() -> None:
    truth  = pd.DataFrame([_truth("quiet.wav")])
    replay = pd.DataFrame([_no_clip("quiet.wav")])
    ps = p.score_per_pass(truth, replay)
    assert ps.n_passes == 0
    assert ps.recall_per_pass is None
