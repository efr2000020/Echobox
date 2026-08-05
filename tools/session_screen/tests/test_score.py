# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Scorer tests.

The ML model isn't tested here — that's BatDetect2's own suite's job. We
test the deterministic pandas layer: recall/FP math, per-species
grouping, CF-review inclusion, and the disagreements CSV.
"""
from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from tools.session_screen import manifest as m
from tools.session_screen import score as s


def _truth_df(rows):
    return pd.DataFrame(rows)


def _replay_df(rows):
    return pd.DataFrame(rows)


def _truth(file, bat, species="", conf=0.0, error=""):
    return {
        "file": file, "duration_s": 60.0, "bat_present": bat,
        "n_detections": 1 if bat else 0,
        "top_species": species, "top_confidence": conf,
        "detections_json": "[]", "resample_hz": 256000,
        "model_hash": "abc", "error": error,
    }


def _replay(file, kept, n_save=1, n_disc=0, reasons=None, error=""):
    return {
        "file": file, "duration_s": 60.0,
        "n_would_save": n_save if kept else 0,
        "n_would_discard": n_disc, "would_save": kept,
        "clip_starts_ms": "[]", "clip_ends_ms": "[]",
        "discard_reasons": m.encode_json_list(reasons or []),
        "error": error,
    }


def test_headline_confusion_and_rates() -> None:
    truth = _truth_df([
        _truth("tp.wav", True,  species="Pipistrellus", conf=0.9),
        _truth("fn.wav", True,  species="Pipistrellus", conf=0.7),
        _truth("fp.wav", False),
        _truth("tn.wav", False),
    ])
    replay = _replay_df([
        _replay("tp.wav", kept=True),
        _replay("fn.wav", kept=False, n_save=0, n_disc=1, reasons=["cricket-gate"]),
        _replay("fp.wav", kept=True),
        _replay("tn.wav", kept=False),
    ])
    summ = s.score(truth, replay)
    assert (summ.n_tp, summ.n_fn, summ.n_fp, summ.n_tn) == (1, 1, 1, 1)
    assert summ.recall  == pytest.approx(0.5)
    assert summ.fp_rate == pytest.approx(0.5)


def test_zero_denominator_yields_none_not_zero() -> None:
    """When there are no bat files, recall is *n/a*, not 0.0 — 'saw 0 of
    0' is not a failure rate. Same for cricket-FP with no non-bat files."""
    truth  = _truth_df([_truth("a.wav", True), _truth("b.wav", True)])
    replay = _replay_df([_replay("a.wav", True), _replay("b.wav", True)])
    summ = s.score(truth, replay)
    assert summ.fp_rate is None
    assert summ.recall  == pytest.approx(1.0)


def test_errors_count_but_do_not_score() -> None:
    """A row with an error on either side is excluded from TP/FP counts
    (silently pretending 'harness crashed' == 'would not save' would
    corrupt the recall number)."""
    truth = _truth_df([
        _truth("ok.wav", True, species="Myotis"),
        _truth("bad.wav", False, error="oom"),
    ])
    replay = _replay_df([
        _replay("ok.wav",  kept=True),
        _replay("bad.wav", kept=False),
    ])
    summ = s.score(truth, replay)
    assert summ.n_errors == 1
    assert summ.n_files  == 1
    assert summ.n_tp     == 1


def test_per_species_groups_non_bat_files_separately() -> None:
    truth = _truth_df([
        _truth("a.wav", True,  species="Pipistrellus pipistrellus", conf=0.9),
        _truth("b.wav", True,  species="Pipistrellus pipistrellus", conf=0.8),
        _truth("c.wav", True,  species="Nyctalus noctula",          conf=0.7),
        _truth("d.wav", False),
        _truth("e.wav", False),
    ])
    replay = _replay_df([
        _replay("a.wav", True),
        _replay("b.wav", False),
        _replay("c.wav", True),
        _replay("d.wav", True),   # cricket FP
        _replay("e.wav", False),
    ])
    df = s.score_per_species(truth, replay)
    groups = dict(zip(df["group"], df["recall"]))
    assert "Pipistrellus pipistrellus" in groups
    assert groups["Pipistrellus pipistrellus"] == pytest.approx(0.5)
    assert groups["Nyctalus noctula"]          == pytest.approx(1.0)
    assert "__no_bat__" in groups
    no_bat = df[df["group"] == "__no_bat__"].iloc[0]
    assert no_bat["fp_rate"] == pytest.approx(0.5)


def test_disagreements_include_cf_species_even_when_agreeing() -> None:
    """The plan calls out Rhinolophus/horseshoe as our weak spot — the
    CSV must include those rows for human review whether or not the two
    sides agreed."""
    truth = _truth_df([
        _truth("agree.wav",   True,  species="Pipistrellus", conf=0.9),
        _truth("rhino.wav",   True,  species="Rhinolophus ferrumequinum", conf=0.9),
        _truth("horse.wav",   True,  species="Greater horseshoe bat",     conf=0.8),
        _truth("miss.wav",    True,  species="Myotis", conf=0.7),
    ])
    replay = _replay_df([
        _replay("agree.wav", True),
        _replay("rhino.wav", True),   # agree
        _replay("horse.wav", True),   # agree
        _replay("miss.wav",  False),  # disagree
    ])
    df = s.disagreements(truth, replay)
    files = set(df["file"])
    assert "rhino.wav" in files
    assert "horse.wav" in files
    assert "miss.wav"  in files
    assert "agree.wav" not in files
    # CF rows are tagged so a reader can filter cleanly.
    cf_rows = df[df["file"].isin({"rhino.wav", "horse.wav"})]
    assert (cf_rows["category"] == "cf_review").all()


def test_disagreements_categorises_missed_recall_and_cricket_fp() -> None:
    truth = _truth_df([
        _truth("miss.wav", True,  species="Pipistrellus", conf=0.9),
        _truth("fp.wav",   False),
    ])
    replay = _replay_df([
        _replay("miss.wav", False),
        _replay("fp.wav",   True),
    ])
    df = s.disagreements(truth, replay).set_index("file")
    assert df.loc["miss.wav", "category"] == "missed_recall"
    assert df.loc["fp.wav",   "category"] == "cricket_fp"


def test_disagreements_empty_when_all_agree_and_no_cf(tmp_path: Path) -> None:
    """The apply(axis=1) path pandas takes on an empty frame returns a
    DataFrame, not a Series — this exercises the guard we put in for it."""
    truth  = _truth_df([_truth("a.wav", False), _truth("b.wav", True,
                                                       species="Pipistrellus")])
    replay = _replay_df([_replay("a.wav", False), _replay("b.wav", True)])
    df = s.disagreements(truth, replay)
    assert df.empty
    assert "category" in df.columns


def test_render_report_prints_both_caveats() -> None:
    truth  = _truth_df([_truth("a.wav", True)])
    replay = _replay_df([_replay("a.wav", True)])
    summ = s.score(truth, replay)
    per_sp = s.score_per_species(truth, replay)
    body = s.render_report(summ, per_sp,
                           truth_path=Path("t.parquet"),
                           replay_path=Path("r.parquet"))
    assert "rough tuning proxy" in body.lower()
    assert "BatDetect2 is NOT ground truth" in body
    assert "ARM" in body


def test_score_and_write_end_to_end(tmp_path: Path) -> None:
    """Wire truth + replay → parquet → score.score_and_write in one go."""
    truth_rows = [
        m.TruthRow(file="a.wav", duration_s=60, bat_present=True,
                   n_detections=1, top_species="Pipistrellus",
                   top_confidence=0.9, detections_json="[]",
                   resample_hz=256000, model_hash="x"),
        m.TruthRow(file="b.wav", duration_s=60, bat_present=False,
                   n_detections=0, top_species="", top_confidence=0.0,
                   detections_json="[]",
                   resample_hz=256000, model_hash="x"),
    ]
    replay_rows = [
        m.ReplayRow(file="a.wav", duration_s=60, n_would_save=1,
                    n_would_discard=0, would_save=True,
                    clip_starts_ms="[]", clip_ends_ms="[]",
                    discard_reasons="[]"),
        m.ReplayRow(file="b.wav", duration_s=60, n_would_save=1,
                    n_would_discard=0, would_save=True,
                    clip_starts_ms="[]", clip_ends_ms="[]",
                    discard_reasons="[]"),
    ]
    truth_path  = tmp_path / "truth.parquet"
    replay_path = tmp_path / "replay.parquet"
    m.write_truth_manifest(truth_path, truth_rows)
    m.write_replay_manifest(replay_path, replay_rows)

    out = tmp_path / "out"
    summary = s.score_and_write(truth_path, replay_path, out)
    assert (out / "report.md").exists()
    assert (out / "results.csv").exists()
    assert (out / "disagreements.csv").exists()
    assert summary.n_tp == 1
    assert summary.n_fp == 1
    report_body = (out / "report.md").read_text()
    assert "rough tuning proxy" in report_body.lower()
