#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""§4.2 harness-vs-device feature parity.

The offline detector harness (`tools/validator/native.py`) and the
shipping firmware share source but run on different hardware and with
different compiler flags:

  - offline `.so`: x86, `-march=native -ffast-math` (see top-level
    `CMakeLists.txt`);
  - device: ARM (Pi Zero 2 W).

So they are NOT numerically identical by construction. Near the gate's
hard thresholds — `min_bandwidth_khz = 0.9`, `rep_cv_min = 0.50`,
`rep_cv_max = 1.30` — small numerical drift can flip a per-event
verdict. This tool measures that drift on real bench data so the §4.1
recorder-model check can distinguish "boundary-proximity numerical
noise" from "a real state-machine bug".

Method:

  1. For each Stream A chunk, replay through the offline detector
     (same production `.so` via ctypes), collecting the harness's own
     `EventFeatures` via `Detector.peek_pending_events()`.
  2. Match harness events against the device's own Stream C event log
     by absolute-sample proximity (± ½ hop, tunable).
  3. Compute per-feature deltas
     (`bandwidth_khz / drift_khz / path_ratio / mono_fraction /
     cv_idi`).
  4. Emit max + p50/p95/p99 per feature.
  5. Count **boundary-proximity events** — device events whose feature
     value sits within `max_delta` of a gate threshold (0.9 / 0.50 /
     1.30). Any non-zero count means a per-event verdict could flip
     under the measured drift.

**Pre-registered falsifier**: any single event with a feature delta
larger than `--max-delta`, OR any boundary-proximity flip that is not
independently explained (see `verify_recorder_model.py`, which reads
this tool's JSON output). Non-zero flips do NOT auto-fail this tool —
they annotate row 4b as YELLOW-with-quantified-drift and calibrate the
§4.1 tolerance.

**Promotion rule**: row 4b in `VALIDATION_PROVENANCE.md` moves from
YELLOW to GREEN only when this tool runs on real bench data
(x86 harness output vs actual ARM-device session on the same audio),
every per-feature delta is within tolerance, AND the boundary-proximity
count is zero.

CV is not stored per-event by the device — it is computed at gate time
from a rolling onset ring. This tool computes CV(IDI) on both sides
from the ordered event start-frame sequences using the same Python
helper, so any CV delta measures both numerical drift AND
event-detection drift (they can't be separated without the device
logging its per-event CV). That's acceptable for a bench parity check:
if events line up 1:1 between harness and device, the event-detection
component is zero and only numerical drift remains.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import soundfile as sf

from tools.collection.session_layout import Chunk, EventRecord, Session
from tools.validator import native


# The three hard thresholds in the shipping gate — bandwidth (min) and
# the CV(IDI) band (min, max). Values match the compiled defaults in
# BandEnergyDetector.cpp; if a session's config blob overrides them,
# the CLI's `--thresholds` should be updated to match.
DEFAULT_THRESHOLDS = {
    "bandwidth_khz": [0.9],           # >= min → keep
    "cv_idi":        [0.50, 1.30],    # veto inside band
}


@dataclass
class FeatureDelta:
    """Per-event delta between harness and device for one feature."""
    event_start_sample: int
    device_value:       float
    harness_value:      float

    @property
    def delta(self) -> float:
        return self.harness_value - self.device_value

    @property
    def abs_delta(self) -> float:
        return abs(self.delta)


@dataclass
class ParityReport:
    """Machine-readable output of one parity run.

    Written to <session>/parity_report.json for consumption by
    verify_recorder_model.py's boundary-proximity annotation.
    """
    session_root:          str
    matched_events:        int
    unmatched_device:      int
    unmatched_harness:     int
    # Per-feature: list of FeatureDelta rows (compact form) plus
    # aggregate stats.
    features:              Dict[str, Dict] = field(default_factory=dict)
    boundary_proximity:    Dict[str, Dict] = field(default_factory=dict)

    def to_json(self) -> str:
        return json.dumps({
            "session_root":      self.session_root,
            "matched_events":    self.matched_events,
            "unmatched_device":  self.unmatched_device,
            "unmatched_harness": self.unmatched_harness,
            "features":          self.features,
            "boundary_proximity": self.boundary_proximity,
        }, indent=2)


# -------------------------------------------------------------------- helpers


def compute_cv_series(start_frames: List[int], window: int = 8
                      ) -> Dict[int, float]:
    """Sliding-window CV(IDI) per event.

    For event at index i (0-based), CV is computed over the inter-onset
    intervals between the preceding `window` events (mirrors
    BandEnergyDetector.computeRepStats' onset ring semantics). Returns
    a mapping start_frame → CV, empty for the first `window` events
    where the window isn't populated.
    """
    if not start_frames:
        return {}
    sorted_frames = sorted(start_frames)
    out: Dict[int, float] = {}
    for i in range(1, len(sorted_frames)):
        lo = max(0, i - window + 1)
        window_frames = sorted_frames[lo:i + 1]
        idi = [window_frames[j] - window_frames[j - 1]
               for j in range(1, len(window_frames))]
        if len(idi) < 2:
            continue
        mean_idi = statistics.fmean(idi)
        if mean_idi == 0:
            continue
        stdev = statistics.pstdev(idi)
        out[sorted_frames[i]] = stdev / mean_idi
    return out


def _percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    xs = sorted(values)
    idx = int(round((len(xs) - 1) * p))
    return xs[max(0, min(len(xs) - 1, idx))]


def _summarise_deltas(rows: List[FeatureDelta]) -> Dict:
    if not rows:
        return {"n": 0, "max": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0}
    absolutes = [r.abs_delta for r in rows]
    return {
        "n":   len(rows),
        "max": max(absolutes),
        "p50": _percentile(absolutes, 0.50),
        "p95": _percentile(absolutes, 0.95),
        "p99": _percentile(absolutes, 0.99),
    }


def _boundary_proximity(device_values: List[Tuple[int, float]],
                        thresholds: List[float],
                        max_delta: float) -> Dict:
    """For each threshold, count how many device events fall within
    ``max_delta`` of it — those are the events whose verdict could flip
    under the measured harness↔device drift.
    """
    per_threshold: Dict[str, int] = {}
    within: List[Dict] = []
    for th in thresholds:
        n = 0
        for sample, v in device_values:
            if abs(v - th) < max_delta:
                n += 1
                within.append({"start_sample": sample,
                               "device_value": v, "threshold": th,
                               "gap": v - th})
        per_threshold[f"{th:.2f}"] = n
    return {
        "per_threshold":  per_threshold,
        "total_flippable": len(within),
        "events":         within[:64],   # cap the JSON size
    }


# ---------------------------------------------------------------- harness side


def _harness_events_for_chunk(chunk: Chunk, ref_dir: Path,
                              d_cfg: native.DetectorConfig,
                              hop_size: int) -> List[native.EventFeatures]:
    """Drive the offline detector across one Stream A chunk and drain
    its pending EventFeatures. Uses the production STFT via ctypes.
    """
    wav_path = ref_dir / chunk.file
    samples, sr = sf.read(str(wav_path), dtype="int16", always_2d=False)
    samples = np.asarray(samples, dtype=np.float32) / 32768.0
    mags = native.stft(samples, sr, hop=hop_size, hpf_cutoff_hz=20000.0)

    det = native.Detector(d_cfg)
    for f in range(mags.shape[0]):
        det.process_frame(mags[f], f)
    return det.peek_pending_events()


# ------------------------------------------------------------ correlation


def _match_events(harness: List[native.EventFeatures],
                  harness_chunk_start_sample: int,
                  device: List[EventRecord],
                  hop_size: int, tolerance_samples: int
                  ) -> Tuple[List[Tuple[EventRecord, native.EventFeatures]],
                             List[EventRecord],
                             List[native.EventFeatures]]:
    """Match each harness event to the closest device event within
    ``tolerance_samples`` of the same absolute start_sample. Each side
    can be matched at most once.
    """
    # Absolute start samples for each harness event.
    harness_abs = [
        (harness_chunk_start_sample + h.start_frame * hop_size, h)
        for h in harness
    ]
    # Preserve stable order; O(N*M) is fine for a few thousand events per
    # chunk.
    matched: List[Tuple[EventRecord, native.EventFeatures]] = []
    used_h = set()
    used_d = set()
    for di, d in enumerate(device):
        best_hi, best_gap = -1, tolerance_samples + 1
        for hi, (h_abs, _h) in enumerate(harness_abs):
            if hi in used_h:
                continue
            gap = abs(h_abs - d.start_sample)
            if gap <= tolerance_samples and gap < best_gap:
                best_hi, best_gap = hi, gap
        if best_hi >= 0:
            used_h.add(best_hi); used_d.add(di)
            matched.append((d, harness_abs[best_hi][1]))
    unmatched_d = [d for i, d in enumerate(device) if i not in used_d]
    unmatched_h = [h for i, (_a, h) in enumerate(harness_abs) if i not in used_h]
    return matched, unmatched_d, unmatched_h


# ------------------------------------------------------------------ verify()


def verify(session_root: Path, max_delta: float,
           match_tolerance_samples: int, hop_size: int,
           write_json: bool = True) -> Tuple[int, ParityReport]:
    session = Session.load(session_root)
    if not session.chunks:
        print("FAIL: no Stream-A chunks; cannot run harness on this session.",
              file=sys.stderr)
        return 1, ParityReport(session_root=str(session_root),
                                matched_events=0,
                                unmatched_device=0,
                                unmatched_harness=0)
    if not session.events:
        print("FAIL: no device events in decisions.jsonl.", file=sys.stderr)
        return 1, ParityReport(session_root=str(session_root),
                                matched_events=0,
                                unmatched_device=0,
                                unmatched_harness=0)

    sample_rate = int(session.header.get("sample_rate", 384000))
    d_cfg = native.DetectorConfig(sample_rate=sample_rate)

    # Bucket device events by chunk for the per-chunk match.
    device_by_chunk: Dict[int, List[EventRecord]] = {i: [] for i in range(len(session.chunks))}
    for e in session.events:
        for i, ch in enumerate(session.chunks):
            if ch.start_sample <= e.start_sample < ch.end_sample:
                device_by_chunk[i].append(e); break

    ref_dir = session_root / "reference"
    total_matched: List[Tuple[EventRecord, native.EventFeatures]] = []
    unmatched_device_all: List[EventRecord] = []
    unmatched_harness_all: List[native.EventFeatures] = []
    for i, ch in enumerate(session.chunks):
        harness_events = _harness_events_for_chunk(ch, ref_dir, d_cfg, hop_size)
        matched, unmatched_d, unmatched_h = _match_events(
            harness_events, ch.start_sample, device_by_chunk[i],
            hop_size, match_tolerance_samples)
        total_matched.extend(matched)
        unmatched_device_all.extend(unmatched_d)
        unmatched_harness_all.extend(unmatched_h)

    # Per-feature deltas on the matched set (device value → harness value).
    scalar_features = ("bandwidth_khz", "drift_khz", "path_ratio", "mono_fraction")
    per_feature: Dict[str, List[FeatureDelta]] = {k: [] for k in scalar_features}
    for d, h in total_matched:
        for name in scalar_features:
            per_feature[name].append(FeatureDelta(
                event_start_sample=d.start_sample,
                device_value=float(d.raw.get(name, 0.0)),
                harness_value=float(getattr(h, name)),
            ))

    # CV — computed on both sides from the ordered event start_frame
    # sequences. Since the device didn't log per-event CV, this diff also
    # picks up event-detection drift; documented in the module docstring.
    device_start_frames  = [int(d.raw.get("start_frame", 0)) for d, _h in total_matched]
    harness_start_frames = [int(h.start_frame) for _d, h in total_matched]
    device_cv  = compute_cv_series(device_start_frames)
    harness_cv = compute_cv_series(harness_start_frames)
    cv_rows: List[FeatureDelta] = []
    for d, h in total_matched:
        d_cv = device_cv.get(int(d.raw.get("start_frame", 0)))
        h_cv = harness_cv.get(int(h.start_frame))
        if d_cv is None or h_cv is None:
            continue
        cv_rows.append(FeatureDelta(
            event_start_sample=d.start_sample,
            device_value=d_cv, harness_value=h_cv,
        ))
    per_feature["cv_idi"] = cv_rows

    # Assemble the report.
    report = ParityReport(
        session_root=str(session_root),
        matched_events=len(total_matched),
        unmatched_device=len(unmatched_device_all),
        unmatched_harness=len(unmatched_harness_all),
    )
    for name, rows in per_feature.items():
        report.features[name] = _summarise_deltas(rows)
        # Boundary proximity: only for features that have thresholds.
        if name in DEFAULT_THRESHOLDS:
            device_values = [(r.event_start_sample, r.device_value) for r in rows]
            report.boundary_proximity[name] = _boundary_proximity(
                device_values, DEFAULT_THRESHOLDS[name], max_delta)

    # Human summary + machine JSON (for verify_recorder_model.py to read).
    print(f"session:  {session_root}")
    print(f"matched:  {report.matched_events}  "
          f"unmatched(device)={report.unmatched_device}  "
          f"unmatched(harness)={report.unmatched_harness}")
    print(f"max-delta gate: {max_delta}")
    print("\n--- per-feature deltas (|harness - device|) ---")
    for name in ("bandwidth_khz", "drift_khz", "path_ratio",
                 "mono_fraction", "cv_idi"):
        st = report.features.get(name, {})
        print(f"  {name:16s} n={st.get('n',0)}  "
              f"max={st.get('max',0):.4f}  "
              f"p50={st.get('p50',0):.4f}  "
              f"p95={st.get('p95',0):.4f}  "
              f"p99={st.get('p99',0):.4f}")
    print("\n--- boundary-proximity events (device value within max-delta of a threshold) ---")
    for name, bp in report.boundary_proximity.items():
        for th, n in bp["per_threshold"].items():
            print(f"  {name} @ {th}: {n} events within {max_delta} — verdict-flippable")
        print(f"  {name} total flippable: {bp['total_flippable']}")

    if write_json:
        out_path = session_root / "parity_report.json"
        out_path.write_text(report.to_json())
        print(f"\nwrote {out_path}")

    # Falsifier: any per-feature max delta > tolerance is a fail. The
    # boundary-proximity count is INFORMATIONAL for this tool — it
    # calibrates §4.1's tolerance rather than auto-failing here.
    max_seen = 0.0
    for name in ("bandwidth_khz", "drift_khz", "path_ratio",
                 "mono_fraction", "cv_idi"):
        max_seen = max(max_seen, report.features.get(name, {}).get("max", 0.0))
    if max_seen > max_delta:
        print(f"\nRESULT: FAIL — max feature delta {max_seen:.4f} > {max_delta}. "
              f"§4.2 row 4b stays YELLOW; recorder_model boundary annotation "
              f"cannot rely on this bound.")
        return 1, report
    if any(bp["total_flippable"] > 0
           for bp in report.boundary_proximity.values()):
        print(f"\nRESULT: YELLOW — deltas within tolerance, but some events "
              f"are close enough to a gate threshold that numerical drift "
              f"could flip a verdict. §4.1 will use the {max_delta} bound "
              f"to explain any divergences it sees.")
        return 0, report
    print(f"\nRESULT: GREEN — deltas within tolerance, no boundary-proximity "
          f"events. Row 4b can be promoted to GREEN for this session.")
    return 0, report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("session_root", type=Path)
    ap.add_argument("--max-delta", type=float, default=0.05,
                    help="Per-feature tolerance for GREEN promotion. Also the "
                         "boundary-proximity bound. Default 0.05 (0.05 kHz for "
                         "bandwidth, 0.05 for path_ratio / mono_fraction / CV).")
    ap.add_argument("--match-tolerance-samples", type=int, default=256,
                    help="Max absolute-sample distance for pairing a harness "
                         "event with a device event. Default 256 = ½ hop at "
                         "hop_size=512.")
    ap.add_argument("--hop-size", type=int, default=512,
                    help="STFT hop size (must match the session's setting).")
    ap.add_argument("--no-json", action="store_true",
                    help="Don't write parity_report.json.")
    args = ap.parse_args()
    if not native.is_available():
        print("FAIL: validator native lib not loaded. Build with ./build_dev.sh.",
              file=sys.stderr)
        return 1
    rc, _report = verify(args.session_root, args.max_delta,
                          args.match_tolerance_samples, args.hop_size,
                          write_json=not args.no_json)
    return rc


if __name__ == "__main__":
    sys.exit(main())
