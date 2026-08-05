# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Parquet-backed manifests for the truth (BatDetect2) and replay (Echobox
offline harness) stages.

Kept as plain dataclass rows with explicit schema helpers so a re-run
detects a stale cache (schema change) without silently mixing formats.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path
from typing import Iterable, List, Optional

try:
    import pandas as pd
except ImportError as e:  # pragma: no cover - surfaced by validate.py at entry
    raise ImportError(
        "pandas is required for tools.session_screen. Install with "
        "'pip install -r tools/session_screen/requirements.txt'."
    ) from e


TRUTH_MANIFEST_SCHEMA_VERSION = 1
REPLAY_MANIFEST_SCHEMA_VERSION = 1


@dataclass
class TruthRow:
    """One BatDetect2 pass over one input WAV.

    ``detections_json`` is a JSON string of a list of
    ``{"start_time_s", "end_time_s", "low_freq_hz", "high_freq_hz",
    "confidence", "species"}`` records — flat enough to survive a
    parquet round-trip without a nested-type dance.
    """
    file:            str
    duration_s:      float
    bat_present:     bool
    n_detections:    int
    top_species:     str
    top_confidence:  float
    detections_json: str
    resample_hz:     int
    model_hash:      str
    error:           str = ""


@dataclass
class ReplayRow:
    """One Echobox offline-harness pass over one input WAV."""
    file:            str
    duration_s:      float
    n_would_save:    int
    n_would_discard: int
    would_save:      bool
    clip_starts_ms:  str        # JSON list[float]
    clip_ends_ms:    str        # JSON list[float]
    discard_reasons: str        # JSON list[str] (one entry per discarded clip)
    error:           str = ""


def _rows_to_frame(rows: Iterable[object], schema_version: int) -> pd.DataFrame:
    df = pd.DataFrame([asdict(r) for r in rows])
    df.attrs["schema_version"] = schema_version
    return df


def write_truth_manifest(path: Path, rows: Iterable[TruthRow]) -> None:
    """Write truth rows to a parquet file.

    Schema version rides in file-level metadata so a stale cache is
    detected on read, not silently mixed with a newer format.
    """
    df = _rows_to_frame(rows, TRUTH_MANIFEST_SCHEMA_VERSION)
    _write_parquet(df, path, schema_version=TRUTH_MANIFEST_SCHEMA_VERSION)


def write_replay_manifest(path: Path, rows: Iterable[ReplayRow]) -> None:
    df = _rows_to_frame(rows, REPLAY_MANIFEST_SCHEMA_VERSION)
    _write_parquet(df, path, schema_version=REPLAY_MANIFEST_SCHEMA_VERSION)


def read_truth_manifest(path: Path) -> pd.DataFrame:
    return _read_parquet(path, expect_version=TRUTH_MANIFEST_SCHEMA_VERSION,
                         expected_columns=[f.name for f in fields(TruthRow)])


def read_replay_manifest(path: Path) -> pd.DataFrame:
    return _read_parquet(path, expect_version=REPLAY_MANIFEST_SCHEMA_VERSION,
                         expected_columns=[f.name for f in fields(ReplayRow)])


def _write_parquet(df: pd.DataFrame, path: Path, *, schema_version: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    import pyarrow as pa
    import pyarrow.parquet as pq
    table = pa.Table.from_pandas(df, preserve_index=False)
    md = table.schema.metadata or {}
    md = {**md, b"echobox_schema_version": str(schema_version).encode()}
    table = table.replace_schema_metadata(md)
    pq.write_table(table, path)


def _read_parquet(path: Path, *, expect_version: int,
                  expected_columns: List[str]) -> pd.DataFrame:
    import pyarrow.parquet as pq
    table = pq.read_table(path)
    md = table.schema.metadata or {}
    got = md.get(b"echobox_schema_version")
    if got is None or int(got) != expect_version:
        raise ValueError(
            f"{path}: schema version {got!r} != expected {expect_version}. "
            "Re-run with --force to regenerate the cache.")
    df = table.to_pandas()
    missing = [c for c in expected_columns if c not in df.columns]
    if missing:
        raise ValueError(
            f"{path}: missing columns {missing}. Re-run with --force.")
    return df


# --- small JSON helpers so callers don't repeat the same boilerplate --------

def encode_json_list(items) -> str:
    """JSON-encode a small list for storage in a manifest column."""
    return json.dumps(list(items), separators=(",", ":"))


def decode_json_list(s: str) -> list:
    if not s:
        return []
    return json.loads(s)
