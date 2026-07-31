# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for the §4.2 feature-parity tool.

Focuses on the correlation + boundary-proximity logic — the parts that
run on ANY host and produce the machine-readable output
`verify_recorder_model.py` reads. The full end-to-end run
(x86 harness driving the .so against a real ARM-device session)
requires the bench rehearsal and is documented as YELLOW until then in
VALIDATION_PROVENANCE.md.
"""
from __future__ import annotations

import importlib
import json
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))


class FakeEventFeatures:
    """Shape-compatible with native.EventFeatures for the matcher."""
    def __init__(self, start_frame, end_frame=None, bandwidth_khz=1.5,
                 drift_khz=1.0, path_ratio=1.0, mono_fraction=1.0,
                 gate_rejected=False):
        self.start_frame     = start_frame
        self.end_frame       = end_frame if end_frame is not None else start_frame
        self.duration_frames = self.end_frame - start_frame
        self.band_index      = 1
        self.trigger_snr     = 10.0
        self.trigger_flatness = 0.5
        self.peak_snr        = 12.0
        self.lo_hz           = 20000
        self.hi_hz           = 45000
        self.bandwidth_khz   = bandwidth_khz
        self.drift_khz       = drift_khz
        self.path_ratio      = path_ratio
        self.mono_fraction   = mono_fraction
        self.gate_rejected   = gate_rejected


def _make_session(tmp_path: Path, device_events, header=None) -> Path:
    """Session with a single 8000-sample chunk (no audio content of
    interest — the harness output is stubbed) and the given device
    event log."""
    root = tmp_path / "session"
    root.mkdir()
    (root / "SESSION_HEADER.json").write_text(json.dumps(header or {
        "sample_rate":  8000, "channels": 1,
        "start_sample": 0,
        "start_wall_iso8601": "2026-07-31T00:00:00.000Z",
    }))
    ref = root / "reference"; ref.mkdir()
    sf.write(ref / f"{0:020d}_dummy.wav",
             np.zeros(8000, dtype=np.int16), 8000, subtype="PCM_16")
    (ref / "chunks.jsonl").write_text(json.dumps({
        "file": f"{0:020d}_dummy.wav",
        "start_sample": 0, "end_sample": 8000, "frames": 8000,
        "wall_start_iso8601": "2026-07-31T00:00:00.000Z",
        "wall_end_iso8601":   "2026-07-31T00:00:01.000Z",
        "drops_snapshot": 0,
    }) + "\n")
    lines = []
    for e in device_events:
        lines.append({"kind": "event", **e})
    (root / "decisions.jsonl").write_text("\n".join(json.dumps(l) for l in lines) + "\n")
    return root


@pytest.fixture
def parity(monkeypatch):
    """Load verify_feature_parity with the native lib + harness stubbed."""
    import tools.validator.native as native
    monkeypatch.setattr(native, "is_available", lambda: True)
    monkeypatch.setattr(native, "_require_lib", lambda: None)
    module = importlib.reload(importlib.import_module(
        "tools.collection.verify_feature_parity"))

    def install_harness_events(harness_events_per_chunk):
        """harness_events_per_chunk: list-of-lists, one entry per chunk."""
        it = iter(harness_events_per_chunk)
        def _fake_run(chunk, ref_dir, d_cfg, hop_size):
            return next(it)
        monkeypatch.setattr(module, "_harness_events_for_chunk", _fake_run)
        return module
    return install_harness_events


def test_cv_series_matches_hand_derivation() -> None:
    from tools.collection.verify_feature_parity import compute_cv_series
    # Perfectly metronomic sequence: CV(IDI) = 0 for every event.
    cv = compute_cv_series([0, 10, 20, 30, 40], window=4)
    for v in cv.values():
        assert v == pytest.approx(0.0, abs=1e-9)
    # Alternating IDI 10/20: mean=15, stdev=5 (population), CV=1/3.
    cv = compute_cv_series([0, 10, 30, 40, 60, 70], window=8)
    # First event (start_frame=0) has no CV. Last event with 5 IDI:
    # [10, 20, 10, 20, 10] → mean=14, pstdev=sqrt((3*16+2*36)/5)=sqrt(24)≈4.899
    last_cv = cv[70]
    assert last_cv == pytest.approx(4.899 / 14.0, abs=1e-3)


def test_perfect_agreement_returns_green(tmp_path: Path, parity) -> None:
    device_events = [
        {"start_sample": 2 * 512, "end_sample": 4 * 512,
         "start_frame": 2, "end_frame": 4, "duration_frames": 5,
         "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
         "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
         "bandwidth_khz": 2.5, "drift_khz": 1.0, "path_ratio": 1.0,
         "mono_fraction": 1.0, "gate_rejected": False},
        {"start_sample": 8 * 512, "end_sample": 10 * 512,
         "start_frame": 8, "end_frame": 10, "duration_frames": 5,
         "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
         "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
         "bandwidth_khz": 3.0, "drift_khz": 1.2, "path_ratio": 1.1,
         "mono_fraction": 0.9, "gate_rejected": False},
    ]
    root = _make_session(tmp_path, device_events)
    # Harness fires the SAME features at the same start_frames.
    module = parity([[
        FakeEventFeatures(start_frame=2, end_frame=4,
                          bandwidth_khz=2.5, drift_khz=1.0,
                          path_ratio=1.0, mono_fraction=1.0),
        FakeEventFeatures(start_frame=8, end_frame=10,
                          bandwidth_khz=3.0, drift_khz=1.2,
                          path_ratio=1.1, mono_fraction=0.9),
    ]])
    rc, report = module.verify(root, max_delta=0.05,
                                match_tolerance_samples=256,
                                hop_size=512, write_json=False)
    assert rc == 0
    assert report.matched_events == 2
    assert report.unmatched_device == 0
    assert report.unmatched_harness == 0
    for name in ("bandwidth_khz", "drift_khz", "path_ratio", "mono_fraction"):
        assert report.features[name]["max"] == pytest.approx(0.0, abs=1e-6)


def test_delta_beyond_tolerance_fails(tmp_path: Path, parity) -> None:
    device_events = [{
        "start_sample": 2 * 512, "end_sample": 4 * 512,
        "start_frame": 2, "end_frame": 4, "duration_frames": 5,
        "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
        "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
        "bandwidth_khz": 2.5, "drift_khz": 1.0, "path_ratio": 1.0,
        "mono_fraction": 1.0, "gate_rejected": False,
    }]
    root = _make_session(tmp_path, device_events)
    # Harness fires the same event but bandwidth drifts by 0.2 kHz.
    module = parity([[
        FakeEventFeatures(start_frame=2, end_frame=4, bandwidth_khz=2.7,
                          drift_khz=1.0, path_ratio=1.0, mono_fraction=1.0),
    ]])
    rc, report = module.verify(root, max_delta=0.05,
                                match_tolerance_samples=256,
                                hop_size=512, write_json=False)
    assert rc == 1
    assert report.features["bandwidth_khz"]["max"] == pytest.approx(0.2, abs=1e-6)


def test_boundary_proximity_flags_events_near_thresholds(
        tmp_path: Path, parity) -> None:
    # Two device events: one bandwidth=0.92 (within 0.05 of 0.9),
    # one bandwidth=3.0 (far from any threshold).
    device_events = [
        {"start_sample": 2 * 512, "end_sample": 4 * 512,
         "start_frame": 2, "end_frame": 4, "duration_frames": 5,
         "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
         "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
         "bandwidth_khz": 0.92, "drift_khz": 1.0, "path_ratio": 1.0,
         "mono_fraction": 1.0, "gate_rejected": False},
        {"start_sample": 8 * 512, "end_sample": 10 * 512,
         "start_frame": 8, "end_frame": 10, "duration_frames": 5,
         "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
         "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
         "bandwidth_khz": 3.0, "drift_khz": 1.2, "path_ratio": 1.1,
         "mono_fraction": 0.9, "gate_rejected": False},
    ]
    root = _make_session(tmp_path, device_events)
    # Harness values identical → no delta failure, but boundary-proximity
    # still flags the first event.
    module = parity([[
        FakeEventFeatures(start_frame=2, end_frame=4, bandwidth_khz=0.92,
                          drift_khz=1.0, path_ratio=1.0, mono_fraction=1.0),
        FakeEventFeatures(start_frame=8, end_frame=10, bandwidth_khz=3.0,
                          drift_khz=1.2, path_ratio=1.1, mono_fraction=0.9),
    ]])
    rc, report = module.verify(root, max_delta=0.05,
                                match_tolerance_samples=256,
                                hop_size=512, write_json=False)
    # Exit 0 — deltas are within tolerance. The boundary-proximity count
    # is informational: YELLOW-with-quantified-drift, not a fail.
    assert rc == 0
    bp = report.boundary_proximity["bandwidth_khz"]
    assert bp["per_threshold"]["0.90"] == 1
    assert bp["total_flippable"] == 1


def test_writes_machine_readable_json(tmp_path: Path, parity) -> None:
    device_events = [{
        "start_sample": 2 * 512, "end_sample": 4 * 512,
        "start_frame": 2, "end_frame": 4, "duration_frames": 5,
        "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
        "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
        "bandwidth_khz": 2.5, "drift_khz": 1.0, "path_ratio": 1.0,
        "mono_fraction": 1.0, "gate_rejected": False,
    }]
    root = _make_session(tmp_path, device_events)
    module = parity([[FakeEventFeatures(start_frame=2, end_frame=4,
                                         bandwidth_khz=2.5, drift_khz=1.0,
                                         path_ratio=1.0, mono_fraction=1.0)]])
    module.verify(root, max_delta=0.05, match_tolerance_samples=256,
                   hop_size=512, write_json=True)
    report_path = root / "parity_report.json"
    assert report_path.exists()
    payload = json.loads(report_path.read_text())
    assert payload["matched_events"] == 1
    assert "bandwidth_khz" in payload["features"]
    assert "bandwidth_khz" in payload["boundary_proximity"]


def test_unmatched_events_reported(tmp_path: Path, parity) -> None:
    # Device sees an event the harness does not (and vice versa).
    device_events = [{
        "start_sample": 2 * 512, "end_sample": 4 * 512,
        "start_frame": 2, "end_frame": 4, "duration_frames": 5,
        "band_index": 1, "trigger_snr": 10, "trigger_flatness": 0.5,
        "peak_snr": 12, "lo_hz": 20000, "hi_hz": 45000,
        "bandwidth_khz": 2.5, "drift_khz": 1.0, "path_ratio": 1.0,
        "mono_fraction": 1.0, "gate_rejected": False,
    }]
    root = _make_session(tmp_path, device_events)
    # Harness fires a different event at a totally different frame.
    module = parity([[FakeEventFeatures(start_frame=500, end_frame=505,
                                         bandwidth_khz=2.5)]])
    rc, report = module.verify(root, max_delta=0.05,
                                match_tolerance_samples=256,
                                hop_size=512, write_json=False)
    # Zero matched → max delta stays 0.0 → tool passes on tolerance,
    # but the counts surface the disagreement for callers to act on.
    assert report.matched_events == 0
    assert report.unmatched_device == 1
    assert report.unmatched_harness == 1
