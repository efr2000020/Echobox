# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""File-level and pass-level presence recall — re-buckets an existing
per-clip join without re-running replay or BatDetect2.

Why this exists:
    The per-clip recall (score.py) measures "of clips near a BatDetect2
    bat detection, what fraction did the recorder keep?". That is an
    *aggressiveness* metric on bat-adjacent audio, not a presence-recall
    metric: a busy bat pass generates many BatDetect2 detections and a
    small number of recorder clips, so one discarded clip counts as
    many FN in the per-clip bucket even when other clips in the same
    pass were kept.

    Presence recall is the number the product actually cares about:
    when BatDetect2 says a file contains a bat, did the recorder save
    ≥1 clip for it? This module answers that from artefacts already on
    disk.

Reads:
    - ``truth_manifest.parquet`` — per-file BatDetect2 rows with
      ``bat_present`` and ``detections_json``.
    - ``replay_manifest.parquet`` — per-clip rows with ``source_file``,
      ``kept``, and ``no_clips`` sentinel rows.

No re-run, no additional dependencies beyond pandas.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import pandas as pd


# --- file-level presence ----------------------------------------------------

@dataclass
class PresenceSummary:
    """Per-file confusion + rates for one config.

    ``present_tp`` = bat-present source with ≥1 accepted clip anywhere.
    ``present_fn`` = bat-present source with 0 accepted clips.
    ``present_tn`` = no-bat source with 0 accepted clips (correctly silent).
    ``present_fp`` = no-bat source with ≥1 accepted clip (file-level cricket leak).

    Denominators are the truth-side source counts, so recall_per_file =
    present_tp / (present_tp + present_fn) = present_tp / 433 on the
    field session.
    """
    n_files:      int
    present_tp:   int
    present_fn:   int
    present_fp:   int
    present_tn:   int

    @property
    def recall_per_file(self) -> Optional[float]:
        den = self.present_tp + self.present_fn
        return self.present_tp / den if den > 0 else None

    @property
    def cricket_rejection_per_file(self) -> Optional[float]:
        den = self.present_fp + self.present_tn
        return self.present_tn / den if den > 0 else None


def _kept_counts_by_source(replay: pd.DataFrame) -> pd.DataFrame:
    """Return a DataFrame with one row per source WAV:
    (basename, n_kept, n_discarded, saw_any_clip)."""
    if replay.empty:
        return pd.DataFrame(columns=["basename", "n_kept",
                                     "n_discarded", "saw_any_clip"])
    df = replay.copy()
    df["basename"] = df["source_file"].astype(str).map(lambda p: Path(p).name)
    df["is_kept"]   = df["kept"].fillna(False).astype(bool)
    df["is_clip"]   = ~df["no_clips"].fillna(False).astype(bool)
    grouped = df.groupby("basename").agg(
        n_kept=("is_kept", "sum"),
        n_discarded=("is_clip",
                     lambda s: int(((~df.loc[s.index, "is_kept"]) &
                                    (df.loc[s.index, "is_clip"])).sum())),
        saw_any_clip=("is_clip", "any"),
    ).reset_index()
    return grouped


def _truth_by_basename(truth: pd.DataFrame) -> pd.DataFrame:
    """Return truth rows keyed by WAV basename (matches the join key)."""
    df = truth.copy()
    df["basename"] = df["file"].astype(str).map(lambda p: Path(p).name)
    return df[["basename", "bat_present", "top_species",
               "n_detections", "detections_json"]]


def per_file_table(truth: pd.DataFrame,
                   replay: pd.DataFrame) -> pd.DataFrame:
    """One row per source WAV: bat_present + kept-clip counts + presence bucket."""
    kept = _kept_counts_by_source(replay)
    tr   = _truth_by_basename(truth)
    merged = tr.merge(kept, on="basename", how="left")
    merged["n_kept"] = merged["n_kept"].fillna(0).astype(int)
    merged["n_discarded"] = merged["n_discarded"].fillna(0).astype(int)
    merged["saw_any_clip"] = merged["saw_any_clip"].fillna(False)

    def _bucket(r) -> str:
        bat = bool(r["bat_present"])
        kept_any = int(r["n_kept"]) > 0
        if bat and kept_any:
            return "present_tp"
        if bat and not kept_any:
            return "present_fn"
        if not bat and kept_any:
            return "present_fp"
        return "present_tn"

    merged["bucket"] = merged.apply(_bucket, axis=1)
    return merged


def score_per_file(truth: pd.DataFrame,
                   replay: pd.DataFrame) -> PresenceSummary:
    """Pooled per-file confusion counts."""
    df = per_file_table(truth, replay)
    counts = df["bucket"].value_counts().to_dict()
    return PresenceSummary(
        n_files=len(df),
        present_tp=int(counts.get("present_tp", 0)),
        present_fn=int(counts.get("present_fn", 0)),
        present_fp=int(counts.get("present_fp", 0)),
        present_tn=int(counts.get("present_tn", 0)),
    )


def presence_fn_files(truth: pd.DataFrame,
                      replay: pd.DataFrame) -> pd.DataFrame:
    """Bat-present source WAVs with zero accepted clips.

    This is the human-ear queue: every row here is a file BatDetect2
    said contains a bat, and the recorder saved nothing. Sorted by
    number of BatDetect2 detections descending — files with many
    detections are the highest-signal misses.
    """
    df = per_file_table(truth, replay)
    fn = df[df["bucket"] == "present_fn"].copy()
    fn = fn[["basename", "top_species", "n_detections",
             "n_kept", "n_discarded"]]
    return fn.sort_values("n_detections", ascending=False).reset_index(drop=True)


# --- pass-level presence ----------------------------------------------------

PASS_GAP_MS = 500.0
"""Detections within a pass are gap ≤ 500 ms apart. Above that we
declare a new pass. Chosen because a Pipistrellus pass typically has
inter-call gaps of tens to a couple hundred ms; a 500 ms gap is
conservative — merges near-connected passes rather than splitting
one pass into many."""


def cluster_detections_into_passes(dets: List[dict],
                                   gap_ms: float = PASS_GAP_MS
                                   ) -> List[Tuple[float, float]]:
    """Return a list of ``(pass_start_ms, pass_end_ms)`` windows.

    Empty ``dets`` yields ``[]``. Single-detection files yield one
    single-detection pass. The window is span (first start → last end),
    NOT per-detection sum.
    """
    if not dets:
        return []
    sorted_d = sorted(dets, key=lambda d: float(d.get("start_time_s", 0.0)))
    passes: List[Tuple[float, float]] = []
    cur_start = float(sorted_d[0].get("start_time_s", 0.0)) * 1000.0
    cur_end   = float(sorted_d[0].get("end_time_s",   0.0)) * 1000.0
    for d in sorted_d[1:]:
        s = float(d.get("start_time_s", 0.0)) * 1000.0
        e = float(d.get("end_time_s",   0.0)) * 1000.0
        if s - cur_end > gap_ms:
            passes.append((cur_start, cur_end))
            cur_start, cur_end = s, e
        else:
            cur_end = max(cur_end, e)
    passes.append((cur_start, cur_end))
    return passes


@dataclass
class PassSummary:
    """Pass-level presence recall: fraction of BatDetect2-clustered
    passes whose window overlaps at least one accepted clip."""
    n_passes:     int
    caught:       int
    missed:       int

    @property
    def recall_per_pass(self) -> Optional[float]:
        return self.caught / self.n_passes if self.n_passes > 0 else None


def score_per_pass(truth: pd.DataFrame, replay: pd.DataFrame,
                   *, gap_ms: float = PASS_GAP_MS) -> PassSummary:
    """Cluster each truth file's detections into passes, then for each
    pass check whether any accepted clip in the same source WAV overlaps
    the pass window (± the same edge slop as the per-clip scorer).
    """
    from .score import CLIP_EDGE_SLOP_MS
    kept_by_source: Dict[str, List[Tuple[float, float]]] = {}
    if not replay.empty:
        rp = replay.copy()
        rp["basename"] = rp["source_file"].astype(str).map(lambda p: Path(p).name)
        for _, r in rp.iterrows():
            if bool(r.get("no_clips", False)):
                continue
            if not bool(r["kept"]):
                continue
            kept_by_source.setdefault(str(r["basename"]), []).append(
                (float(r["clip_start_ms"]), float(r["clip_end_ms"])))

    caught = 0
    total  = 0
    for _, r in truth.iterrows():
        if not bool(r["bat_present"]):
            continue
        basename = Path(str(r["file"])).name
        try:
            dets = json.loads(r["detections_json"] or "[]")
        except Exception:
            dets = []
        passes = cluster_detections_into_passes(dets, gap_ms=gap_ms)
        clips = kept_by_source.get(basename, [])
        for p_lo, p_hi in passes:
            total += 1
            lo = p_lo - CLIP_EDGE_SLOP_MS
            hi = p_hi + CLIP_EDGE_SLOP_MS
            if any(not (c_hi < lo or c_lo > hi) for c_lo, c_hi in clips):
                caught += 1

    return PassSummary(n_passes=total, caught=caught, missed=total - caught)


# --- rendering --------------------------------------------------------------

def _fmt_pct(x: Optional[float]) -> str:
    if x is None:
        return "n/a"
    return f"{100.0 * x:.1f}%"


def render_summary_md(label_summaries: List[Tuple[str, PresenceSummary,
                                                  PassSummary,
                                                  "object"]]) -> str:
    """Emit the recall_summary.md body.

    ``label_summaries`` is a list of ``(config_label, PresenceSummary,
    PassSummary, ClipScoreSummary)`` tuples — one per config. The clip
    summary is the object returned by ``score.score()`` so we can print
    the per-clip and per-file numbers side by side.
    """
    from .score import HEADLINE_CAVEATS
    lines: List[str] = []
    lines.append("# Presence recall (per-file + per-pass) — follow-up")
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(
        "**Additional caveat for this report:** per-file recall is still "
        "measured against BatDetect2, so it is *presence recall on the "
        "species BatDetect2 can see* — Pipistrellus. Not a general "
        "recall claim, not a CF claim.")
    lines.append("")
    lines.append("## The bracket")
    lines.append("")
    lines.append(
        "Product goal maps to **per-file / per-pass presence recall** "
        "(top row). The per-clip number (bottom row) is an "
        "*aggressiveness* metric on bat-adjacent audio and is expected "
        "to be lower — a busy bat pass produces many BatDetect2 "
        "detections and few recorder clips, so a single discarded clip "
        "counts as many FN. True presence recall sits at or above the "
        "per-file line.")
    lines.append("")
    lines.append("| config | per-file recall | per-pass recall | per-clip recall |")
    lines.append("|---|---|---|---|")
    for label, pres, pas, clip in label_summaries:
        pf_num = pres.present_tp
        pf_den = pres.present_tp + pres.present_fn
        pp_num = pas.caught
        pp_den = pas.n_passes
        c = clip.confusion
        pc_num = c.n_tp
        pc_den = c.n_tp + c.n_fn
        lines.append(
            f"| {label} "
            f"| {_fmt_pct(pres.recall_per_file)} ({pf_num}/{pf_den}) "
            f"| {_fmt_pct(pas.recall_per_pass)} ({pp_num}/{pp_den}) "
            f"| {_fmt_pct(clip.recall)} ({pc_num}/{pc_den}) |")
    lines.append("")
    lines.append("## Presence-side confusion")
    lines.append("")
    lines.append(
        "For completeness — the per-file breakdown. "
        "``present_fn`` (bat-present, zero clips saved) is the human-ear "
        "queue in ``presence_fn_files.csv``. ``present_fp`` (no-bat file, "
        "≥1 clip saved) is the file-level view of the cricket leak.")
    lines.append("")
    lines.append("| config | n_files | present_tp | present_fn | present_fp | present_tn |")
    lines.append("|---|---|---|---|---|---|")
    for label, pres, _, _ in label_summaries:
        lines.append(
            f"| {label} | {pres.n_files} "
            f"| {pres.present_tp} | {pres.present_fn} "
            f"| {pres.present_fp} | {pres.present_tn} |")
    lines.append("")
    return "\n".join(lines)


def write_deliverables(label: str, output_dir: Path,
                       truth: pd.DataFrame,
                       replay: pd.DataFrame) -> Tuple[PresenceSummary,
                                                     PassSummary]:
    """Write per-config presence artefacts.

    Emits ``recall_per_file.csv`` (every source row + its bucket) and
    ``presence_fn_files.csv`` (just the bat-present, zero-clip rows,
    sorted by detection count) under ``output_dir``.

    Returns the summaries so a top-level driver can build the bracket
    table without re-doing the join.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    per_file = per_file_table(truth, replay)
    per_file.to_csv(output_dir / "recall_per_file.csv", index=False)

    presence_fn_files(truth, replay).to_csv(
        output_dir / "presence_fn_files.csv", index=False)

    pres_summary = score_per_file(truth, replay)
    pass_summary = score_per_pass(truth, replay)
    return pres_summary, pass_summary
