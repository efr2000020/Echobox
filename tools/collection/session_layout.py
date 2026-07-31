# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""On-disk layout of a collection session, mirrored in Python.

Kept minimal on purpose: everything is derived from filesystem walks and
JSON parsing so it stays in sync with the C++ writers by construction —
the verifier scripts break if the layout drifts, which is what §3 wants.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class Chunk:
    """One row from reference/chunks.jsonl."""
    file: str
    start_sample: int
    end_sample: int
    frames: int
    wall_start_iso8601: str
    wall_end_iso8601: str
    drops_snapshot: int

    @property
    def path(self) -> str:
        return self.file  # relative to reference/


@dataclass
class EventRecord:
    """One {"kind":"event",...} line from decisions.jsonl."""
    start_sample: int
    end_sample: int
    start_frame: int
    end_frame: int
    gate_rejected: bool
    lo_hz: float
    hi_hz: float
    raw: Dict[str, object] = field(default_factory=dict)


@dataclass
class DecisionRecord:
    """One {"kind":"decision",...} line from decisions.jsonl."""
    clip_start_sample: int
    clip_end_sample: int
    duration_ms: int
    saved: bool
    reason: str
    raw: Dict[str, object] = field(default_factory=dict)


@dataclass
class Session:
    root: Path
    header: Dict
    end: Optional[Dict]
    chunks: List[Chunk]
    events: List[EventRecord]
    decisions: List[DecisionRecord]

    @classmethod
    def load(cls, root: Path) -> "Session":
        root = Path(root)
        header_path = root / "SESSION_HEADER.json"
        if not header_path.exists():
            raise FileNotFoundError(f"{header_path} missing — not a session dir")
        header = json.loads(header_path.read_text())

        end: Optional[Dict] = None
        end_path = root / "SESSION_END.json"
        if end_path.exists():
            end = json.loads(end_path.read_text())

        chunks: List[Chunk] = []
        manifest = root / "reference" / "chunks.jsonl"
        if manifest.exists():
            for line in manifest.read_text().splitlines():
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                chunks.append(Chunk(
                    file=d["file"],
                    start_sample=int(d["start_sample"]),
                    end_sample=int(d["end_sample"]),
                    frames=int(d["frames"]),
                    wall_start_iso8601=d["wall_start_iso8601"],
                    wall_end_iso8601=d["wall_end_iso8601"],
                    drops_snapshot=int(d.get("drops_snapshot", 0)),
                ))

        events: List[EventRecord] = []
        decisions: List[DecisionRecord] = []
        dl = root / "decisions.jsonl"
        if dl.exists():
            for line in dl.read_text().splitlines():
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                if d.get("kind") == "event":
                    events.append(EventRecord(
                        start_sample=int(d["start_sample"]),
                        end_sample=int(d["end_sample"]),
                        start_frame=int(d["start_frame"]),
                        end_frame=int(d["end_frame"]),
                        gate_rejected=bool(d["gate_rejected"]),
                        lo_hz=float(d["lo_hz"]),
                        hi_hz=float(d["hi_hz"]),
                        raw=d,
                    ))
                elif d.get("kind") == "decision":
                    decisions.append(DecisionRecord(
                        clip_start_sample=int(d["clip_start_sample"]),
                        clip_end_sample=int(d["clip_end_sample"]),
                        duration_ms=int(d["duration_ms"]),
                        saved=bool(d["saved"]),
                        reason=str(d["reason"]),
                        raw=d,
                    ))
        return cls(
            root=root,
            header=header,
            end=end,
            chunks=chunks,
            events=events,
            decisions=decisions,
        )


def event_wav_paths(session_root: Path) -> Dict[int, Path]:
    """Map start_sample → path for every Stream-B event WAV under
    events/{accepted,rejected}/. Used by the A/B byte-identity check.
    """
    out: Dict[int, Path] = {}
    for sub in ("accepted", "rejected"):
        d = session_root / "events" / sub
        if not d.is_dir():
            continue
        for f in d.iterdir():
            if not f.name.endswith(".wav"):
                continue
            # Filename is <20-digit-start-sample>.wav
            try:
                start_sample = int(f.name.split(".")[0])
            except ValueError:
                continue
            out[start_sample] = f
    return out
