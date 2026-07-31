#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""§4.1 recorder-model cross-check.

Diffs `tools/validator/recorder_model.py`'s Python state machine against
the shipping C++ firmware's actual save/discard verdicts on the same
real field audio. This is the check that would promote recorder_model
from YELLOW (Python re-implementation, only checked against itself) to
GREEN (validated against an independent implementation — the C++
Recorder — on real inputs).

**Pre-registered outcomes**:

  - **GREEN promotion**: for every real decision in the log, the model
    produces a matching prediction (same `saved` verdict, sample ranges
    overlap). Firmware and model agree on real data → the model's
    findings can be reported as validated.
  - **RED / model-wrong**: any divergence at all. §4.1's rule (per the
    plan) is "flag it, quantify the disagreement, and correct the model
    before any further use. Do not average it away." This script does
    not tolerate 99.9% agreement — every mismatch is enumerated.
  - **RED / firmware-wrong**: a model prediction that has no real
    decision in its window. Points to the recorder emitting no
    IRecorderDecisionSink call for a case it should have (a bug in the
    shipping firmware, not the model).

Correlation policy: a real decision D and a model prediction P are
considered "the same clip" iff their sample ranges overlap by at least
one sample. That's tolerant enough that a small boundary offset between
the real recorder's steady-clock silence timeout and the model's frame-
domain silence-frame count doesn't cause spurious divergences.

Requires the validator native lib to be built (`./build_dev.sh`) so
recorder_model.run_on_wav can drive the production STFT and detector
via ctypes.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Standard-library import path assumes the repo root is on sys.path.
from tools.collection.session_layout import Chunk, DecisionRecord, Session
from tools.validator import native
from tools.validator.recorder_model import (
    RecorderModelConfig, run_on_wav, SavedRecording,
)


@dataclass
class ModelPrediction:
    """One "would-save" verdict from recorder_model on one Stream-A chunk."""
    chunk_start_sample: int
    start_sample_abs:   int
    end_sample_abs:     int
    kept:               bool
    discard_reason:     str

    @classmethod
    def from_saved(cls, chunk_start: int, rec: SavedRecording,
                   hop_size: int) -> "ModelPrediction":
        return cls(
            chunk_start_sample = chunk_start,
            start_sample_abs   = chunk_start + rec.begin_frame * hop_size,
            end_sample_abs     = chunk_start + rec.end_frame   * hop_size,
            kept               = rec.kept,
            discard_reason     = rec.discard_reason,
        )


def _overlap(a_from: int, a_to: int, b_from: int, b_to: int) -> bool:
    return a_from < b_to and b_from < a_to


def _model_predictions_for(chunk: Chunk, chunk_dir: Path,
                            detector_cfg: Optional[native.DetectorConfig],
                            recorder_cfg: RecorderModelConfig,
                            hop_size: int) -> List[ModelPrediction]:
    wav = chunk_dir / chunk.file
    result = run_on_wav(str(wav),
                        detector_config=detector_cfg,
                        recorder_config=recorder_cfg,
                        hop_size=hop_size)
    # Include both saved and discarded so we can report false-negative
    # model predictions ("model would discard, firmware saved").
    out: List[ModelPrediction] = []
    for rec in result.saved + result.discarded:
        out.append(ModelPrediction.from_saved(chunk.start_sample, rec, hop_size))
    return out


def verify(session_root: Path, hop_size: int) -> int:
    session = Session.load(session_root)
    if not session.chunks:
        print("FAIL: no Stream-A chunks in session; cannot cross-check.",
              file=sys.stderr)
        return 1
    if not session.decisions:
        print("FAIL: no recorder decisions in decisions.jsonl; nothing to check.",
              file=sys.stderr)
        return 1

    sample_rate = int(session.header.get("sample_rate", 384000))
    # Recorder knobs the model needs. Pull from the header's config blob
    # so the model uses the SAME knobs the firmware was running with —
    # otherwise a divergence would be measuring config skew, not
    # implementation drift.
    hdr_cfg = session.header.get("config") or {}
    r_cfg = RecorderModelConfig(
        preroll_ms      = int(hdr_cfg.get("preroll_ms",     50)),
        silence_ms      = int(hdr_cfg.get("silence_ms",     50)),
        min_length_ms   = int(hdr_cfg.get("min_length_ms",   0)),
        max_length_ms   = int(hdr_cfg.get("max_length_ms", 200)),
        cricket_discard = bool(hdr_cfg.get("cricket_filter", True)),
    )
    d_cfg = native.DetectorConfig(sample_rate=sample_rate)

    # Model predictions across all chunks.
    predictions: List[ModelPrediction] = []
    ref_dir = session_root / "reference"
    for ch in session.chunks:
        predictions.extend(_model_predictions_for(
            ch, ref_dir, d_cfg, r_cfg, hop_size))

    # Correlate: for each real decision, find overlapping model predictions.
    unmatched_real: List[DecisionRecord] = []
    unmatched_model: List[ModelPrediction] = list(predictions)
    disagreements: List[Tuple[DecisionRecord, ModelPrediction]] = []

    for d in session.decisions:
        matches = [p for p in unmatched_model
                   if _overlap(d.clip_start_sample, d.clip_end_sample,
                                p.start_sample_abs, p.end_sample_abs)]
        if not matches:
            unmatched_real.append(d)
            continue
        # Pick the first overlapping prediction; remove from unmatched.
        p = matches[0]
        unmatched_model.remove(p)
        # Same clip — do we agree on the verdict?
        if d.saved != p.kept:
            disagreements.append((d, p))

    n_total = len(session.decisions)
    n_agree = n_total - len(unmatched_real) - len(disagreements)

    print(f"decisions:      {n_total}")
    print(f"model matches:  {n_agree}")
    print(f"disagreements:  {len(disagreements)}")
    print(f"real w/o model: {len(unmatched_real)}")
    print(f"model w/o real: {len(unmatched_model)}")

    if disagreements:
        print("\n--- disagreements (real vs model) ---")
        for d, p in disagreements:
            print(f"  clip [{d.clip_start_sample},{d.clip_end_sample}) "
                  f"real={('saved' if d.saved else d.reason)}  "
                  f"model={('saved' if p.kept else p.discard_reason)}")
    if unmatched_real:
        print("\n--- real decisions with no matching model prediction ---")
        for d in unmatched_real:
            print(f"  clip [{d.clip_start_sample},{d.clip_end_sample}) "
                  f"real={('saved' if d.saved else d.reason)}")
    if unmatched_model:
        print("\n--- model predictions with no matching real decision ---")
        for p in unmatched_model:
            print(f"  clip [{p.start_sample_abs},{p.end_sample_abs}) "
                  f"model={('saved' if p.kept else p.discard_reason)}")

    # Per the plan §4.1: any divergence blocks GREEN promotion until the
    # model is corrected. Do not average, do not tolerate.
    exit_code = 0
    if disagreements or unmatched_real or unmatched_model:
        print("\nRESULT: RED — recorder_model does NOT match the shipping "
              "firmware on this session. Do NOT report the model as validated.")
        exit_code = 1
    else:
        print("\nRESULT: GREEN — recorder_model matches the shipping firmware "
              "on every clip in this session. Model promoted from YELLOW to "
              "GREEN by this real-data cross-check.")
    return exit_code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("session_root", type=Path)
    ap.add_argument("--hop-size", type=int, default=512,
                    help="STFT hop size (must match session's --hop-size; "
                         "default matches shipping firmware)")
    args = ap.parse_args()
    if not native.is_available():
        print("FAIL: validator native lib not loaded. Build with ./build_dev.sh.",
              file=sys.stderr)
        return 1
    return verify(args.session_root, args.hop_size)


if __name__ == "__main__":
    sys.exit(main())
