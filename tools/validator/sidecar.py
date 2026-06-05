# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Sidecar JSON reader for the offline validator.

Each saved WAV ships with a `<basename>.json` sidecar carrying the per-event
diagnostic features the production detector logged at trigger/close time plus
a snapshot of its noise-floor EMA. Loading the sidecar lets ``cli run``
reproduce on-device decisions on recordings too short to converge the
floor from scratch — see ``--from-sidecar`` in cli.py.

The schema is mirrored from src/recorder/Sidecar.cpp; bump ``EXPECTED_VERSION``
in lock-step when the schema changes.
"""
from __future__ import annotations

import base64
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np


EXPECTED_VERSION = 1


@dataclass
class SidecarEvent:
    start_frame: int
    end_frame: int
    duration_frames: int
    band_index: int
    trigger_snr: float
    trigger_flatness: float
    peak_snr: float
    lo_hz: float
    hi_hz: float


@dataclass
class Sidecar:
    """Parsed sidecar JSON. Read via :func:`load`."""
    wav_path: str
    algorithm: str
    sample_rate: int
    fft_size: int
    hop_size: int
    freq_lo_hz: float
    freq_hi_hz: float
    preroll_ms: int
    silence_ms: int
    boot_ts: str
    capture_ts: str
    events_total_since_boot: int
    frames_processed: int
    tunables: Dict[str, float] = field(default_factory=dict)
    events: List[SidecarEvent] = field(default_factory=list)
    # Empty if no event fired (and so no snapshot was taken).
    noise_floor: Optional[np.ndarray] = None


def load(path: str | Path) -> Sidecar:
    """Parse a sidecar JSON. Raises ValueError on schema mismatch."""
    p = Path(path)
    with p.open() as fh:
        raw = json.load(fh)

    if raw.get("format_version") != EXPECTED_VERSION:
        raise ValueError("sidecar %s: format_version=%r, expected %d"
                         % (p, raw.get("format_version"), EXPECTED_VERSION))

    rec = raw["recording"]
    det = raw["detector"]
    dev = raw.get("device", {})
    nf  = det.get("noise_floor_at_first_event", {})

    floor = None
    data_b64 = nf.get("data", "")
    if data_b64:
        encoding = nf.get("encoding", "")
        if encoding != "float32_base64":
            raise ValueError("sidecar %s: unsupported floor encoding %r"
                             % (p, encoding))
        floor_bytes = base64.b64decode(data_b64)
        floor = np.frombuffer(floor_bytes, dtype=np.float32).copy()
        expected_n = nf.get("n_bins")
        if expected_n is not None and len(floor) != expected_n:
            raise ValueError("sidecar %s: floor n_bins=%d but data decodes to %d"
                             % (p, expected_n, len(floor)))

    events = [SidecarEvent(**e) for e in raw.get("events", [])]

    return Sidecar(
        wav_path=rec["wav_path"],
        algorithm=det.get("algorithm", ""),
        sample_rate=int(rec["sample_rate"]),
        fft_size=int(rec["fft_size"]),
        hop_size=int(rec["hop_size"]),
        freq_lo_hz=float(rec.get("freq_lo_hz", 0.0)),
        freq_hi_hz=float(rec.get("freq_hi_hz", 0.0)),
        preroll_ms=int(rec.get("preroll_ms", 0)),
        silence_ms=int(rec.get("silence_ms", 0)),
        boot_ts=str(dev.get("boot_ts", "")),
        capture_ts=str(rec.get("capture_ts", "")),
        events_total_since_boot=int(dev.get("events_total_since_boot", 0)),
        frames_processed=int(dev.get("frames_processed", 0)),
        tunables={str(k): float(v) for k, v in det.get("tunables", {}).items()},
        events=events,
        noise_floor=floor,
    )
