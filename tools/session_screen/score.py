# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Per-clip time-overlap join between BatDetect2 truth (per file, per
detection) and echobox-replay clips (per accepted/rejected clip, in
source-WAV time).

The scoring model, verbatim from the plan:

  - app kept     + BatDetect2 bat    → TP
  - app discarded + BatDetect2 bat   → FN  (from rejected/ sidecars)
  - app kept     + BatDetect2 no-bat → FP  (cricket leak)
  - app discarded + BatDetect2 no-bat → TN

A clip is "on a bat" iff any of the source WAV's BatDetect2 detections
overlaps the clip's ``[start_ms, end_ms]`` window in source time (with a
small slop, since clip edges are quantised at the DSP hop). The species
label is the top-confidence overlapping detection.

Per-species rates carry raw counts alongside — never bare percentages,
per the plan's honesty ceiling.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import json

import pandas as pd

from .manifest import read_replay_manifest, read_truth_manifest


def assert_basenames_unique(df: pd.DataFrame, *, column: str, source: str) -> None:
    """Fail loud if the join key (WAV basename) collides.

    The mid-analysis fix in score.py switched the truth ↔ replay join
    from full-path to basename. That is only correct if basenames are
    unique across the corpus; a collision would silently mis-attribute
    every clip in the colliding pair and move every downstream number.

    Field convention embeds a globally unique cumulative-sample offset
    in every filename, so uniqueness is expected — this assertion is
    cheap insurance against a future corpus that violates the
    convention (e.g. a merged multi-device dataset).
    """
    if column not in df.columns:
        return
    # Replay manifests have one row per CLIP, so ``source_file`` repeats.
    # What we care about is: do two *different* source paths share a
    # basename? Dedup on the full path before checking uniqueness of
    # basenames; then a real collision (two distinct paths, same
    # basename) is the only condition that trips the assertion.
    unique_paths = df[column].astype(str).drop_duplicates()
    if unique_paths.empty:
        return
    names = unique_paths.map(lambda p: Path(p).name)
    counts = names.value_counts()
    dupes = counts[counts > 1]
    if not dupes.empty:
        top = ", ".join(f"{n}×{k}" for k, n in dupes.head(3).items())
        raise ValueError(
            f"basename collision in {source} ({column}): "
            f"{len(dupes)} duplicate basename(s); first: {top}. "
            "The join keys on basename; a collision mis-attributes "
            "every clip in the colliding pair. Rename or split the "
            "corpus so basenames are unique.")


HEADLINE_CAVEATS = (
    "This assessment is ROUGH and has hard limits. Do not present it as ground truth:\n"
    "\n"
    "  1. **BatDetect2 is a screen, not truth.** On this corpus it labelled everything\n"
    "     Pipistrellus and found **no CF/QCF species** (Rhinolophus/Nyctalus/Myotis).\n"
    "     So CF recall is **NOT ASSESSED** — untested, not \"good\".\n"
    "  2. **x86 replay != ARM device.** Build uses `-ffast-math`/`-march=native`; the\n"
    "     device is ARM. Per-event/near-threshold numbers are a **tuning proxy**, not\n"
    "     device-exact.\n"
    "  3. **Two false-negative classes.** `rejected/` captures \"the gate discarded it\".\n"
    "     It does NOT capture \"the base detector never triggered on it\" (too faint/far/\n"
    "     out of band) — that needs continuous-reference labelling we don't have.\n"
    "  4. **No human adjudication yet.** When app and BatDetect2 disagree, we **count and\n"
    "     list** the rows; we do not score them as right/wrong. Absolute rates are soft;\n"
    "     **run-to-run deltas are the trustworthy signal.**"
)


NO_BAT_LABEL = "__no_bat__"
"""Group label used in the per-species table for clips whose source WAV
had no BatDetect2 detection overlapping the clip window. Matches the
old score.py's pool label so downstream tooling doesn't need to change."""


CF_SPECIES_TOKENS = ("rhinolophus", "horseshoe")
"""Any BatDetect2 species string containing one of these (case-insensitive)
is treated as a CF/Rhinolophus hit and forced into disagreements.csv for
human review — cricket↔CF confusion is exactly the weak spot we're
trying to tune around."""


CLIP_EDGE_SLOP_MS = 20.0
"""Widen the clip window by this much on each side when checking overlap
against a BatDetect2 detection. Clip edges are quantised at the DSP hop
(~1.3 ms at 384 kHz / hop 512) AND the recorder's preroll/silence adds
a few tens of ms of headroom that legitimately contain the detection's
early/late chirps. 20 ms is a small enough slop that a genuinely
non-overlapping cricket clip is not accidentally labelled as bat."""


# --- confusion counters ------------------------------------------------------

@dataclass
class ConfusionCounts:
    n_tp: int = 0
    n_fn: int = 0
    n_fp: int = 0
    n_tn: int = 0

    def add(self, kept: bool, bat: bool) -> None:
        if kept and bat:
            self.n_tp += 1
        elif not kept and bat:
            self.n_fn += 1
        elif kept and not bat:
            self.n_fp += 1
        else:
            self.n_tn += 1

    @property
    def recall(self) -> Optional[float]:
        den = self.n_tp + self.n_fn
        return self.n_tp / den if den > 0 else None

    @property
    def fp_rate(self) -> Optional[float]:
        den = self.n_fp + self.n_tn
        return self.n_fp / den if den > 0 else None

    @property
    def cricket_rejection(self) -> Optional[float]:
        den = self.n_fp + self.n_tn
        return self.n_tn / den if den > 0 else None


@dataclass
class PerCallCounts:
    """Detection-side recall counters.

    - ``n_calls_total``: every BatDetect2 detection in the corpus.
    - ``n_calls_captured``: detections overlapped by at least one KEPT
      clip window (± ``CLIP_EDGE_SLOP_MS``).

    Denominator is fixed by BatDetect2 alone, so it does not move when
    the recorder config changes what clips get emitted — this is the
    defect per-file and per-clip recall have and the reason per-call
    recall is the new primary KPI.
    """
    n_calls_total:    int = 0
    n_calls_captured: int = 0

    @property
    def recall(self) -> Optional[float]:
        if self.n_calls_total == 0:
            return None
        return self.n_calls_captured / self.n_calls_total


@dataclass
class ScoreSummary:
    """Aggregate pooled confusion + rates for one config's run."""
    n_clips:      int
    n_no_clip_sources: int
    n_error_rows: int
    confusion:    ConfusionCounts
    per_call:     PerCallCounts = field(default_factory=PerCallCounts)
    cricket_rejection_rate: Optional[float] = None
    recall:                 Optional[float] = None
    fp_rate:                Optional[float] = None
    per_call_recall:        Optional[float] = None

    def as_row(self) -> Dict[str, object]:
        return {
            "n_clips":  self.n_clips,
            "n_no_clip_sources": self.n_no_clip_sources,
            "n_errors": self.n_error_rows,
            "n_tp": self.confusion.n_tp, "n_fn": self.confusion.n_fn,
            "n_fp": self.confusion.n_fp, "n_tn": self.confusion.n_tn,
            "recall":                 self.recall,
            "fp_rate":                self.fp_rate,
            "cricket_rejection_rate": self.cricket_rejection_rate,
            "n_calls_total":          self.per_call.n_calls_total,
            "n_calls_captured":       self.per_call.n_calls_captured,
            "per_call_recall":        self.per_call_recall,
        }


# --- per-clip overlap join ---------------------------------------------------

@dataclass
class ClipAssignment:
    """One replay row labelled with its overlapping BatDetect2 detection.

    ``group`` is the BatDetect2 species (or NO_BAT_LABEL); ``bat_present``
    is convenience derived from group. Kept alongside so downstream code
    doesn't need to reparse the string.
    """
    source_file:      str
    clip_wav:         str
    kept:             bool
    rejected_reason:  str
    clip_start_ms:    float
    clip_end_ms:      float
    group:            str          # species or NO_BAT_LABEL
    bat_present:      bool
    top_species:      str          # BatDetect2 species of overlapping det (or "")
    top_confidence:   float
    min_bandwidth_khz: float
    max_drift_khz:    float
    tunable_min_bw_khz: float
    error:            str = ""


def _detections_by_file(truth: pd.DataFrame) -> Dict[str, List[dict]]:
    """Map source WAV basename → list of ``{start_time_s,end_time_s,species,confidence}``.

    Keyed by **basename** (``Path.name``) rather than the full path so
    a truth manifest that was built with a relative ``--input`` still
    joins against a replay manifest built with an absolute one, and
    vice versa. Two files with the same basename in different subdirs
    of the same corpus would collide, but our field convention embeds
    a cumulative-sample offset in the filename that already makes
    basenames globally unique.

    ``TruthRow.detections_json`` stores the list as a compact JSON string;
    parse once here so the per-clip loop below is cheap.
    """
    out: Dict[str, List[dict]] = {}
    for _, row in truth.iterrows():
        raw = row.get("detections_json") or "[]"
        key = Path(str(row["file"])).name
        try:
            out[key] = json.loads(raw)
        except Exception:
            out[key] = []
    return out


def _pick_overlap(clip_start_ms: float, clip_end_ms: float,
                  detections: List[dict]) -> Optional[dict]:
    """Return the highest-confidence detection whose window overlaps
    ``[clip_start_ms - slop, clip_end_ms + slop]``; ``None`` if none."""
    lo = clip_start_ms - CLIP_EDGE_SLOP_MS
    hi = clip_end_ms   + CLIP_EDGE_SLOP_MS
    best = None
    best_conf = -1.0
    for d in detections:
        d_lo = float(d.get("start_time_s", 0.0)) * 1000.0
        d_hi = float(d.get("end_time_s",   0.0)) * 1000.0
        if d_hi < lo or d_lo > hi:
            continue
        conf = float(d.get("confidence", 0.0))
        if conf > best_conf:
            best_conf = conf
            best = d
    return best


def assign_clips(truth: pd.DataFrame,
                 replay: pd.DataFrame) -> List[ClipAssignment]:
    """Per-clip overlap join.

    A replay row with ``no_clips=True`` or with an ``error`` set is
    skipped — those sources contribute nothing to the confusion (see
    ScoreSummary.n_no_clip_sources / n_error_rows for their counts).
    """
    det_map = _detections_by_file(truth)
    # Build a source-basename → top_species fallback map for clips whose
    # source WAV BatDetect2 flagged but with no detection that overlaps
    # the clip window. Same basename-key rationale as in
    # ``_detections_by_file``.
    file_species: Dict[str, str] = {
        Path(str(r["file"])).name: str(r.get("top_species") or "")
        for _, r in truth.iterrows()
    }

    assignments: List[ClipAssignment] = []
    for _, r in replay.iterrows():
        if bool(r.get("no_clips", False)) or (r.get("error") or ""):
            continue
        src = str(r["source_file"])
        src_key = Path(src).name
        clip_start = float(r["clip_start_ms"])
        clip_end   = float(r["clip_end_ms"])
        detections = det_map.get(src_key, [])
        hit = _pick_overlap(clip_start, clip_end, detections)
        if hit is not None:
            species = str(hit.get("species", ""))
            group = species or NO_BAT_LABEL
            bat_present = True
            conf = float(hit.get("confidence", 0.0))
        else:
            species = ""
            group = NO_BAT_LABEL
            bat_present = False
            conf = 0.0
        assignments.append(ClipAssignment(
            source_file=src,
            clip_wav=str(r.get("clip_wav") or ""),
            kept=bool(r["kept"]),
            rejected_reason=str(r.get("rejected_reason") or ""),
            clip_start_ms=clip_start, clip_end_ms=clip_end,
            group=group, bat_present=bat_present,
            top_species=species, top_confidence=conf,
            min_bandwidth_khz=float(r.get("min_bandwidth_khz") or 0.0),
            max_drift_khz=float(r.get("max_drift_khz") or 0.0),
            tunable_min_bw_khz=float(r.get("tunable_min_bw_khz") or 0.0),
        ))
    return assignments


# --- per-call recall (inverted join: detection-side) -------------------------

def _kept_clips_by_basename(
        replay: pd.DataFrame) -> Dict[str, List[Tuple[float, float]]]:
    """Map source WAV basename → list of ``(clip_start_ms, clip_end_ms)``
    for KEPT clips only.

    Skips ``no_clips`` and error rows to match ``assign_clips``. Keyed
    by basename for the same reason ``_detections_by_file`` is — see
    that function's docstring.
    """
    out: Dict[str, List[Tuple[float, float]]] = {}
    for _, r in replay.iterrows():
        if bool(r.get("no_clips", False)) or (r.get("error") or ""):
            continue
        if not bool(r.get("kept", False)):
            continue
        key = Path(str(r["source_file"])).name
        out.setdefault(key, []).append(
            (float(r["clip_start_ms"]), float(r["clip_end_ms"])))
    return out


def _detection_captured(det: dict,
                        kept_clips: List[Tuple[float, float]]) -> bool:
    """A detection is captured iff at least one kept clip window (widened
    by ``CLIP_EDGE_SLOP_MS`` on each side) overlaps its window.

    Same overlap arithmetic as ``_pick_overlap`` / ``gate_recovery.
    count_overlapping_calls`` — kept inline to avoid a third join
    implementation drifting.
    """
    d_lo = float(det.get("start_time_s", 0.0)) * 1000.0
    d_hi = float(det.get("end_time_s",   0.0)) * 1000.0
    for c_lo, c_hi in kept_clips:
        lo = c_lo - CLIP_EDGE_SLOP_MS
        hi = c_hi + CLIP_EDGE_SLOP_MS
        if not (d_hi < lo or d_lo > hi):
            return True
    return False


def per_call_recall(truth: pd.DataFrame, replay: pd.DataFrame
                    ) -> Tuple[PerCallCounts, pd.DataFrame]:
    """Detection-side recall — the new primary KPI.

    Returns ``(pooled, per_species_df)``. The per-species breakdown
    groups detections by *each detection's own* ``species`` field, not
    by the clip-side top-species assignment used elsewhere.

    Denominator is every BatDetect2 detection in the corpus, so it is
    invariant to recorder config; a gate-OFF run should be high but not
    100% (calls the base detector never triggered on are still misses).
    A 100% result on gate-OFF means the join is wrong.
    """
    det_map  = _detections_by_file(truth)
    kept_map = _kept_clips_by_basename(replay)

    pooled = PerCallCounts()
    per: Dict[str, PerCallCounts] = {}
    for basename, dets in det_map.items():
        kept_clips = kept_map.get(basename, [])
        for d in dets:
            species = str(d.get("species", "")) or NO_BAT_LABEL
            captured = _detection_captured(d, kept_clips)
            pooled.n_calls_total += 1
            counts = per.setdefault(species, PerCallCounts())
            counts.n_calls_total += 1
            if captured:
                pooled.n_calls_captured += 1
                counts.n_calls_captured += 1

    rows: List[Dict[str, object]] = []
    for species, c in per.items():
        rows.append({
            "species":          species,
            "n_calls_total":    c.n_calls_total,
            "n_calls_captured": c.n_calls_captured,
            "per_call_recall":  c.recall,
        })
    df = pd.DataFrame(rows)
    if not df.empty:
        df = df.sort_values(["n_calls_total", "species"],
                            ascending=[False, True]).reset_index(drop=True)
    return pooled, df


# --- summary + per-species ---------------------------------------------------

def score(truth: pd.DataFrame, replay: pd.DataFrame) -> ScoreSummary:
    """Pooled confusion + rates over every scorable clip."""
    assignments = assign_clips(truth, replay)
    conf = ConfusionCounts()
    for a in assignments:
        conf.add(kept=a.kept, bat=a.bat_present)

    n_no_clip = int(
        replay["no_clips"].fillna(False).astype(bool).sum()
        if "no_clips" in replay.columns else 0)
    n_err = int(
        (replay["error"].fillna("").astype(str) != "").sum()
        if "error" in replay.columns else 0)

    pooled_pc, _ = per_call_recall(truth, replay)

    return ScoreSummary(
        n_clips=len(assignments),
        n_no_clip_sources=n_no_clip,
        n_error_rows=n_err,
        confusion=conf,
        per_call=pooled_pc,
        recall=conf.recall,
        fp_rate=conf.fp_rate,
        cricket_rejection_rate=conf.cricket_rejection,
        per_call_recall=pooled_pc.recall,
    )


def score_per_species(truth: pd.DataFrame,
                      replay: pd.DataFrame) -> pd.DataFrame:
    """Break confusion out by species. Non-bat clips group under
    ``__no_bat__`` so the cricket-FP + TN cells have their own row."""
    assignments = assign_clips(truth, replay)
    per: Dict[str, ConfusionCounts] = {}
    for a in assignments:
        per.setdefault(a.group, ConfusionCounts()).add(a.kept, a.bat_present)

    rows: List[Dict[str, object]] = []
    for group, c in per.items():
        rows.append({
            "group":   group,
            "n_clips": c.n_tp + c.n_fn + c.n_fp + c.n_tn,
            "n_tp": c.n_tp, "n_fn": c.n_fn,
            "n_fp": c.n_fp, "n_tn": c.n_tn,
            "recall":                 c.recall,
            "fp_rate":                c.fp_rate,
            "cricket_rejection_rate": c.cricket_rejection,
        })
    df = pd.DataFrame(rows)
    if df.empty:
        return df
    return df.sort_values(["n_clips", "group"],
                          ascending=[False, True]).reset_index(drop=True)


# --- disagreement list -------------------------------------------------------

def disagreements(truth: pd.DataFrame, replay: pd.DataFrame) -> pd.DataFrame:
    """FN + FP + every CF/Rhinolophus overlap, ranked for human review.

    Sort order: category (missed_recall → cricket_fp → cf_review), then
    within each by top_confidence descending — the operator wants the
    highest-signal rows at the top of the queue.
    """
    assignments = assign_clips(truth, replay)
    rows: List[Dict[str, object]] = []
    for a in assignments:
        category = ""
        if a.bat_present and not a.kept:
            category = "missed_recall"
        elif not a.bat_present and a.kept:
            category = "cricket_fp"
        elif any(tok in a.top_species.lower() for tok in CF_SPECIES_TOKENS):
            category = "cf_review"
        if not category:
            continue
        rows.append({
            "category":       category,
            "source_file":    a.source_file,
            "clip_wav":       a.clip_wav,
            "kept":           a.kept,
            "rejected_reason": a.rejected_reason,
            "clip_start_ms":  a.clip_start_ms,
            "clip_end_ms":    a.clip_end_ms,
            "top_species":    a.top_species,
            "top_confidence": a.top_confidence,
            "min_bandwidth_khz": a.min_bandwidth_khz,
            "max_drift_khz":     a.max_drift_khz,
            "tunable_min_bw_khz": a.tunable_min_bw_khz,
        })
    df = pd.DataFrame(rows)
    if df.empty:
        return df
    # Category-first, then confidence-descending — highest-signal rows on top.
    cat_order = {"missed_recall": 0, "cricket_fp": 1, "cf_review": 2}
    df["_cat_key"] = df["category"].map(cat_order).fillna(99)
    df = df.sort_values(["_cat_key", "top_confidence"],
                        ascending=[True, False])
    return df.drop(columns=["_cat_key"]).reset_index(drop=True)


# --- rendering ---------------------------------------------------------------

def render_report(summary: ScoreSummary, per_species: pd.DataFrame, *,
                  truth_path: Path, replay_path: Path,
                  config_label: str = "",
                  per_call_species: Optional[pd.DataFrame] = None,
                  extra_sections: str = "") -> str:
    """Markdown report body. Honesty header rides verbatim at the top."""
    lines: List[str] = []
    title = "# Echobox rough algorithm assessment"
    if config_label:
        title += f" — {config_label}"
    lines.append(title)
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(f"- truth manifest:  `{truth_path}`")
    lines.append(f"- replay manifest: `{replay_path}`")
    lines.append("")
    lines.append("## Headline")
    lines.append("")
    conf = summary.confusion
    pc = summary.per_call
    lines.append(f"- clips scored: **{summary.n_clips}** "
                 f"(sources with no clips: {summary.n_no_clip_sources}; "
                 f"error rows: {summary.n_error_rows})")
    lines.append(f"- **per-call recall (KPI 1)**: "
                 f"**{_fmt_pct(summary.per_call_recall)}** "
                 f"({pc.n_calls_captured}/{pc.n_calls_total} BatDetect2 "
                 f"detections overlapped by ≥1 kept clip)")
    lines.append(f"- **cricket rejection (KPI 2, TN / (TN+FP))**: "
                 f"**{_fmt_pct(summary.cricket_rejection_rate)}** "
                 f"({conf.n_tn}/{conf.n_tn + conf.n_fp})")
    lines.append("")
    lines.append("### Legacy per-clip metrics (retired as decision KPIs)")
    lines.append("")
    lines.append("Per-clip recall is distorted by clip creation — the "
                 "denominator moves with recorder config — and per-file "
                 "recall is distorted by the arbitrary 1-minute chopping. "
                 "Kept below for continuity with earlier reports.")
    lines.append("")
    lines.append(f"- bat-recall (kept ∧ bat / bat, per-clip): "
                 f"{_fmt_pct(summary.recall)} "
                 f"({conf.n_tp}/{conf.n_tp + conf.n_fn})")
    lines.append(f"- cricket-FP rate (FP / (FP+TN), per-clip): "
                 f"{_fmt_pct(summary.fp_rate)} "
                 f"({conf.n_fp}/{conf.n_fp + conf.n_tn})")
    lines.append("")
    lines.append("## Per-call recall by species")
    lines.append("")
    if per_call_species is None or per_call_species.empty:
        lines.append("_no BatDetect2 detections in corpus_")
    else:
        lines.append(_df_to_md_table(per_call_species))
    lines.append("")
    lines.append("## Per-species confusion (per-clip)")
    lines.append("")
    if per_species.empty:
        lines.append("_no scorable clips_")
    else:
        lines.append(_df_to_md_table(per_species))
    lines.append("")
    lines.append("See `disagreements.csv` for the per-clip rows that missed "
                 "recall, tripped the cricket FP, or hit a CF/Rhinolophus "
                 "species — sorted with the highest-confidence rows first.")
    lines.append("")
    if extra_sections:
        lines.append(extra_sections)
    return "\n".join(lines)


def _fmt_pct(x: Optional[float]) -> str:
    if x is None:
        return "n/a"
    return f"{100.0 * x:.1f}%"


_RATE_COLUMNS = {"recall", "fp_rate", "cricket_rejection_rate",
                 "per_call_recall"}


def _df_to_md_table(df: pd.DataFrame) -> str:
    if df.empty:
        return ""
    cols = list(df.columns)
    header = "| " + " | ".join(cols) + " |"
    sep    = "|" + "|".join(["---"] * len(cols)) + "|"
    body_lines = []
    for _, row in df.iterrows():
        cells = []
        for c in cols:
            v = row[c]
            if v is None or (isinstance(v, float) and pd.isna(v)):
                cells.append("n/a" if c in _RATE_COLUMNS else "")
            elif isinstance(v, float):
                cells.append(f"{v:.3f}")
            else:
                cells.append(str(v))
        body_lines.append("| " + " | ".join(cells) + " |")
    return "\n".join([header, sep, *body_lines])


# --- top-level entry point --------------------------------------------------

def score_and_write(truth_path: Path, replay_path: Path,
                    output_dir: Path, *,
                    config_label: str = "",
                    ) -> ScoreSummary:
    """Load both manifests, emit report.md + results.csv + disagreements.csv
    + per_call_by_species.csv."""
    truth  = read_truth_manifest(truth_path)
    replay = read_replay_manifest(replay_path)
    assert_basenames_unique(truth,  column="file",        source="truth")
    assert_basenames_unique(replay, column="source_file", source="replay")

    summary            = score(truth, replay)
    per_species        = score_per_species(truth, replay)
    _, per_call_species = per_call_recall(truth, replay)
    disagrees          = disagreements(truth, replay)

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "report.md").write_text(render_report(
        summary, per_species,
        truth_path=truth_path, replay_path=replay_path,
        config_label=config_label,
        per_call_species=per_call_species))
    per_species.to_csv(output_dir / "results.csv", index=False)
    per_call_species.to_csv(output_dir / "per_call_by_species.csv", index=False)
    disagrees.to_csv(output_dir / "disagreements.csv", index=False)
    return summary


# --- baseline-vs-shorter diff ------------------------------------------------

def render_config_diff(labels: List[str],
                       summaries: List[ScoreSummary]) -> str:
    """One markdown table summarising two (or more) configs side-by-side.

    Columns: config label, clips scored, **per-call recall (KPI 1)**,
    cricket-rejection (KPI 2, num/den), plus the retired per-clip
    bat-recall and cricket-FP kept for continuity. Flat table — no
    split into sub-tables — so a diff of two rows is trivially readable.
    """
    lines: List[str] = []
    lines.append("## Config comparison")
    lines.append("")
    lines.append(
        "| config | clips | per-call recall (KPI 1) "
        "| cricket-rejection (KPI 2) "
        "| per-clip recall (legacy) | per-clip cricket-FP (legacy) |")
    lines.append("|---|---|---|---|---|---|")
    for label, s in zip(labels, summaries):
        c = s.confusion
        pc = s.per_call
        lines.append(
            f"| {label} | {s.n_clips} | "
            f"{_fmt_pct(s.per_call_recall)} "
            f"({pc.n_calls_captured}/{pc.n_calls_total}) | "
            f"{_fmt_pct(s.cricket_rejection_rate)} "
            f"({c.n_tn}/{c.n_tn + c.n_fp}) | "
            f"{_fmt_pct(s.recall)} ({c.n_tp}/{c.n_tp + c.n_fn}) | "
            f"{_fmt_pct(s.fp_rate)} ({c.n_fp}/{c.n_fp + c.n_tn}) |")
    lines.append("")
    lines.append(
        "KPI 1 target: per-call recall ≥ 90%. KPI 2 target: cricket "
        "rejection ≥ 70%. Per-clip metrics retained for continuity but "
        "retired as decision metrics — per-clip denominator moves with "
        "recorder config, per-file denominator is arbitrary chopping.")
    return "\n".join(lines)
