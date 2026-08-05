# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Compare the truth manifest (BatDetect2) against the replay manifest
(Echobox offline harness) and emit a rough per-file recall / cricket-FP
report.

Nothing here talks to BatDetect2 or the native harness — pure pandas over
the two parquet caches. Kept import-light so unit tests can run in a bare
venv.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

import pandas as pd

from .manifest import read_replay_manifest, read_truth_manifest
from . import rejected as rejected_mod


HEADLINE_CAVEATS = (
    "This is a **rough tuning proxy, not a device-exact benchmark**.\n"
    "  1. BatDetect2 is NOT ground truth. It is documented to confuse crickets\n"
    "     with horseshoe (Rhinolophus / CF) bats — that is exactly our known\n"
    "     weak spot. `disagreements.csv` exists so a human can spot-check\n"
    "     these; do not treat the headline numbers as authoritative on CF cases.\n"
    "  2. The Echobox replay path is x86 with `-ffast-math`; the device is ARM.\n"
    "     File-level agree/disagree numbers are fine for local tuning; per-event\n"
    "     margins near the sweep-shape gate thresholds are indicative only."
)


CF_SPECIES_TOKENS = ("rhinolophus", "horseshoe")
"""Any BatDetect2 species string containing one of these (case-insensitive)
is treated as a CF/Rhinolophus hit and forced into ``disagreements.csv``
for human review — the plan calls these out as our weak spot."""


@dataclass
class ScoreSummary:
    """Aggregate confusion + rates for one comparison run.

    Rates are None when the denominator is zero — the report prints an
    honest "n/a" rather than a misleading 0.0.
    """
    n_files:  int
    n_tp:     int   # BatDetect2 bat + app would_save
    n_fn:     int   # BatDetect2 bat + app not save (missed recall)
    n_fp:     int   # BatDetect2 no-bat + app would_save (cricket FP)
    n_tn:     int   # BatDetect2 no-bat + app not save
    n_errors: int
    recall:   Optional[float]
    fp_rate:  Optional[float]

    def as_row(self) -> Dict[str, object]:
        return {
            "n_files":  self.n_files,
            "n_tp":     self.n_tp,
            "n_fn":     self.n_fn,
            "n_fp":     self.n_fp,
            "n_tn":     self.n_tn,
            "n_errors": self.n_errors,
            "recall":   self.recall,
            "fp_rate":  self.fp_rate,
        }


def _safe_ratio(num: int, den: int) -> Optional[float]:
    return num / den if den > 0 else None


def _join(truth: pd.DataFrame, replay: pd.DataFrame) -> pd.DataFrame:
    """Outer-join on ``file`` so we can see files present in only one side.

    The ``file`` column carries the same relative path from both sides
    (both walk the same input dir), so a plain merge is enough.
    """
    return truth.merge(replay, on="file", how="outer",
                       suffixes=("_truth", "_replay"))


def _is_cf(species: object) -> bool:
    if not isinstance(species, str):
        return False
    lowered = species.lower()
    return any(tok in lowered for tok in CF_SPECIES_TOKENS)


def score(truth: pd.DataFrame, replay: pd.DataFrame) -> ScoreSummary:
    """Compute overall confusion + rates.

    Files with an error on either side count into ``n_errors`` and are
    excluded from the confusion counts — refusing to score is more honest
    than silently treating "harness failed" as "app would not save".
    """
    joined = _join(truth, replay)
    # A row is scorable only if BOTH sides succeeded (no error, non-null cols).
    has_truth  = joined["bat_present"].notna() & (
        joined.get("error_truth", "").fillna("") == "")
    has_replay = joined["would_save"].notna() & (
        joined.get("error_replay", "").fillna("") == "")
    scorable   = has_truth & has_replay
    errors     = int((~scorable).sum())

    s = joined[scorable]
    bat  = s["bat_present"].astype(bool)
    kept = s["would_save"].astype(bool)

    n_tp = int(( bat &  kept).sum())
    n_fn = int(( bat & ~kept).sum())
    n_fp = int((~bat &  kept).sum())
    n_tn = int((~bat & ~kept).sum())
    return ScoreSummary(
        n_files=len(s), n_tp=n_tp, n_fn=n_fn, n_fp=n_fp, n_tn=n_tn,
        n_errors=errors,
        recall=_safe_ratio(n_tp, n_tp + n_fn),
        fp_rate=_safe_ratio(n_fp, n_fp + n_tn),
    )


def score_per_species(truth: pd.DataFrame, replay: pd.DataFrame) -> pd.DataFrame:
    """Break the score out by BatDetect2's top_species field.

    Non-bat files (bat_present=False) are grouped under ``"__no_bat__"`` so
    the cricket-FP rate has a clear per-group cell.
    """
    joined = _join(truth, replay)
    has_truth  = joined["bat_present"].notna() & (
        joined.get("error_truth", "").fillna("") == "")
    has_replay = joined["would_save"].notna() & (
        joined.get("error_replay", "").fillna("") == "")
    s = joined[has_truth & has_replay].copy()

    s["group"] = s.apply(
        lambda r: r["top_species"] if bool(r["bat_present"]) else "__no_bat__",
        axis=1,
    )

    rows: List[Dict[str, object]] = []
    for group, g in s.groupby("group", dropna=False):
        bat  = g["bat_present"].astype(bool)
        kept = g["would_save"].astype(bool)
        n_tp = int(( bat &  kept).sum())
        n_fn = int(( bat & ~kept).sum())
        n_fp = int((~bat &  kept).sum())
        n_tn = int((~bat & ~kept).sum())
        rows.append({
            "group":   str(group) if group is not None else "",
            "n_files": len(g),
            "n_tp":    n_tp, "n_fn": n_fn, "n_fp": n_fp, "n_tn": n_tn,
            "recall":  _safe_ratio(n_tp, n_tp + n_fn),
            "fp_rate": _safe_ratio(n_fp, n_fp + n_tn),
        })
    return pd.DataFrame(rows).sort_values(
        ["n_files", "group"], ascending=[False, True]).reset_index(drop=True)


def disagreements(truth: pd.DataFrame, replay: pd.DataFrame) -> pd.DataFrame:
    """Every file where BatDetect2 and Echobox disagree, plus every
    CF/Rhinolophus hit (whether or not the two sides agreed).

    CF rows are always included because BatDetect2's known cricket↔CF
    confusion means "agree" tells us the least on those.
    """
    joined = _join(truth, replay)
    has_truth  = joined["bat_present"].notna() & (
        joined.get("error_truth", "").fillna("") == "")
    has_replay = joined["would_save"].notna() & (
        joined.get("error_replay", "").fillna("") == "")

    disagree_mask = (
        has_truth & has_replay
        & (joined["bat_present"].astype(bool)
           != joined["would_save"].astype(bool))
    )
    cf_mask = joined["top_species"].apply(_is_cf).fillna(False)
    keep = disagree_mask | cf_mask

    out = joined[keep].copy()
    # Row category so a human reader can prioritise: missed recall, FP,
    # and CF-review buckets are the useful sort keys.
    def _category(r) -> str:
        if not has_truth.loc[r.name] or not has_replay.loc[r.name]:
            return "error"
        bat  = bool(r["bat_present"])
        kept = bool(r["would_save"])
        if _is_cf(r.get("top_species")):
            return "cf_review"
        if bat and not kept:
            return "missed_recall"
        if not bat and kept:
            return "cricket_fp"
        return "agree"

    # An empty frame's apply(axis=1) returns a DataFrame, not a Series,
    # which pandas won't assign back to a single column. Guard that case.
    if out.empty:
        out["category"] = pd.Series([], dtype=str)
    else:
        out["category"] = out.apply(_category, axis=1)
    cols = ["file", "category", "bat_present", "would_save",
            "top_species", "top_confidence",
            "n_would_save", "n_would_discard", "discard_reasons",
            "error_truth", "error_replay"]
    present = [c for c in cols if c in out.columns]
    return out[present].sort_values(["category", "file"]).reset_index(drop=True)


# --- rendering --------------------------------------------------------------

def render_report(summary: ScoreSummary, per_species: pd.DataFrame, *,
                  truth_path: Path, replay_path: Path,
                  rejected_section: str = "") -> str:
    """Markdown report body. Caveats always ride at the top."""
    lines: List[str] = []
    lines.append("# Echobox validation quick-win — report")
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(f"- truth manifest:  `{truth_path}`")
    lines.append(f"- replay manifest: `{replay_path}`")
    lines.append("")
    lines.append("## Headline (per-file, all species pooled)")
    lines.append("")
    lines.append(f"- files scored: **{summary.n_files}**"
                 f" (skipped for errors: {summary.n_errors})")
    lines.append(f"- recall (BatDetect2 bat → app kept): "
                 f"**{_fmt_pct(summary.recall)}** "
                 f"({summary.n_tp}/{summary.n_tp + summary.n_fn})")
    lines.append(f"- cricket-FP (BatDetect2 no-bat → app kept): "
                 f"**{_fmt_pct(summary.fp_rate)}** "
                 f"({summary.n_fp}/{summary.n_fp + summary.n_tn})")
    lines.append("")
    lines.append("## Per-species breakdown")
    lines.append("")
    if per_species.empty:
        lines.append("_no scorable files_")
    else:
        lines.append(_df_to_md_table(per_species))
    lines.append("")
    lines.append("See `disagreements.csv` for the per-file rows that missed "
                 "recall, tripped the cricket FP, or hit a CF/Rhinolophus "
                 "species (always listed for human review).")
    lines.append("")
    if rejected_section:
        lines.append(rejected_section)
    return "\n".join(lines)


def _fmt_pct(x: Optional[float]) -> str:
    if x is None:
        return "n/a"
    return f"{100.0 * x:.1f}%"


_RATE_COLUMNS = {"recall", "fp_rate"}
"""Columns where a missing value legitimately means "n/a (0/0 denominator)"
rather than "unknown". Rendered as ``n/a`` instead of a blank cell so the
report never looks like it just forgot a number."""


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
                    output_dir: Path,
                    *,
                    rejected_dir: Optional[Path] = None,
                    rejected_truth_path: Optional[Path] = None,
                    ) -> ScoreSummary:
    """Load both manifests, emit ``report.md`` + ``results.csv`` +
    ``disagreements.csv`` into ``output_dir``. Returns the headline summary
    so a CLI caller can print it directly.

    When ``rejected_dir`` is provided, the report gets an extra section
    reading sidecars from that dir (the shipping app's ``--save-rejected``
    output). If ``rejected_truth_path`` is also given, BatDetect2 hits
    over the rejected clips are joined in as "real bats the gate
    discarded" — the more trustworthy signal (see rejected.py).
    """
    truth  = read_truth_manifest(truth_path)
    replay = read_replay_manifest(replay_path)

    summary     = score(truth, replay)
    per_species = score_per_species(truth, replay)
    disagrees   = disagreements(truth, replay)

    rejected_section = ""
    if rejected_dir is not None:
        rej_df = rejected_mod.load_rejected_dir(rejected_dir)
        truth_join = None
        if rejected_truth_path is not None and rejected_truth_path.exists():
            truth_join = read_truth_manifest(rejected_truth_path)
        rej_summary = rejected_mod.summarise(rej_df, truth_join=truth_join)
        rejected_section = rejected_mod.render_section(rej_summary)
        if not rej_df.empty:
            rej_df.to_csv(output_dir / "rejected_summary.csv", index=False)

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "report.md").write_text(render_report(
        summary, per_species,
        truth_path=truth_path, replay_path=replay_path,
        rejected_section=rejected_section))
    per_species.to_csv(output_dir / "results.csv", index=False)
    disagrees.to_csv(output_dir / "disagreements.csv", index=False)
    return summary
