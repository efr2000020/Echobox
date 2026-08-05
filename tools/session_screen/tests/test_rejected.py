# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Rejected-sidecar ingest tests.

Covers the deterministic parts of the ``--save-rejected`` scoring path:
sidecar parsing, per-reason counts, near-threshold classification, the
optional BatDetect2 cross-reference join, and the report fragment
rendering. No ML deps.
"""
from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from tools.session_screen import rejected as R


def _write_sidecar(dir_: Path, name: str, *,
                   reason: str = "sweep",
                   mode: str = "all",
                   events: list = None,
                   tunable_min_bw: float = 0.9) -> Path:
    """Write a minimal but shape-correct sidecar. Fields the ingest
    doesn't read are populated with plausible values so the JSON is a
    realistic fixture, not a specifically-hand-tailored subset."""
    dir_.mkdir(parents=True, exist_ok=True)
    doc = {
        "format_version": 1,
        "device": {"boot_ts": "2026-08-05T20:00:00.000Z"},
        "recording": {
            "wav_path": name,
            "capture_ts": "2026-08-05T20:00:01.000Z",
            "sample_rate": 384000,
            "fft_size": 4096,
            "hop_size": 512,
            "freq_lo_hz": 20000,
            "freq_hi_hz": 192000,
            "preroll_ms": 50,
            "silence_ms": 50,
            "rejected": {"reason": reason, "mode": mode},
        },
        "detector": {
            "algorithm": "BandEnergyDetector",
            "tunables": {"min_bandwidth_khz": tunable_min_bw},
            "noise_floor_at_first_event": {
                "n_bins": 0, "encoding": "float32_base64", "data": "",
            },
        },
        "events": events if events is not None else [],
    }
    p = dir_ / (Path(name).stem + ".json")
    p.write_text(json.dumps(doc))
    return p


def _event(bw_khz: float, drift_khz: float = 0.0,
           gate_rejected: bool = True) -> dict:
    return {
        "start_frame": 0, "end_frame": 10, "duration_frames": 10,
        "band_index": 1, "trigger_snr": 15.0, "trigger_flatness": 0.3,
        "peak_snr": 15.0, "lo_hz": 40000, "hi_hz": 45000,
        "bandwidth_khz": bw_khz, "drift_khz": drift_khz,
        "path_ratio": 1.2, "mono_fraction": 0.9,
        "gate_rejected": gate_rejected,
    }


def test_empty_dir_yields_empty_frame_with_schema(tmp_path: Path) -> None:
    """No sidecars ⇒ empty DataFrame, but with the full column set so
    downstream code doesn't KeyError when the corpus has no rejects."""
    df = R.load_rejected_dir(tmp_path)
    assert df.empty
    for c in ("file", "rejected_reason", "min_bandwidth_khz",
              "tunable_min_bandwidth_khz"):
        assert c in df.columns


def test_missing_dir_returns_empty_frame(tmp_path: Path) -> None:
    """A missing --rejected-dir is a soft error: caller printed the CLI
    validation, but load_rejected_dir on a nonexistent path shouldn't
    raise — the report just omits the section."""
    df = R.load_rejected_dir(tmp_path / "does-not-exist")
    assert df.empty


def test_parses_reason_mode_and_features(tmp_path: Path) -> None:
    _write_sidecar(tmp_path / "2026-08-05", "clip1.wav",
                   reason="sweep", mode="all",
                   events=[_event(0.5), _event(0.7)])
    df = R.load_rejected_dir(tmp_path)
    assert len(df) == 1
    row = df.iloc[0]
    assert row["rejected_reason"] == "sweep"
    assert row["rejected_mode"]   == "all"
    assert row["n_events"]        == 2
    assert row["n_gate_rejected"] == 2
    # Aggregation picks the min bandwidth (the closest to the gate).
    assert row["min_bandwidth_khz"] == pytest.approx(0.5)
    assert row["tunable_min_bandwidth_khz"] == pytest.approx(0.9)


def test_summary_counts_by_reason(tmp_path: Path) -> None:
    _write_sidecar(tmp_path, "a.wav", reason="sweep",   events=[_event(0.5)])
    _write_sidecar(tmp_path, "b.wav", reason="sweep",   events=[_event(0.6)])
    _write_sidecar(tmp_path, "c.wav", reason="temporal", events=[_event(1.2)])
    df = R.load_rejected_dir(tmp_path)
    summary = R.summarise(df)
    assert summary.n_sidecars    == 3
    assert summary.by_reason.get("sweep")    == 2
    assert summary.by_reason.get("temporal") == 1


def test_near_threshold_counts_use_sidecar_tunable(tmp_path: Path) -> None:
    """Near-miss = within margin of the *sidecar's own* min_bandwidth_khz
    so a corpus with mixed tunables is still classified correctly."""
    # tunable=0.9, margin default=0.30 → near-miss window [0.60, 1.20]
    _write_sidecar(tmp_path, "near1.wav", events=[_event(0.85)],
                   tunable_min_bw=0.9)
    _write_sidecar(tmp_path, "near2.wav", events=[_event(0.65)],
                   tunable_min_bw=0.9)
    _write_sidecar(tmp_path, "far.wav",   events=[_event(0.20)],
                   tunable_min_bw=0.9)
    # tunable=1.5, margin 0.30 → window [1.20, 1.80]; 1.25 is near.
    _write_sidecar(tmp_path, "shifted.wav", events=[_event(1.25)],
                   tunable_min_bw=1.5)
    df = R.load_rejected_dir(tmp_path)
    summary = R.summarise(df)
    assert summary.n_sidecars   == 4
    assert summary.n_near_min_bw == 3


def test_truth_join_reports_bat_hits(tmp_path: Path) -> None:
    """When a truth manifest is provided, the section reports the number
    of rejected clips BatDetect2 flagged as real bats. Basename-only join
    so a different dir prefix on either side doesn't silently drop rows."""
    _write_sidecar(tmp_path, "hit.wav",  reason="sweep",   events=[_event(0.5)])
    _write_sidecar(tmp_path, "miss.wav", reason="temporal", events=[_event(1.5)])
    _write_sidecar(tmp_path, "unmatched.wav", reason="sweep", events=[_event(0.5)])
    df = R.load_rejected_dir(tmp_path)
    # Truth manifest with different dir prefixes (worst-case for the join).
    truth = pd.DataFrame([
        {"file": "/some/other/dir/hit.wav",  "bat_present": True},
        {"file": "/some/other/dir/miss.wav", "bat_present": False},
    ])
    summary = R.summarise(df, truth_join=truth)
    assert summary.n_truth_matched == 2  # hit + miss matched; unmatched didn't
    assert summary.n_bat_hit       == 1
    assert summary.by_reason_bat_hit.get("sweep") == 1


def test_render_section_is_empty_for_empty_input(tmp_path: Path) -> None:
    """The score renderer concatenates this fragment unconditionally, so
    empty-in ⇒ empty-out lets it stay side-effect-free."""
    df = R.load_rejected_dir(tmp_path)  # empty
    summary = R.summarise(df)
    assert R.render_section(summary) == ""


def test_render_section_headline_and_reason_breakdown(tmp_path: Path) -> None:
    _write_sidecar(tmp_path, "a.wav", reason="sweep",   events=[_event(0.5)])
    _write_sidecar(tmp_path, "b.wav", reason="temporal", events=[_event(1.2)])
    df = R.load_rejected_dir(tmp_path)
    summary = R.summarise(df)
    section = R.render_section(summary)
    assert "device's own near-miss population" in section
    assert "rejected clips ingested: **2**" in section
    assert "sweep=1" in section
    assert "temporal=1" in section


def test_render_section_shows_bat_hit_when_joined(tmp_path: Path) -> None:
    _write_sidecar(tmp_path, "hit.wav", reason="sweep", events=[_event(0.5)])
    df = R.load_rejected_dir(tmp_path)
    truth = pd.DataFrame([{"file": "hit.wav", "bat_present": True}])
    summary = R.summarise(df, truth_join=truth)
    section = R.render_section(summary)
    assert "BatDetect2 cross-reference" in section
    assert "real bats the gate discarded" in section


def test_malformed_sidecar_is_flagged_not_fatal(tmp_path: Path) -> None:
    _write_sidecar(tmp_path, "good.wav", events=[_event(0.5)])
    (tmp_path / "broken.json").write_text("{not-json")
    df = R.load_rejected_dir(tmp_path)
    assert len(df) == 2
    summary = R.summarise(df)
    assert summary.n_sidecars     == 2
    assert summary.n_parse_errors == 1
