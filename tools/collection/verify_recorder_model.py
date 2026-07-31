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
  - **RED / model-wrong**: any divergence that CANNOT be explained by
    boundary-proximity numerical drift. §4.1's rule (per the plan) is
    "flag it, quantify the disagreement, and correct the model before
    any further use. Do not average it away." This script does not
    apply a blanket percentage tolerance — every unexplained mismatch
    is enumerated.
  - **YELLOW / boundary-drift**: a divergence where at least one event
    in the clip window has a gate-relevant feature within the parity
    check's `max_delta` of a threshold (`min_bandwidth_khz = 0.9`,
    `rep_cv_min = 0.50`, `rep_cv_max = 1.30`). The x86 harness ↔ ARM
    device numerical drift is enough to flip that event's verdict, so
    the divergence is *consistent with* numerical noise rather than a
    model-logic bug. Reported separately, does NOT count toward RED.
  - **RED / firmware-wrong**: a model prediction that has no real
    decision in its window. Points to the recorder emitting no
    IRecorderDecisionSink call for a case it should have (a bug in the
    shipping firmware, not the model).

**Boundary-proximity input**: this tool reads
`<session_root>/parity_report.json` (produced by
`verify_feature_parity.py`) if present, and takes its per-feature max
delta as the "flippable" bound. If no parity report exists the tool
falls back to a conservative default and warns in the output.

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
from tools.collection.session_layout import Chunk, DecisionRecord, EventRecord, Session
from tools.collection.verify_feature_parity import (
    DEFAULT_THRESHOLDS, compute_cv_series,
)
from tools.validator import native
from tools.validator.recorder_model import (
    RecorderModelConfig, run_on_wav, SavedRecording,
)


# Fallback delta when no parity_report.json is present — deliberately
# larger than the parity tool's default so the annotation flags MORE
# events as boundary-proximate rather than fewer. The right value to
# use here is whatever `verify_feature_parity.py` measured; this is
# only a "no parity data available, be conservative" backstop.
FALLBACK_MAX_DELTA = 0.10


def _load_parity_max_delta(session_root: Path) -> Tuple[float, str]:
    """Read the max feature delta from parity_report.json if present.
    Returns (delta, source_description). Falls back to FALLBACK_MAX_DELTA
    when the report is missing — that path is announced to the user.
    """
    p = session_root / "parity_report.json"
    if not p.exists():
        return FALLBACK_MAX_DELTA, (
            f"no parity_report.json at {p} — using conservative fallback "
            f"({FALLBACK_MAX_DELTA}). Run verify_feature_parity.py first "
            "for an accurate bound; row 4b's promotion is a prerequisite "
            "for row 4a's promotion to GREEN."
        )
    try:
        data = json.loads(p.read_text())
    except (OSError, json.JSONDecodeError) as e:
        return FALLBACK_MAX_DELTA, f"could not parse {p}: {e}"
    # Take the largest max across all features — the tolerance applied
    # to any single divergence must cover the worst per-feature drift so
    # a mismatch on a mixed-feature threshold isn't accidentally called
    # unexplained.
    max_delta = 0.0
    for name, stats in (data.get("features") or {}).items():
        max_delta = max(max_delta, float(stats.get("max", 0.0)))
    if max_delta == 0.0:
        return FALLBACK_MAX_DELTA, f"parity_report.json has zero deltas; using {FALLBACK_MAX_DELTA} as bound"
    return max_delta, f"max delta from {p} = {max_delta:.4f}"


def _events_in_clip(events: List[EventRecord],
                    clip_start: int, clip_end: int) -> List[EventRecord]:
    return [e for e in events
            if e.start_sample < clip_end and e.end_sample > clip_start]


def _event_boundary_hits(events: List[EventRecord],
                         max_delta: float) -> List[str]:
    """For each event, return human-readable descriptions of every
    gate-relevant feature that is within ``max_delta`` of a threshold
    (i.e. whose verdict could flip under the measured x86↔ARM drift).
    Empty list = no boundary hits → divergence cannot be explained as
    numerical drift.
    """
    hits: List[str] = []
    # Compute CV(IDI) for the whole event set with the same helper the
    # parity check uses. Keyed by event start_frame.
    cv_by_frame = compute_cv_series([e.start_frame for e in events])
    for e in events:
        bw = float(e.raw.get("bandwidth_khz", 0.0))
        for th in DEFAULT_THRESHOLDS.get("bandwidth_khz", []):
            if abs(bw - th) < max_delta:
                hits.append(
                    f"start_sample={e.start_sample} bandwidth_khz={bw:.3f} "
                    f"within {max_delta} of threshold {th}"
                )
        cv = cv_by_frame.get(e.start_frame)
        if cv is None:
            continue
        for th in DEFAULT_THRESHOLDS.get("cv_idi", []):
            if abs(cv - th) < max_delta:
                hits.append(
                    f"start_sample={e.start_sample} cv_idi={cv:.3f} "
                    f"within {max_delta} of threshold {th}"
                )
    return hits


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

    # Load the parity-check tolerance up front so every divergence is
    # classified against the same bound.
    max_delta, delta_source = _load_parity_max_delta(session_root)
    print(f"boundary tolerance: {delta_source}")

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

    # Classify each disagreement: boundary-drift (explained by measured
    # x86↔ARM numerical drift near a gate threshold) or unexplained
    # (real model-logic bug). Only unexplained divergences count as RED.
    boundary_drift: List[Tuple[DecisionRecord, ModelPrediction, List[str]]] = []
    unexplained:    List[Tuple[DecisionRecord, ModelPrediction]] = []
    for d, p in disagreements:
        clip_events = _events_in_clip(session.events,
                                       d.clip_start_sample, d.clip_end_sample)
        hits = _event_boundary_hits(clip_events, max_delta)
        if hits:
            boundary_drift.append((d, p, hits))
        else:
            unexplained.append((d, p))

    n_total = len(session.decisions)
    n_agree = n_total - len(unmatched_real) - len(disagreements)

    print(f"decisions:              {n_total}")
    print(f"model matches:          {n_agree}")
    print(f"disagreements (total):  {len(disagreements)}")
    print(f"  boundary-drift:       {len(boundary_drift)}")
    print(f"  unexplained (RED):    {len(unexplained)}")
    print(f"real w/o model:         {len(unmatched_real)}")
    print(f"model w/o real:         {len(unmatched_model)}")

    if boundary_drift:
        print("\n--- boundary-drift divergences (explained by x86↔ARM numerical drift) ---")
        for d, p, hits in boundary_drift:
            print(f"  clip [{d.clip_start_sample},{d.clip_end_sample}) "
                  f"real={('saved' if d.saved else d.reason)}  "
                  f"model={('saved' if p.kept else p.discard_reason)}")
            for h in hits:
                print(f"      {h}")
    if unexplained:
        print("\n--- unexplained divergences (RED — real state-machine drift) ---")
        for d, p in unexplained:
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

    # Per the plan §4.1: only UNEXPLAINED divergences block GREEN.
    # Boundary-drift is reported separately as numerical noise, not
    # averaged into a percentage tolerance. Missing-side records are
    # always RED — they cannot be explained by numerical drift.
    exit_code = 0
    if unexplained or unmatched_real or unmatched_model:
        print("\nRESULT: RED — recorder_model has unexplained disagreements "
              "with the shipping firmware. Do NOT report the model as "
              "validated. Fix the model before proceeding.")
        exit_code = 1
    elif boundary_drift:
        print("\nRESULT: YELLOW — every real↔model disagreement is explained "
              "by measured x86↔ARM numerical drift near a gate threshold. "
              "The model logic itself is consistent with the firmware; the "
              "numerical bound needs to shrink (see §4.2 / row 4b) before "
              "row 4a can be promoted to GREEN.")
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
