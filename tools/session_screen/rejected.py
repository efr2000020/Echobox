# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Ingest ``rejected/`` sidecars produced by the shipping app's
``--save-rejected`` feature and summarise the device's own near-miss
population.

Motivation. BatDetect2 is a screen that provably misses species here — it
labelled the shipping corpus all-Pipistrellus, found no CF/QCF species,
and is documented to confuse crickets with horseshoe bats. Using it as
"truth" is a coarse tuning proxy at best. The ``rejected/`` capture is
the more trustworthy signal: it is exactly *what the app threw away*, so
counting near-threshold clips (and optionally cross-referencing them
against a BatDetect2 run over the same files) tells us directly how the
cricket-FP gate is trading off recall vs. FP.

This module is pure sidecar IO + pandas — no ML deps. The BatDetect2
cross-reference is optional and comes from an existing truth manifest,
so it slots into the existing ``score`` output cleanly.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import pandas as pd


@dataclass
class RejectedRow:
    """One flattened sidecar. Only the fields the report actually uses;
    the raw JSON is available on disk if a deeper post-mortem needs it."""
    file:            str    # basename of the .wav (matches truth_manifest.file)
    dir_relative:    str    # sidecar path relative to the rejected/ root
    capture_ts:      str    # ISO-8601 from recording.capture_ts
    rejected_reason: str    # "sweep" / "temporal" / "unknown" / "" (older sidecar)
    rejected_mode:   str    # "all" / "sample" / "boundary" / ""
    n_events:        int
    n_gate_rejected: int
    # Aggregate features across all gate-rejected events in the clip; picked
    # to be the ones the operator can compare directly against the tunable
    # thresholds without a second lookup.
    min_bandwidth_khz: float   # min across rejected events (closest to gate)
    max_drift_khz:     float
    # Tunable snapshot at the time this clip was written — pulled from the
    # sidecar's detector.tunables block so a mixed-tunable corpus is
    # visible in the report (all zeros = older/tunable-less plugin).
    tunable_min_bandwidth_khz: float
    error:           str = ""


def _to_float(v) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def _load_one_sidecar(path: Path, root: Path) -> Optional[RejectedRow]:
    """Parse one sidecar JSON into a row. Malformed sidecars are skipped
    with ``error=…`` populated so the report can flag how many were
    unreadable without aborting the whole ingest."""
    try:
        with open(path) as f:
            doc = json.load(f)
    except Exception as e:
        return RejectedRow(
            file="", dir_relative=str(path.relative_to(root)),
            capture_ts="", rejected_reason="", rejected_mode="",
            n_events=0, n_gate_rejected=0,
            min_bandwidth_khz=0.0, max_drift_khz=0.0,
            tunable_min_bandwidth_khz=0.0,
            error=f"{type(e).__name__}: {e}",
        )

    recording = doc.get("recording") or {}
    rejected  = recording.get("rejected") or {}
    detector  = doc.get("detector") or {}
    tunables  = detector.get("tunables") or {}
    events    = doc.get("events") or []

    gate_rejected_events = [
        e for e in events if bool(e.get("gate_rejected", False))
    ]
    min_bw = 0.0
    max_dr = 0.0
    if gate_rejected_events:
        bws = [_to_float(e.get("bandwidth_khz")) for e in gate_rejected_events]
        drs = [_to_float(e.get("drift_khz"))     for e in gate_rejected_events]
        min_bw = min(bws) if bws else 0.0
        max_dr = max(drs) if drs else 0.0

    return RejectedRow(
        file=str(recording.get("wav_path", "")),
        dir_relative=str(path.relative_to(root)),
        capture_ts=str(recording.get("capture_ts", "")),
        rejected_reason=str(rejected.get("reason", "")),
        rejected_mode=str(rejected.get("mode", "")),
        n_events=len(events),
        n_gate_rejected=len(gate_rejected_events),
        min_bandwidth_khz=min_bw,
        max_drift_khz=max_dr,
        tunable_min_bandwidth_khz=_to_float(tunables.get("min_bandwidth_khz")),
    )


def load_rejected_dir(root: Path) -> pd.DataFrame:
    """Return a DataFrame of one row per sidecar under ``root``.

    Empty (or missing) directory returns an empty DataFrame with the
    schema populated, so downstream ``score`` code can branch on ``.empty``
    without a special-case null check.
    """
    cols = [f.name for f in RejectedRow.__dataclass_fields__.values()]
    if not root.exists() or not root.is_dir():
        return pd.DataFrame(columns=cols)
    rows: List[RejectedRow] = []
    for p in sorted(root.rglob("*.json")):
        # Ignore hidden files (e.g. the atomic writer's .json.tmp is renamed
        # into place on success — anything left over from a crash isn't a
        # sidecar we should score).
        if p.name.startswith("."):
            continue
        r = _load_one_sidecar(p, root)
        if r is not None:
            rows.append(r)
    if not rows:
        return pd.DataFrame(columns=cols)
    return pd.DataFrame([r.__dict__ for r in rows])


@dataclass
class RejectedSummary:
    """Report-level counts for the rejected/ section."""
    n_sidecars:        int
    n_parse_errors:    int
    by_reason:         Dict[str, int]        # {"sweep": n, "temporal": n, ...}
    n_near_min_bw:     int                   # rejected clips within margin of threshold
    near_bw_margin_khz: float                # margin used for near_min_bw count
    # Optional BatDetect2 cross-reference (populated when a truth manifest
    # for the rejected/ dir is provided). ``n_truth_matched`` is the number
    # of rejected clips whose basename matched a truth row; ``n_bat_hit``
    # is the subset BatDetect2 flagged as containing a bat.
    n_truth_matched:   int = 0
    n_bat_hit:         int = 0
    by_reason_bat_hit: Dict[str, int] = None       # per-reason bat-hit counts


NEAR_BW_MARGIN_KHZ = 0.30
"""Same margin the shipping app's Boundary mode uses to classify a
near-miss. Keeping the report's default aligned with the field-mode
default means a boundary-mode capture and this section describe the
same population."""


def summarise(df: pd.DataFrame,
              *, truth_join: Optional[pd.DataFrame] = None,
              near_bw_margin_khz: float = NEAR_BW_MARGIN_KHZ,
              ) -> RejectedSummary:
    """Compute the summary counts.

    ``truth_join`` — if provided — must have the columns ``file`` and
    ``bat_present``. Rows are joined by the sidecar's ``file`` (WAV
    basename) against the truth manifest's ``file`` column. We compare
    basenames rather than full paths because BatDetect2 was almost
    certainly run over the rejected/ dir separately, so the ``file``
    column paths will differ.
    """
    if df.empty:
        return RejectedSummary(
            n_sidecars=0, n_parse_errors=0, by_reason={},
            n_near_min_bw=0,
            near_bw_margin_khz=near_bw_margin_khz,
            by_reason_bat_hit={})

    errors_mask = df.get("error", pd.Series([""] * len(df))).fillna("") != ""
    n_errors    = int(errors_mask.sum())
    good        = df[~errors_mask]

    by_reason: Dict[str, int] = {}
    if not good.empty:
        by_reason = {
            str(k): int(v)
            for k, v in good["rejected_reason"].value_counts().items()
        }

    # Near-miss count: min_bandwidth_khz across rejected events sits within
    # ± margin of the sidecar's own tunable_min_bandwidth_khz. Uses the
    # sidecar's tunable rather than a globally hard-coded 0.9 so a tuning
    # sweep can be scored against the tunable value that produced it.
    n_near = 0
    if not good.empty:
        thr = good["tunable_min_bandwidth_khz"]
        bw  = good["min_bandwidth_khz"]
        # If the tunable is 0 (older sidecar, or plugin without the knob),
        # fall back to 0.9 kHz — the shipped default.
        thr_eff = thr.where(thr > 0.0, 0.9)
        near = (bw - thr_eff).abs() <= near_bw_margin_khz
        n_near = int(near.sum())

    n_matched = 0
    n_bat_hit = 0
    by_reason_bat: Dict[str, int] = {}
    if truth_join is not None and not truth_join.empty and not good.empty:
        # Normalise both sides on WAV basename so a mismatched dir prefix
        # doesn't silently drop the join.
        tj = truth_join.copy()
        tj["_basename"] = tj["file"].astype(str).map(lambda p: Path(p).name)
        gj = good.copy()
        gj["_basename"] = gj["file"].astype(str).map(lambda p: Path(p).name)
        merged = gj.merge(tj[["_basename", "bat_present"]],
                          on="_basename", how="left")
        matched = merged["bat_present"].notna()
        n_matched = int(matched.sum())
        bat_hit_mask = matched & merged["bat_present"].fillna(False).astype(bool)
        n_bat_hit = int(bat_hit_mask.sum())
        if n_bat_hit > 0:
            by_reason_bat = {
                str(k): int(v)
                for k, v in merged.loc[bat_hit_mask, "rejected_reason"]
                                   .value_counts().items()
            }

    return RejectedSummary(
        n_sidecars=len(df),
        n_parse_errors=n_errors,
        by_reason=by_reason,
        n_near_min_bw=n_near,
        near_bw_margin_khz=near_bw_margin_khz,
        n_truth_matched=n_matched,
        n_bat_hit=n_bat_hit,
        by_reason_bat_hit=by_reason_bat,
    )


def render_section(summary: RejectedSummary) -> str:
    """Markdown fragment for insertion into ``report.md``.

    Returns an empty string when there are no sidecars, so the caller can
    unconditionally concatenate this into the report body without a
    special-case check.
    """
    if summary.n_sidecars == 0:
        return ""

    lines: List[str] = []
    lines.append("## Rejected-set — device's own near-miss population")
    lines.append("")
    lines.append(
        "This section reads sidecars written by the shipping app under "
        "`<output>/rejected/` when the operator passed `--save-rejected`. "
        "**These are the more trustworthy signal** — BatDetect2 misses "
        "species and confuses crickets with CF bats; the rejected/ capture "
        "is exactly what the app threw away, so the near-threshold count "
        "is the direct tuning target.")
    lines.append("")
    lines.append(
        f"- rejected clips ingested: **{summary.n_sidecars}** "
        f"(parse errors: {summary.n_parse_errors})")

    if summary.by_reason:
        parts = ", ".join(f"{k}={v}"
                          for k, v in sorted(summary.by_reason.items()))
        lines.append(f"- by reason: {parts}")

    lines.append(
        f"- near-threshold (bandwidth within ±"
        f"{summary.near_bw_margin_khz:.2f} kHz of the sidecar's "
        f"min_bandwidth_khz): **{summary.n_near_min_bw}**")

    if summary.n_truth_matched > 0:
        pct = (100.0 * summary.n_bat_hit / summary.n_truth_matched
               if summary.n_truth_matched > 0 else 0.0)
        lines.append(
            f"- BatDetect2 cross-reference: matched **{summary.n_truth_matched}** "
            f"rejected clips against a truth manifest; **{summary.n_bat_hit}** "
            f"({pct:.1f}%) were flagged as containing a bat — these are "
            "real bats the gate discarded.")
        if summary.by_reason_bat_hit:
            parts = ", ".join(f"{k}={v}"
                              for k, v in sorted(summary.by_reason_bat_hit.items()))
            lines.append(f"  - bat-hit by reason: {parts}")

    lines.append("")
    return "\n".join(lines)
