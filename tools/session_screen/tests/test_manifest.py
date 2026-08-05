# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Round-trip tests for the parquet manifests.

Guards the schema version + column set against silent drift: if a future
change adds a field, at least one of these round-trips will fail until
the reader is bumped alongside the writer.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from tools.session_screen import manifest as m


def test_truth_manifest_round_trip(tmp_path: Path) -> None:
    rows = [
        m.TruthRow(
            file="a.wav", duration_s=60.0, bat_present=True,
            n_detections=3, top_species="Myotis daubentonii",
            top_confidence=0.87,
            detections_json=m.encode_json_list([
                {"start_time_s": 1.0, "end_time_s": 1.1,
                 "low_freq_hz": 40000, "high_freq_hz": 80000,
                 "confidence": 0.87, "species": "Myotis daubentonii"},
            ]),
            resample_hz=256000, model_hash="c4e399122bb8d99b",
        ),
        m.TruthRow(
            file="b.wav", duration_s=60.0, bat_present=False,
            n_detections=0, top_species="", top_confidence=0.0,
            detections_json="[]", resample_hz=256000,
            model_hash="c4e399122bb8d99b",
        ),
    ]
    path = tmp_path / "truth.parquet"
    m.write_truth_manifest(path, rows)

    df = m.read_truth_manifest(path)
    assert list(df["file"]) == ["a.wav", "b.wav"]
    assert bool(df.loc[0, "bat_present"]) is True
    assert int(df.loc[0, "n_detections"]) == 3
    assert df.loc[0, "top_species"] == "Myotis daubentonii"
    detections = m.decode_json_list(df.loc[0, "detections_json"])
    assert detections[0]["low_freq_hz"] == 40000


def test_replay_manifest_round_trip(tmp_path: Path) -> None:
    rows = [
        m.ReplayRow(
            file="a.wav", duration_s=60.0,
            n_would_save=2, n_would_discard=1, would_save=True,
            clip_starts_ms=m.encode_json_list([100.0, 500.0]),
            clip_ends_ms=m.encode_json_list([180.0, 600.0]),
            discard_reasons=m.encode_json_list(["cricket-gate"]),
        ),
        m.ReplayRow(
            file="b.wav", duration_s=60.0,
            n_would_save=0, n_would_discard=0, would_save=False,
            clip_starts_ms="[]", clip_ends_ms="[]", discard_reasons="[]",
        ),
    ]
    path = tmp_path / "replay.parquet"
    m.write_replay_manifest(path, rows)

    df = m.read_replay_manifest(path)
    assert list(df["file"]) == ["a.wav", "b.wav"]
    assert int(df.loc[0, "n_would_save"]) == 2
    assert bool(df.loc[0, "would_save"]) is True
    starts = m.decode_json_list(df.loc[0, "clip_starts_ms"])
    assert starts == [100.0, 500.0]


def test_stale_schema_version_is_rejected(tmp_path: Path) -> None:
    """A cache written with the wrong schema version must fail loudly on
    read rather than silently mix formats. Simulated by handwriting a
    parquet with an obviously wrong version tag."""
    import pandas as pd
    import pyarrow as pa
    import pyarrow.parquet as pq

    df = pd.DataFrame([{
        "file": "x.wav", "duration_s": 0.0, "bat_present": False,
        "n_detections": 0, "top_species": "", "top_confidence": 0.0,
        "detections_json": "[]", "resample_hz": 256000,
        "model_hash": "0000000000000000", "error": "",
    }])
    table = pa.Table.from_pandas(df, preserve_index=False)
    table = table.replace_schema_metadata({b"echobox_schema_version": b"99"})
    path = tmp_path / "stale.parquet"
    pq.write_table(table, path)

    with pytest.raises(ValueError, match="schema version"):
        m.read_truth_manifest(path)


def test_missing_columns_are_rejected(tmp_path: Path) -> None:
    """A parquet written with a smaller/differing column set — e.g. a
    hand-edited debug dump — must not silently pass through the reader."""
    import pandas as pd
    import pyarrow as pa
    import pyarrow.parquet as pq

    df = pd.DataFrame([{"file": "x.wav"}])
    table = pa.Table.from_pandas(df, preserve_index=False)
    table = table.replace_schema_metadata({b"echobox_schema_version":
                                           str(m.TRUTH_MANIFEST_SCHEMA_VERSION).encode()})
    path = tmp_path / "partial.parquet"
    pq.write_table(table, path)

    with pytest.raises(ValueError, match="missing columns"):
        m.read_truth_manifest(path)


def test_json_helpers_round_trip() -> None:
    xs = [1.0, 2.5, 3.7]
    assert m.decode_json_list(m.encode_json_list(xs)) == xs
    assert m.decode_json_list("") == []
