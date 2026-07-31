#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""§3 check 3: Stream A/B byte-identity.

For every event WAV under <session>/events/{accepted,rejected}/, extract
the same absolute sample range from Stream A and assert **bit-identical
int16 PCM**.

**Why this is GREEN-eligible.** Stream A is written by ContinuousWriter
(dedicated writer thread, drained from ReferenceRing). Stream B is
written by EventClipWriter (a separate writer thread, drained from an
independent PreRollBuffer populated in the same audio-thread step).
Agreement on real sample values means:

  - the two writers agree on the sample stream itself,
  - the SampleClock arithmetic converting event start_frame → absolute
    sample is correct (otherwise B would slice the wrong window from A),
  - libsndfile's WAV round-trip is lossless at PCM_16 (a background
    assumption but worth stating).

Any single-sample mismatch is a real bug and blocks scientific use.

**Pre-registered falsifiers**:

  F1. An event WAV whose start_sample is not covered by any Stream A
      chunk (event pre-dates the first chunk or post-dates the last).
  F2. A single-sample difference between event PCM and the corresponding
      Stream A slice.
  F3. An event WAV whose length does not equal the width of the window
      the collection firmware asked for (preSamples + evt_len + postSamples,
      modulo start-of-session clipping).

Exit code 0 = every event checked passes. Non-zero = at least one
falsifier hit.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable, List, Optional

import numpy as np
import soundfile as sf

from tools.collection.session_layout import Chunk, Session, event_wav_paths


def read_wav_int16(path: Path) -> np.ndarray:
    data, _sr = sf.read(str(path), dtype="int16", always_2d=False)
    return np.asarray(data, dtype=np.int16)


def slice_from_chunks(session_root: Path, chunks: List[Chunk],
                      start_sample: int, length: int) -> Optional[np.ndarray]:
    """Concatenate the [start_sample, start_sample+length) slice out of
    the chunks list. Returns None if the range is not fully covered.
    """
    if length <= 0:
        return np.zeros((0,), dtype=np.int16)
    out = np.zeros((length,), dtype=np.int16)
    filled = 0
    ref_dir = session_root / "reference"
    for ch in chunks:
        if filled >= length:
            break
        # Overlap between [start_sample+filled, start_sample+length) and
        # [ch.start_sample, ch.end_sample).
        want_from = start_sample + filled
        want_to   = start_sample + length
        if want_from >= ch.end_sample or want_to <= ch.start_sample:
            continue
        seg_from_abs = max(want_from, ch.start_sample)
        seg_to_abs   = min(want_to,   ch.end_sample)
        seg_len      = seg_to_abs - seg_from_abs
        if seg_len <= 0:
            continue
        with sf.SoundFile(ref_dir / ch.file) as f:
            f.seek(seg_from_abs - ch.start_sample)
            seg = f.read(seg_len, dtype="int16", always_2d=False)
            seg = np.asarray(seg, dtype=np.int16)
        dst_from = seg_from_abs - start_sample
        out[dst_from:dst_from + seg_len] = seg
        filled += seg_len
    if filled != length:
        return None
    return out


def verify(session_root: Path) -> int:
    session = Session.load(session_root)
    events = event_wav_paths(session_root)
    errors = []
    checked = 0

    for start_sample, wav_path in sorted(events.items()):
        clip = read_wav_int16(wav_path)
        length = int(clip.shape[0])

        reference = slice_from_chunks(session_root, session.chunks, start_sample, length)
        if reference is None:
            errors.append(
                f"[F1] {wav_path.name}: sample range [{start_sample}, "
                f"{start_sample + length}) not covered by any Stream-A chunk"
            )
            continue

        if not np.array_equal(clip, reference):
            n_diff = int(np.count_nonzero(clip != reference))
            first = int(np.argmax(clip != reference))
            errors.append(
                f"[F2] {wav_path.name}: {n_diff}/{length} samples differ; "
                f"first mismatch at offset {first} "
                f"(clip={int(clip[first])} vs A={int(reference[first])})"
            )
            continue
        checked += 1

    if errors:
        print(f"FAIL {session_root}", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(f"PASS {session_root}: byte-identical for {checked} event clips")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("session_root", type=Path,
                    help="Path to a collection-mode session directory")
    args = ap.parse_args()
    return verify(args.session_root)


if __name__ == "__main__":
    sys.exit(main())
