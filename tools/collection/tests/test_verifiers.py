# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for the §3 offline verifier scripts.

Runs against a synthetic session fixture — no firmware, no hardware. Each
falsifier from the verifier docstrings is exercised by mutating the
fixture and asserting the verifier returns non-zero. That way the
falsifier list isn't just documentation: if it silently stopped
falsifying, this suite would catch it.

Run with: PYTHONPATH=. python -m pytest tools/collection/tests -q
(or just point pytest at this file; the imports assume repo root is on
sys.path so the `tools.collection.*` module path resolves.)
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf


REPO_ROOT = Path(__file__).resolve().parents[3]


def _write_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(path, samples.astype(np.int16), sample_rate, subtype="PCM_16")


def _write_jsonl(path: Path, records) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        for r in records:
            f.write(json.dumps(r) + "\n")


@pytest.fixture
def synthetic_session(tmp_path: Path):
    """Build a self-consistent 2-chunk / 1-event session in tmp_path.

    - Stream A: two 500-sample chunks at 8 kHz mono int16, contiguous,
      with a square-wave content the tests can differentiate.
    - Stream B: one event WAV whose 100-sample window slices exactly
      out of Stream A chunk 1 (so A/B byte-identity passes).
    - Stream C: one event + one decision record matching that event.
    """
    session_root = tmp_path / "session"
    session_root.mkdir()

    # SESSION_HEADER — minimal but valid.
    (session_root / "SESSION_HEADER.json").write_text(json.dumps({
        "firmware_sha": "deadbeef",
        "algorithm":    "BandEnergyDetector",
        "sample_rate":  8000,
        "channels":     1,
        "streams":      {"a": True, "b": True, "c": True, "d": True},
        "start_sample": 0,
        "start_wall_iso8601": "2026-07-31T00:00:00.000Z",
    }))

    # Stream A: two 500-sample chunks. Sample values encode absolute
    # sample index so byte-identity comparisons are unambiguous.
    ref_dir = session_root / "reference"
    chunk_len = 500
    total_samples = 2 * chunk_len
    all_samples = np.arange(total_samples, dtype=np.int32).astype(np.int16)
    for i in range(2):
        start = i * chunk_len
        end   = start + chunk_len
        fname = f"{start:020d}_2026-07-31T00-00-{i:02d}.000Z.wav"
        _write_wav(ref_dir / fname, all_samples[start:end], 8000)
    manifest = [
        {
            "file": f"{0:020d}_2026-07-31T00-00-00.000Z.wav",
            "start_sample": 0,
            "end_sample":   chunk_len,
            "frames":       chunk_len,
            "wall_start_iso8601": "2026-07-31T00:00:00.000Z",
            "wall_end_iso8601":   "2026-07-31T00:00:01.000Z",
            "drops_snapshot":     0,
        },
        {
            "file": f"{chunk_len:020d}_2026-07-31T00-00-01.000Z.wav",
            "start_sample": chunk_len,
            "end_sample":   2 * chunk_len,
            "frames":       chunk_len,
            "wall_start_iso8601": "2026-07-31T00:00:01.000Z",
            "wall_end_iso8601":   "2026-07-31T00:00:02.000Z",
            "drops_snapshot":     0,
        },
    ]
    _write_jsonl(ref_dir / "chunks.jsonl", manifest)

    # Stream B: one event WAV that byte-matches Stream A [600, 700).
    event_start = 600
    event_len   = 100
    event_wav = all_samples[event_start:event_start + event_len]
    _write_wav(session_root / "events" / "accepted"
               / f"{event_start:020d}.wav", event_wav, 8000)

    # Stream C: one event + one decision record for that clip.
    _write_jsonl(session_root / "decisions.jsonl", [
        {"kind": "event", "start_sample": event_start,
         "end_sample": event_start + event_len,
         "start_frame": 1, "end_frame": 1, "duration_frames": 1, "band_index": 1,
         "trigger_snr": 10.0, "trigger_flatness": 0.5, "peak_snr": 12.0,
         "lo_hz": 20000, "hi_hz": 45000,
         "bandwidth_khz": 5, "drift_khz": 2, "path_ratio": 1.1, "mono_fraction": 0.8,
         "gate_rejected": False},
        {"kind": "decision", "clip_start_sample": 550, "clip_end_sample": 750,
         "bat_like_at_start": 0, "bat_like_at_end": 1,
         "duration_ms": 25, "event_lo_hz": 20000, "event_hi_hz": 45000,
         "saved": True, "reason": "saved"},
    ])

    (session_root / "SESSION_END.json").write_text(json.dumps({
        "end_wall_iso8601": "2026-07-31T00:00:02.000Z",
        "end_sample":       total_samples,
        "samples_captured": total_samples,
        "reason":           "shutdown-signal",
    }))

    return session_root


def _run(script: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-m", f"tools.collection.{script}", *args],
        cwd=str(REPO_ROOT),
        capture_output=True, text=True,
    )


def test_stream_a_passes_on_clean_session(synthetic_session: Path) -> None:
    r = _run("verify_stream_a", str(synthetic_session))
    assert r.returncode == 0, r.stderr
    assert "PASS" in r.stdout


def test_stream_a_flags_missing_chunk(synthetic_session: Path) -> None:
    # F1: delete one chunk file.
    ref = synthetic_session / "reference"
    victims = sorted(p for p in ref.iterdir() if p.name.endswith(".wav"))
    victims[0].unlink()
    r = _run("verify_stream_a", str(synthetic_session))
    assert r.returncode != 0
    assert "F1" in r.stderr


def test_stream_a_flags_gap_between_chunks(synthetic_session: Path,
                                            tmp_path: Path) -> None:
    # F3: rewrite manifest so chunk 2 starts 50 samples after chunk 1 ends
    # with no drops recorded.
    manifest = synthetic_session / "reference" / "chunks.jsonl"
    lines = [json.loads(l) for l in manifest.read_text().splitlines() if l.strip()]
    lines[1]["start_sample"] = lines[0]["end_sample"] + 50
    lines[1]["end_sample"]   = lines[1]["start_sample"] + lines[1]["frames"]
    manifest.write_text("\n".join(json.dumps(r) for r in lines) + "\n")
    r = _run("verify_stream_a", str(synthetic_session))
    assert r.returncode != 0
    assert "F3" in r.stderr


def test_stream_a_flags_backwards_drops(synthetic_session: Path) -> None:
    # F4: pretend chunk 0 saw 5 drops, chunk 1 saw 3 — impossible in the
    # firmware; verifier must flag.
    manifest = synthetic_session / "reference" / "chunks.jsonl"
    lines = [json.loads(l) for l in manifest.read_text().splitlines() if l.strip()]
    lines[0]["drops_snapshot"] = 5
    lines[1]["drops_snapshot"] = 3
    manifest.write_text("\n".join(json.dumps(r) for r in lines) + "\n")
    r = _run("verify_stream_a", str(synthetic_session))
    assert r.returncode != 0
    assert "F4" in r.stderr


def test_ab_identity_passes_on_clean_session(synthetic_session: Path) -> None:
    r = _run("verify_ab_identity", str(synthetic_session))
    assert r.returncode == 0, r.stderr
    assert "PASS" in r.stdout


def test_ab_identity_flags_bitflip_in_event_wav(synthetic_session: Path) -> None:
    # F2: bit-flip one sample in the event WAV.
    victim = next((synthetic_session / "events" / "accepted").iterdir())
    data, sr = sf.read(str(victim), dtype="int16", always_2d=False)
    data = np.asarray(data, dtype=np.int16).copy()
    data[10] = data[10] ^ 1
    sf.write(victim, data, sr, subtype="PCM_16")
    r = _run("verify_ab_identity", str(synthetic_session))
    assert r.returncode != 0
    assert "F2" in r.stderr


def test_ab_identity_flags_out_of_range_event(synthetic_session: Path) -> None:
    # F1: put an event WAV whose start_sample is past the last Stream A
    # chunk.
    off_end = 5000
    ev = synthetic_session / "events" / "accepted" / f"{off_end:020d}.wav"
    _write_wav(ev, np.zeros(50, dtype=np.int16), 8000)
    r = _run("verify_ab_identity", str(synthetic_session))
    assert r.returncode != 0
    assert "F1" in r.stderr
