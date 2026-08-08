# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Scorer tests — per-clip time-overlap join.

The v1 scorer was per-file; the v2 scorer buckets each accepted /
rejected clip against BatDetect2 detections overlapping the clip's
[start_ms, end_ms] window in source-WAV time. Tests here exercise:

  1. the four confusion buckets (TP/FN/FP/TN),
  2. the overlap slop (small edge-adjacent detections still count),
  3. per-species grouping,
  4. the CF forced-review path in disagreements.csv,
  5. sources with zero clips don't score,
  6. end-to-end write of report.md + CSVs.
"""
from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from tools.session_screen import manifest as m
from tools.session_screen import score as s


# --- fixture helpers ---------------------------------------------------------

def _truth(file, *, detections=(), top_species="", conf=0.0):
    """One TruthRow-shaped dict. ``detections`` is a list of
    ``(start_s, end_s, species, confidence)`` tuples."""
    dets = [
        {"start_time_s": st, "end_time_s": et,
         "low_freq_hz": 20000, "high_freq_hz": 100000,
         "confidence": c, "species": sp}
        for (st, et, sp, c) in detections
    ]
    return {
        "file": file, "duration_s": 60.0,
        "bat_present": bool(dets),
        "n_detections": len(dets),
        "top_species": top_species or (dets[0]["species"] if dets else ""),
        "top_confidence": conf or (dets[0]["confidence"] if dets else 0.0),
        "detections_json": json.dumps(dets),
        "resample_hz": 256000, "model_hash": "abc", "error": "",
    }


def _clip(source, *, start_ms, end_ms, kept=True, reason=""):
    """One per-clip ReplayRow-shaped dict."""
    return {
        "source_file": source,
        "clip_wav": f"{source}.clip.wav",
        "kept": kept,
        "rejected_reason": reason,
        "rejected_mode": "all" if reason else "",
        "clip_start_ms": float(start_ms),
        "clip_end_ms":   float(end_ms),
        "clip_duration_ms": float(end_ms - start_ms),
        "n_events": 1, "n_gate_rejected_events": 0 if kept else 1,
        "min_bandwidth_khz": 0.0, "max_drift_khz": 0.0,
        "tunable_min_bw_khz": 0.9,
        "frames_processed": 0, "no_clips": False, "error": "",
    }


def _no_clip_row(source):
    return {
        "source_file": source, "clip_wav": "", "kept": False,
        "rejected_reason": "", "rejected_mode": "",
        "clip_start_ms": 0.0, "clip_end_ms": 0.0, "clip_duration_ms": 0.0,
        "n_events": 0, "n_gate_rejected_events": 0,
        "min_bandwidth_khz": 0.0, "max_drift_khz": 0.0,
        "tunable_min_bw_khz": 0.0, "frames_processed": 0,
        "no_clips": True, "error": "",
    }


# --- confusion + rates -------------------------------------------------------

def test_four_confusion_buckets_populate() -> None:
    """One clip per bucket: TP, FN, FP, TN. Verify each lands where the
    plan says it should."""
    truth = pd.DataFrame([
        _truth("bat.wav", detections=[(1.0, 1.2, "Pipistrellus", 0.9)]),
        _truth("cricket.wav"),  # no bat
    ])
    replay = pd.DataFrame([
        _clip("bat.wav",     start_ms=1000, end_ms=1200, kept=True),   # TP
        _clip("bat.wav",     start_ms=1100, end_ms=1300, kept=False,
              reason="sweep"),                                          # FN (still overlaps)
        _clip("cricket.wav", start_ms=100,  end_ms=300,  kept=True),   # FP
        _clip("cricket.wav", start_ms=400,  end_ms=600,  kept=False,
              reason="sweep"),                                          # TN
    ])
    summ = s.score(truth, replay)
    c = summ.confusion
    assert (c.n_tp, c.n_fn, c.n_fp, c.n_tn) == (1, 1, 1, 1)
    assert summ.recall                  == pytest.approx(0.5)
    assert summ.cricket_rejection_rate  == pytest.approx(0.5)


def test_overlap_slop_admits_edge_adjacent_detections() -> None:
    """Clip [1000, 1200] and detection [0.79, 0.99]s (ends 10 ms before
    clip start) should still count as bat because the slop is 20 ms."""
    truth = pd.DataFrame([
        _truth("edge.wav", detections=[(0.79, 0.99, "Pipistrellus", 0.9)]),
    ])
    replay = pd.DataFrame([
        _clip("edge.wav", start_ms=1000, end_ms=1200, kept=True),
    ])
    summ = s.score(truth, replay)
    assert summ.confusion.n_tp == 1


def test_non_overlapping_detection_does_not_credit_clip_as_bat() -> None:
    """Source WAV has a bat detection at t=1s, but the clip covers
    [5000, 5200]ms — no overlap. The clip must NOT be credited as bat
    (that's the whole reason the plan wants time-overlap)."""
    truth = pd.DataFrame([
        _truth("mixed.wav", detections=[(1.0, 1.2, "Pipistrellus", 0.9)]),
    ])
    replay = pd.DataFrame([
        _clip("mixed.wav", start_ms=5000, end_ms=5200, kept=True),
    ])
    summ = s.score(truth, replay)
    # Clip is no-bat → kept → FP, not TP.
    assert (summ.confusion.n_tp, summ.confusion.n_fp) == (0, 1)


def test_zero_denominator_yields_none_not_zero() -> None:
    """Cricket-rejection rate with no discarded-or-kept cricket clips is
    n/a, not 0.0. Same for recall with no bat clips."""
    truth  = pd.DataFrame([_truth("a.wav", detections=[(1, 1.2, "Pip", 0.9)])])
    replay = pd.DataFrame([_clip("a.wav", start_ms=1000, end_ms=1200, kept=True)])
    summ = s.score(truth, replay)
    assert summ.fp_rate is None
    assert summ.cricket_rejection_rate is None
    assert summ.recall == pytest.approx(1.0)


def test_no_clip_sources_dont_score() -> None:
    """A ``no_clips=True`` row is neither TP nor FN — the base detector
    never triggered, which is the untracked FN class the honesty ceiling
    calls out (caveat 3)."""
    truth = pd.DataFrame([
        _truth("silent_bat.wav", detections=[(1, 1.2, "Pip", 0.9)]),
    ])
    replay = pd.DataFrame([_no_clip_row("silent_bat.wav")])
    summ = s.score(truth, replay)
    assert summ.n_clips == 0
    assert summ.n_no_clip_sources == 1
    assert summ.confusion.n_fn == 0    # NOT credited as FN


# --- per-species -------------------------------------------------------------

def test_per_species_groups_split_bat_vs_no_bat() -> None:
    truth = pd.DataFrame([
        _truth("pip.wav",   detections=[(1.0, 1.2, "Pipistrellus pipistrellus", 0.9)]),
        _truth("nyc.wav",   detections=[(2.0, 2.3, "Nyctalus noctula", 0.8)]),
        _truth("empty.wav"),
    ])
    replay = pd.DataFrame([
        _clip("pip.wav",   start_ms=1000, end_ms=1200, kept=True),   # TP-Pip
        _clip("pip.wav",   start_ms=1000, end_ms=1200, kept=False,
              reason="sweep"),                                        # FN-Pip
        _clip("nyc.wav",   start_ms=2000, end_ms=2300, kept=True),   # TP-Nyc
        _clip("empty.wav", start_ms=100,  end_ms=300,  kept=True),   # FP-nobat
    ])
    df = s.score_per_species(truth, replay)
    by_group = df.set_index("group")
    assert by_group.loc["Pipistrellus pipistrellus", "recall"] == pytest.approx(0.5)
    assert by_group.loc["Nyctalus noctula",          "recall"] == pytest.approx(1.0)
    assert by_group.loc[s.NO_BAT_LABEL, "fp_rate"] == pytest.approx(1.0)


# --- disagreements -----------------------------------------------------------

def test_disagreements_flag_missed_recall_and_cricket_fp() -> None:
    truth = pd.DataFrame([
        _truth("miss.wav", detections=[(1.0, 1.2, "Pipistrellus", 0.9)]),
        _truth("cricket.wav"),
    ])
    replay = pd.DataFrame([
        _clip("miss.wav",    start_ms=1000, end_ms=1200, kept=False,
              reason="sweep"),                                        # missed_recall
        _clip("cricket.wav", start_ms=100,  end_ms=300,  kept=True),  # cricket_fp
    ])
    df = s.disagreements(truth, replay)
    categories = dict(zip(df["source_file"], df["category"]))
    assert categories["miss.wav"]    == "missed_recall"
    assert categories["cricket.wav"] == "cricket_fp"


def test_disagreements_include_cf_species_even_when_agreeing() -> None:
    """The plan calls out Rhinolophus/horseshoe as our weak spot — the
    CSV must include those rows for human review whether or not the two
    sides agreed."""
    truth = pd.DataFrame([
        _truth("rhino.wav", detections=[(1.0, 1.2,
                                         "Rhinolophus ferrumequinum", 0.9)]),
    ])
    replay = pd.DataFrame([
        _clip("rhino.wav", start_ms=1000, end_ms=1200, kept=True),
    ])
    df = s.disagreements(truth, replay)
    assert "cf_review" in set(df["category"])


def test_disagreements_sort_puts_high_confidence_missed_recall_first() -> None:
    """Category ordering (missed_recall → cricket_fp → cf_review), then
    within each, top_confidence descending — the operator wants the
    highest-signal rows at the top of the queue."""
    truth = pd.DataFrame([
        _truth("low.wav",  detections=[(1, 1.2, "Pipistrellus", 0.55)]),
        _truth("high.wav", detections=[(1, 1.2, "Pipistrellus", 0.95)]),
    ])
    replay = pd.DataFrame([
        _clip("low.wav",  start_ms=1000, end_ms=1200, kept=False, reason="sweep"),
        _clip("high.wav", start_ms=1000, end_ms=1200, kept=False, reason="sweep"),
    ])
    df = s.disagreements(truth, replay)
    assert list(df["source_file"]) == ["high.wav", "low.wav"]


# --- rendering + end-to-end --------------------------------------------------

def test_assert_basenames_unique_flags_collision() -> None:
    """Task 3 — the join keys on basename; a collision would silently
    mis-attribute every clip in the pair. The assertion is cheap
    insurance against a future corpus that violates the field
    convention (globally-unique cumulative-sample-offset prefix)."""
    df = pd.DataFrame([
        {"file": "/dir1/clip.wav"},
        {"file": "/dir2/clip.wav"},   # same basename, different dir
    ])
    with pytest.raises(ValueError, match="basename collision"):
        s.assert_basenames_unique(df, column="file", source="truth")


def test_assert_basenames_unique_ignores_repeats_within_one_source() -> None:
    """Replay manifests have one row per CLIP, so the same source_file
    repeats across the manifest. That is not a collision — only two
    *distinct* paths sharing a basename is."""
    df = pd.DataFrame([
        {"source_file": "/dir1/clip.wav"},
        {"source_file": "/dir1/clip.wav"},   # same source, different clip
        {"source_file": "/dir1/clip.wav"},
    ])
    s.assert_basenames_unique(df, column="source_file", source="replay")


def test_assert_basenames_unique_passes_when_all_unique() -> None:
    df = pd.DataFrame([
        {"file": "/dir1/clip_a.wav"},
        {"file": "/dir2/clip_b.wav"},
    ])
    s.assert_basenames_unique(df, column="file", source="truth")  # no raise


def test_join_uses_basename_not_full_path() -> None:
    """Regression: the truth manifest may store relative paths while the
    replay manifest stores absolute paths (or vice versa) depending on
    how ``--input`` was invoked. Overlap join must key on basename so a
    mixed-provenance run doesn't silently drop every clip into
    __no_bat__."""
    truth = pd.DataFrame([_truth(
        "field_samples/bat.wav",   # relative, as older truth caches store
        detections=[(1.0, 1.2, "Pipistrellus", 0.9)])])
    replay = pd.DataFrame([_clip(
        "/absolute/path/field_samples/bat.wav",   # abs, as a fresh sweep writes
        start_ms=1000, end_ms=1200, kept=True)])
    summ = s.score(truth, replay)
    assert summ.confusion.n_tp == 1, "basename join must match despite path differing"


def test_render_report_prints_all_four_caveats() -> None:
    truth  = pd.DataFrame([_truth("a.wav",
                                   detections=[(1, 1.2, "Pipistrellus", 0.9)])])
    replay = pd.DataFrame([_clip("a.wav", start_ms=1000, end_ms=1200, kept=True)])
    summ = s.score(truth, replay)
    per_sp = s.score_per_species(truth, replay)
    body = s.render_report(summ, per_sp,
                           truth_path=Path("t.parquet"),
                           replay_path=Path("r.parquet"))
    # All four points of the honesty ceiling must appear verbatim.
    assert "BatDetect2 is a screen, not truth" in body
    assert "x86 replay != ARM device"           in body
    assert "Two false-negative classes"         in body
    assert "No human adjudication yet"          in body


def test_score_and_write_end_to_end(tmp_path: Path) -> None:
    truth_rows = [
        m.TruthRow(file="a.wav", duration_s=60, bat_present=True,
                   n_detections=1, top_species="Pipistrellus",
                   top_confidence=0.9,
                   detections_json=json.dumps([{
                       "start_time_s": 1.0, "end_time_s": 1.2,
                       "low_freq_hz": 20000, "high_freq_hz": 100000,
                       "confidence": 0.9, "species": "Pipistrellus",
                   }]),
                   resample_hz=256000, model_hash="x"),
        m.TruthRow(file="b.wav", duration_s=60, bat_present=False,
                   n_detections=0, top_species="", top_confidence=0.0,
                   detections_json="[]",
                   resample_hz=256000, model_hash="x"),
    ]
    replay_rows = [
        m.ReplayRow(source_file="a.wav", clip_wav="a.clip.wav",
                    kept=True, rejected_reason="", rejected_mode="",
                    clip_start_ms=1000.0, clip_end_ms=1200.0,
                    clip_duration_ms=200.0,
                    n_events=1, n_gate_rejected_events=0,
                    min_bandwidth_khz=0.0, max_drift_khz=0.0,
                    tunable_min_bw_khz=0.9, frames_processed=900),
        m.ReplayRow(source_file="b.wav", clip_wav="b.clip.wav",
                    kept=True, rejected_reason="", rejected_mode="",
                    clip_start_ms=100.0, clip_end_ms=300.0,
                    clip_duration_ms=200.0,
                    n_events=1, n_gate_rejected_events=0,
                    min_bandwidth_khz=0.0, max_drift_khz=0.0,
                    tunable_min_bw_khz=0.9, frames_processed=225),
    ]
    truth_path  = tmp_path / "truth.parquet"
    replay_path = tmp_path / "replay.parquet"
    m.write_truth_manifest(truth_path, truth_rows)
    m.write_replay_manifest(replay_path, replay_rows)

    out = tmp_path / "out"
    summary = s.score_and_write(truth_path, replay_path, out,
                                config_label="baseline")
    assert (out / "report.md").exists()
    assert (out / "results.csv").exists()
    assert (out / "disagreements.csv").exists()
    assert summary.confusion.n_tp == 1     # a.wav bat + kept
    assert summary.confusion.n_fp == 1     # b.wav no-bat + kept
    report_body = (out / "report.md").read_text()
    assert "baseline" in report_body
    assert "BatDetect2 is a screen" in report_body


def test_render_config_diff_two_rows(tmp_path: Path) -> None:
    """The sweep report's baseline-vs-shorter table must show both rows
    with counts alongside percentages — never a bare percentage."""
    truth  = pd.DataFrame([_truth("a.wav",
                                   detections=[(1, 1.2, "Pipistrellus", 0.9)])])
    replay = pd.DataFrame([_clip("a.wav", start_ms=1000, end_ms=1200, kept=True)])
    summ = s.score(truth, replay)
    body = s.render_config_diff(["baseline", "shorter"], [summ, summ])
    assert "| baseline |" in body
    assert "| shorter |"  in body
    # Every rate cell should carry counts — grep for a slash inside parens.
    assert "(1/1)" in body