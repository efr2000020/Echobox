# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for the §4.1 recorder-model cross-check tool.

These tests exercise the *correlation logic* — the part that decides
"model prediction P matches real decision D" — without needing to run
the C++ native lib. The `run_on_wav` call inside the verifier is
monkey-patched to return canned SavedRecording lists so tests focus on
the diff/report logic, which is where the tool's own correctness lives.

Testing the native lib end-to-end requires real Stream A audio + real
decisions from a running firmware, i.e. hardware. That path is
documented as YELLOW-until-run in tools/collection/README.md and
VALIDATION_PROVENANCE.md.
"""
from __future__ import annotations

import importlib
import json
import sys
from pathlib import Path
from typing import List

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))


def _make_session(tmp_path: Path, decisions, events=None) -> Path:
    """Minimal session with a single dummy chunk so verify_recorder_model
    doesn't bail on the "no chunks" guard. The chunk's audio content is
    irrelevant because we monkey-patch run_on_wav in the verifier."""
    root = tmp_path / "session"
    root.mkdir()
    (root / "SESSION_HEADER.json").write_text(json.dumps({
        "firmware_sha": "test",
        "algorithm":    "BandEnergyDetector",
        "sample_rate":  8000,
        "channels":     1,
        "streams":      {"a": True, "b": True, "c": True, "d": True},
        "start_sample": 0,
        "start_wall_iso8601": "2026-07-31T00:00:00.000Z",
        "config": {
            "preroll_ms": 50, "silence_ms": 50, "min_length_ms": 0,
            "max_length_ms": 200, "cricket_filter": True,
        },
    }))
    ref = root / "reference"
    ref.mkdir()
    import numpy as np
    import soundfile as sf
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
    for e in (events or []):
        lines.append({"kind": "event", **e})
    for d in decisions:
        lines.append({"kind": "decision", **d})
    (root / "decisions.jsonl").write_text("\n".join(json.dumps(l) for l in lines) + "\n")
    return root


class FakeSavedRecording:
    """Tiny shape-compatible stand-in for recorder_model.SavedRecording."""
    def __init__(self, begin_frame, end_frame, kept, reason=""):
        self.begin_frame = begin_frame
        self.end_frame   = end_frame
        self.kept        = kept
        self.discard_reason = reason


class FakeResult:
    def __init__(self, saved, discarded):
        self.saved = saved
        self.discarded = discarded


@pytest.fixture
def patched_verifier(monkeypatch):
    """Load the verifier module, then swap its `run_on_wav` for a stub
    that returns whatever the test wants. Returns a callable that
    accepts the canned `SavedRecording` list per invocation."""
    import tools.validator.native as native
    # Pretend the native lib is loaded so verify() proceeds past the
    # main() gate; verify() itself doesn't call any native lib code once
    # run_on_wav is patched.
    monkeypatch.setattr(native, "is_available", lambda: True)
    # Prevent Detector construction on unit-test hosts w/o the .so.
    monkeypatch.setattr(native, "_require_lib", lambda: None)
    module = importlib.reload(importlib.import_module(
        "tools.collection.verify_recorder_model"))

    def install(cans):
        it = iter(cans)
        def _fake_run_on_wav(path, **kwargs):
            return next(it)
        monkeypatch.setattr(module, "run_on_wav", _fake_run_on_wav)
        return module
    return install


def test_all_agree_returns_green(tmp_path: Path, patched_verifier) -> None:
    module = patched_verifier([
        FakeResult(
            saved=[FakeSavedRecording(begin_frame=10, end_frame=20, kept=True)],
            discarded=[],
        )
    ])
    root = _make_session(tmp_path, decisions=[{
        "clip_start_sample": 10 * 512, "clip_end_sample": 20 * 512,
        "bat_like_at_start": 0, "bat_like_at_end": 1,
        "duration_ms": 20, "event_lo_hz": 20000, "event_hi_hz": 45000,
        "saved": True, "reason": "saved",
    }])
    rc = module.verify(root, hop_size=512)
    assert rc == 0


def test_verdict_mismatch_returns_red(tmp_path: Path, patched_verifier) -> None:
    # Real recorder saved; model would have cricket-discarded → RED.
    module = patched_verifier([
        FakeResult(
            saved=[],
            discarded=[FakeSavedRecording(10, 20, kept=False, reason="cricket-gate")],
        )
    ])
    root = _make_session(tmp_path, decisions=[{
        "clip_start_sample": 10 * 512, "clip_end_sample": 20 * 512,
        "bat_like_at_start": 0, "bat_like_at_end": 1,
        "duration_ms": 20, "event_lo_hz": 20000, "event_hi_hz": 45000,
        "saved": True, "reason": "saved",
    }])
    rc = module.verify(root, hop_size=512)
    assert rc == 1


def test_real_decision_without_model_prediction_flags_red(
        tmp_path: Path, patched_verifier) -> None:
    module = patched_verifier([FakeResult(saved=[], discarded=[])])
    root = _make_session(tmp_path, decisions=[{
        "clip_start_sample": 10 * 512, "clip_end_sample": 20 * 512,
        "bat_like_at_start": 0, "bat_like_at_end": 1,
        "duration_ms": 20, "event_lo_hz": 20000, "event_hi_hz": 45000,
        "saved": True, "reason": "saved",
    }])
    rc = module.verify(root, hop_size=512)
    assert rc == 1


def test_model_prediction_without_real_decision_flags_red(
        tmp_path: Path, patched_verifier) -> None:
    # Model saw a clip at [10*512, 20*512); real decisions log is empty.
    module = patched_verifier([
        FakeResult(
            saved=[FakeSavedRecording(10, 20, kept=True)],
            discarded=[],
        )
    ])
    # Need at least one real decision to avoid the "nothing to check" gate.
    # Put a decision that does NOT overlap the model prediction.
    root = _make_session(tmp_path, decisions=[{
        "clip_start_sample": 100 * 512, "clip_end_sample": 110 * 512,
        "bat_like_at_start": 0, "bat_like_at_end": 1,
        "duration_ms": 20, "event_lo_hz": 20000, "event_hi_hz": 45000,
        "saved": True, "reason": "saved",
    }])
    rc = module.verify(root, hop_size=512)
    assert rc == 1
