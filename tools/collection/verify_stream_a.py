#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""§3 check 2: Stream-A continuity / no-gap audit.

Walks the chunk manifest and asserts:
  1. Every chunk exists on disk and libsndfile can open it.
  2. The WAV's frame count equals the manifest's `frames`.
  3. Consecutive chunks are contiguous in sample space (start_sample of
     chunk N+1 == end_sample of chunk N) — i.e. zero drops.
  4. `drops_snapshot` is monotonically non-decreasing across the manifest
     and, for a GREEN session, ends at zero.
  5. The final chunk's end_sample matches SESSION_END.samples_captured
     (accounts for the fact that samples captured include any drops).

Provenance ladder (see DATA_COLLECTION_IMPL_VALIDATION_PLAN §3):

  - Self-consistent: A vs its own manifest → YELLOW.
  - Combined with §3 check 3 (A/B byte-identity, separate writers): the
    two together are the GREEN-eligible pair. This script alone reports
    YELLOW-strong.

**Pre-registered falsifiers** (any hit = session FAILED, do NOT use its
audio for downstream science):

  F1. A missing chunk file listed in the manifest.
  F2. A WAV whose libsndfile frame count differs from manifest.frames.
  F3. Any chunk gap (start_sample[N+1] != end_sample[N]) with
      drops_snapshot unchanged across the gap.
  F4. `drops_snapshot` decreases across chunks (impossible in the
      firmware; would mean the log was reordered or truncated).
  F5. SESSION_END.samples_captured < last_chunk.end_sample.
  F6. The session header says Stream A was enabled, but the manifest
      lists no chunks at all. Without this the script passes VACUOUSLY on
      a session whose continuous writer never wrote a byte — which is
      exactly the failure the zero-gap check exists to catch, and the
      loudest possible version of it.

Exit code 0 = all checks pass. Non-zero = at least one falsifier hit;
stderr carries the details.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import soundfile as sf   # libsndfile via python-soundfile

from tools.collection.session_layout import Session


def verify(session_root: Path) -> int:
    session = Session.load(session_root)
    errors = []

    ref_dir = session_root / "reference"
    prev_end = None
    prev_drops = 0

    # F6: an empty manifest is not "nothing to check", it is "the writer
    # produced nothing". Only an assertion in the header that Stream A was
    # meant to run distinguishes that from a legitimately A-less session
    # (--collection-streams b,c on a small card), so key off the header.
    if session.header.get("streams", {}).get("a") and not session.chunks:
        errors.append(
            "[F6] header declares stream A enabled but reference/chunks.jsonl "
            f"lists no chunks (reference dir exists={ref_dir.is_dir()}); "
            "the continuous writer produced nothing, or the reference audio "
            "was pruned from this copy of the session"
        )

    for i, ch in enumerate(session.chunks):
        wav_path = ref_dir / ch.path
        if not wav_path.exists():
            errors.append(f"[F1] missing chunk file: {wav_path}")
            continue
        try:
            with sf.SoundFile(wav_path) as f:
                wav_frames = f.frames
        except Exception as e:
            errors.append(f"[F2] libsndfile could not open {wav_path.name}: {e}")
            continue
        if wav_frames != ch.frames:
            errors.append(
                f"[F2] {wav_path.name}: manifest.frames={ch.frames}, "
                f"WAV.frames={wav_frames}"
            )
        if ch.end_sample - ch.start_sample != ch.frames:
            errors.append(
                f"[F3] {wav_path.name}: manifest end_sample-start_sample="
                f"{ch.end_sample - ch.start_sample} ≠ frames={ch.frames}"
            )

        if prev_end is not None:
            if ch.start_sample != prev_end and ch.drops_snapshot == prev_drops:
                errors.append(
                    f"[F3] gap between chunk {i-1} and {i}: "
                    f"prev.end_sample={prev_end}, this.start_sample={ch.start_sample}, "
                    f"drops_snapshot unchanged ({prev_drops})"
                )
        if ch.drops_snapshot < prev_drops:
            errors.append(
                f"[F4] chunk {i}: drops_snapshot went backwards "
                f"({prev_drops} → {ch.drops_snapshot})"
            )
        prev_end = ch.end_sample
        prev_drops = ch.drops_snapshot

    if session.end is not None and session.chunks:
        end_sample = int(session.end.get("end_sample", 0))
        last_end = session.chunks[-1].end_sample
        if end_sample < last_end:
            errors.append(
                f"[F5] SESSION_END.end_sample={end_sample} < "
                f"last chunk.end_sample={last_end}"
            )

    if errors:
        print(f"FAIL {session_root}", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    total_frames = sum(ch.frames for ch in session.chunks)
    drops_final = session.chunks[-1].drops_snapshot if session.chunks else 0
    print(f"PASS {session_root}: "
          f"chunks={len(session.chunks)} frames={total_frames} drops={drops_final}")
    if drops_final != 0:
        print(f"  NOTE drops_snapshot={drops_final} > 0 — Stream A has gaps; "
              f"treat this session as YELLOW at best (audio is real, but "
              f"absolute-sample alignment past the first drop cannot be trusted)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("session_root", type=Path,
                    help="Path to a collection-mode session directory")
    args = ap.parse_args()
    return verify(args.session_root)


if __name__ == "__main__":
    sys.exit(main())
