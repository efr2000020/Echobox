# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Diagnose *where* the 44% missed passes are lost — pure re-bucket of
data already on disk.

A "pass" is a cluster of BatDetect2 detections within 500 ms gaps
(see ``presence.cluster_detections_into_passes``). A pass is **caught**
if any accepted clip overlaps it; a pass is **missed** otherwise.

This module splits every missed pass into three failure buckets:

  1. ``gate_discarded`` — at least one REJECTED clip overlaps the pass
     window. The base detector triggered, but the cricket-shape gate
     discarded every overlapping clip. **This is the lever we can pull.**
  2. ``detector_never_fired`` — NO clip (accepted or rejected) overlaps
     the pass window. The base detector never triggered on that pass.
     This is the caveat-#3 class of FN — the ``rejected/`` sidecars do
     not capture it, and a gate loosening cannot resurrect it.
  3. ``clip_window_chopping`` — clips exist in the same source WAV but
     none overlap the pass window. Recorder-window effect (preroll +
     silence tail cut off the pass). Note it; don't act on it.

Priority order is (1) > (2) > (3): if *any* rejected clip overlaps the
pass, we call it gate-discarded even if the base detector missed *some*
of the pass — the recoverable lever is what matters for the plan.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import pandas as pd

from .presence import cluster_detections_into_passes, PASS_GAP_MS


# --- data model -------------------------------------------------------------

GATE_DISCARDED       = "gate_discarded"
DETECTOR_NEVER_FIRED = "detector_never_fired"
CLIP_WINDOW_CHOPPING = "clip_window_chopping"

_CATEGORIES = (GATE_DISCARDED, DETECTOR_NEVER_FIRED, CLIP_WINDOW_CHOPPING)


@dataclass
class MissedPassSummary:
    """Per-config counts + fractions of the three buckets.

    Fractions are of *missed* passes (denominator = n_missed, not
    n_passes) — the plan wants "of the losses, where did we lose?".
    """
    n_passes:     int
    n_caught:     int
    n_missed:     int
    n_gate:       int
    n_no_fire:    int
    n_chopping:   int

    def as_row(self, label: str) -> Dict[str, object]:
        def _pct(n: int) -> float:
            return (100.0 * n / self.n_missed) if self.n_missed > 0 else float("nan")
        return {
            "config":     label,
            "n_passes":   self.n_passes,
            "n_caught":   self.n_caught,
            "n_missed":   self.n_missed,
            "n_gate":     self.n_gate,
            "gate_pct":   _pct(self.n_gate),
            "n_no_fire":  self.n_no_fire,
            "no_fire_pct": _pct(self.n_no_fire),
            "n_chopping": self.n_chopping,
            "chopping_pct": _pct(self.n_chopping),
        }


# --- classification ---------------------------------------------------------

def _clips_by_source(replay: pd.DataFrame) -> Dict[str, List[dict]]:
    """Group replay rows by source WAV basename. Each list holds
    ``{start_ms, end_ms, kept}`` dicts for that source, excluding
    no-clip sentinel rows."""
    out: Dict[str, List[dict]] = {}
    if replay.empty:
        return out
    df = replay.copy()
    df["basename"] = df["source_file"].astype(str).map(lambda p: Path(p).name)
    for _, r in df.iterrows():
        if bool(r.get("no_clips", False)):
            continue
        out.setdefault(str(r["basename"]), []).append({
            "start_ms": float(r["clip_start_ms"]),
            "end_ms":   float(r["clip_end_ms"]),
            "kept":     bool(r["kept"]),
        })
    return out


def _overlaps(win: Tuple[float, float], lo: float, hi: float,
              slop_ms: float) -> bool:
    return not (win[1] < lo - slop_ms or win[0] > hi + slop_ms)


CHOPPING_DISTANCE_MS = 1000.0
"""How close the nearest non-overlapping clip must be to a missed pass
for us to call it "chopping" rather than a per-pass detector miss.

Rationale: a 200 ms clip that landed 400 ms before a pass window is a
recorder-length issue (the clip covered the leading silence, not the
pass). A clip that landed 30 seconds before the pass is a *different*
pass in the same file — the base detector simply didn't fire on the
pass in question. Both are "not the sweep gate's fault", but they
point at different fixes, so the report separates them.

1000 ms is generous: any clip within a second of either pass edge is
plausibly a "just barely missed" chopping candidate."""


def classify_missed_pass(pass_window_ms: Tuple[float, float],
                         clips: List[dict],
                         *, slop_ms: float,
                         chopping_ms: float = CHOPPING_DISTANCE_MS
                         ) -> Tuple[str, float]:
    """Return ``(bucket, nearest_clip_distance_ms)`` for one missed pass.

    Precedence: gate_discarded > clip_window_chopping > detector_never_fired.

    Note the ordering change vs the module docstring: only "gate
    discarded" is truly a lever we can pull. A clip within 1 s of the
    pass edge is a recorder-window issue (chopping). Anything further
    or nothing at all is treated as the base detector not firing on
    this specific pass. The distance heuristic separates the "clip
    just missed" story from the "no clip anywhere near this pass"
    story.
    """
    if not clips:
        return DETECTOR_NEVER_FIRED, float("inf")
    p_lo, p_hi = pass_window_ms
    overlaps_rejected = False
    nearest = float("inf")
    for c in clips:
        c_lo, c_hi = c["start_ms"], c["end_ms"]
        if _overlaps(pass_window_ms, c_lo, c_hi, slop_ms):
            if not c["kept"]:
                overlaps_rejected = True
            # Distance = 0 by definition for an overlapping clip.
            nearest = 0.0
            continue
        # Non-overlapping — measure edge-to-edge distance.
        if c_hi < p_lo:
            d = p_lo - c_hi
        else:
            d = c_lo - p_hi
        if d < nearest:
            nearest = d

    if overlaps_rejected:
        return GATE_DISCARDED, 0.0
    if nearest <= chopping_ms:
        return CLIP_WINDOW_CHOPPING, nearest
    return DETECTOR_NEVER_FIRED, nearest


def diagnose_missed_passes(truth: pd.DataFrame, replay: pd.DataFrame,
                           *, gap_ms: float = PASS_GAP_MS,
                           slop_ms: Optional[float] = None
                           ) -> Tuple[MissedPassSummary, pd.DataFrame]:
    """Run the full diagnosis. Returns the pooled summary + a per-pass
    DataFrame with columns:
    ``basename, pass_idx, pass_start_ms, pass_end_ms, caught, bucket``.

    ``slop_ms`` defaults to the per-clip scorer's ``CLIP_EDGE_SLOP_MS``
    (20 ms) so the "caught?" decision here matches the recall figure in
    the parent report row-for-row.
    """
    from .score import CLIP_EDGE_SLOP_MS
    if slop_ms is None:
        slop_ms = CLIP_EDGE_SLOP_MS

    clips_by_src = _clips_by_source(replay)

    rows: List[dict] = []
    n_caught = 0
    n_missed = 0
    n_gate = n_no_fire = n_chopping = 0

    for _, tr in truth.iterrows():
        if not bool(tr["bat_present"]):
            continue
        basename = Path(str(tr["file"])).name
        try:
            dets = json.loads(tr["detections_json"] or "[]")
        except Exception:
            dets = []
        passes = cluster_detections_into_passes(dets, gap_ms=gap_ms)
        clips = clips_by_src.get(basename, [])
        kept_clips = [c for c in clips if c["kept"]]
        for i, (p_lo, p_hi) in enumerate(passes):
            caught = any(_overlaps((p_lo, p_hi), c["start_ms"], c["end_ms"],
                                   slop_ms) for c in kept_clips)
            if caught:
                n_caught += 1
                continue
            n_missed += 1
            bucket, nearest_ms = classify_missed_pass(
                (p_lo, p_hi), clips, slop_ms=slop_ms)
            if bucket == GATE_DISCARDED:
                n_gate += 1
            elif bucket == DETECTOR_NEVER_FIRED:
                n_no_fire += 1
            else:
                n_chopping += 1
            rows.append({
                "basename":       basename,
                "pass_idx":       i,
                "pass_start_ms":  p_lo,
                "pass_end_ms":    p_hi,
                "pass_width_ms":  p_hi - p_lo,
                "n_detections_in_pass": len(
                    [d for d in dets
                     if float(d.get("start_time_s", 0)) * 1000.0 >= p_lo
                     and float(d.get("end_time_s",   0)) * 1000.0 <= p_hi]),
                "bucket":         bucket,
                "nearest_clip_ms": nearest_ms,
                "top_species":    str(tr.get("top_species") or ""),
            })

    summary = MissedPassSummary(
        n_passes=n_caught + n_missed,
        n_caught=n_caught, n_missed=n_missed,
        n_gate=n_gate, n_no_fire=n_no_fire, n_chopping=n_chopping)
    return summary, pd.DataFrame(rows)


# --- rendering --------------------------------------------------------------

def render_summary_md(label_summaries: List[Tuple[str, MissedPassSummary]]
                      ) -> str:
    """Emit a short markdown section for report inclusion.

    Reads as: "of the N missed passes on this corpus, X% were
    gate-discarded (lever), Y% were detector-never-fired (parked), Z%
    were clip-window chopping (note)".
    """
    from .score import HEADLINE_CAVEATS
    lines: List[str] = []
    lines.append("# Missed-pass diagnosis (Step B)")
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(
        "For each missed pass (BatDetect2 pass with no overlapping "
        "accepted clip), we ask: **why was it lost?** Priority order: "
        "``gate_discarded`` (rejected clip overlaps → tunable lever) > "
        "``detector_never_fired`` (no clip at all overlaps → base "
        "detector limit) > ``clip_window_chopping`` (clips exist "
        "nearby but none overlap → recorder-window effect).")
    lines.append("")
    lines.append(
        "| config | passes | caught | missed | gate | no-fire | chopping |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|")
    for label, s in label_summaries:
        def _f(n: int) -> str:
            if s.n_missed <= 0:
                return f"{n} (n/a)"
            return f"{n} ({100.0 * n / s.n_missed:.1f}%)"
        lines.append(
            f"| {label} | {s.n_passes} | {s.n_caught} | {s.n_missed} "
            f"| {_f(s.n_gate)} | {_f(s.n_no_fire)} | {_f(s.n_chopping)} |")
    lines.append("")
    lines.append(
        "**Decision gate:** if ``gate_discarded`` dominates, a "
        "loosening sweep can help. If ``detector_never_fired`` "
        "dominates, gate tuning cannot recover the bulk of the loss "
        "— name the real culprit and stop.")
    lines.append("")
    lines.append(
        "**Chopping distance:** a missed pass is called "
        "``clip_window_chopping`` iff the nearest non-overlapping "
        "clip is within 1 s of the pass edges (see "
        "``pass_diagnosis.CHOPPING_DISTANCE_MS``). Beyond 1 s the "
        "base detector plainly didn't fire on that pass — it fired "
        "elsewhere in the file — so the loss is bucketed as "
        "``detector_never_fired``.")
    return "\n".join(lines)
