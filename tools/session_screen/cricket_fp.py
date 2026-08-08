# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Characterise the cricket-FP leakers — clips the recorder kept even
though no BatDetect2 detection overlapped them.

Data model: the score.py per-clip join produced ``disagreements.csv``
rows in category ``cricket_fp``. Their aggregate features
(``min_bandwidth_khz``, ``max_drift_khz``) are already in the manifest,
but only aggregated over ``gate_rejected`` events — for a kept clip
those aggregates are 0 because no event in it was rejected. To
characterise the *passing* events (the ones the gate let through), we
re-parse the raw sidecar JSONs kept under ``<config>/replay_manifest.replay/``
by ``echobox-replay``. No re-run of replay or BatDetect2 is required.

The trade-off table is honest: for each candidate ``min_bandwidth_khz``
nudge, count leakers reclaimed (FP → TN) AND currently-TP clips lost
(TP → FN). Applying the nudge is out of scope; the auditor gets a
number pair to weigh.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import pandas as pd


# --- feature extraction ------------------------------------------------------

FEATURE_KEYS = ("bandwidth_khz", "drift_khz", "path_ratio", "mono_fraction")
"""The four sweep-gate features per event, as written by the shipping
recorder into every sidecar's ``events[]`` array. See
``src/dsp/algorithms/BandEnergyDetector/…`` for the gate logic."""


@dataclass
class ClipFeatures:
    """Aggregated per-clip features across all events in the clip.

    We keep the *max* bandwidth (the event that most easily passes the
    "at least this wide" test), the *min* drift (the event with the
    least drift, most bat-like), the *min* path_ratio, and the *max*
    mono_fraction. Each aggregation is chosen so a threshold-nudge
    simulation asks "would this clip still pass the gate?" cleanly:
    a clip passes iff *any* of its events passes, so we track the
    most-passing event per feature.
    """
    clip_wav:          str
    source_file:       str
    max_bandwidth_khz: float
    min_drift_khz:     float
    min_path_ratio:    float
    max_mono_fraction: float
    n_events:          int


def _to_float(v) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def _clip_features(sidecar: Path) -> Optional[ClipFeatures]:
    """Parse one sidecar into a ClipFeatures row. Returns None if the
    file is malformed — the caller can decide to skip or fail."""
    try:
        doc = json.loads(sidecar.read_text())
    except Exception:
        return None
    events = doc.get("events") or []
    if not events:
        return ClipFeatures(
            clip_wav=str(sidecar.with_suffix(".wav")),
            source_file="",
            max_bandwidth_khz=0.0, min_drift_khz=0.0,
            min_path_ratio=0.0, max_mono_fraction=0.0,
            n_events=0,
        )
    bws  = [_to_float(e.get("bandwidth_khz"))  for e in events]
    drs  = [_to_float(e.get("drift_khz"))      for e in events]
    prs  = [_to_float(e.get("path_ratio"))     for e in events]
    mfs  = [_to_float(e.get("mono_fraction"))  for e in events]
    return ClipFeatures(
        clip_wav=str(sidecar.with_suffix(".wav")),
        source_file="",
        max_bandwidth_khz=float(max(bws)),
        min_drift_khz=float(min(drs)),
        min_path_ratio=float(min(prs)),
        max_mono_fraction=float(max(mfs)),
        n_events=len(events),
    )


def load_clip_features(replay_root: Path,
                       clip_wavs: List[str]) -> pd.DataFrame:
    """For each ``clip_wav`` (path relative to ``replay_root``), locate
    the sidecar JSON and extract the aggregate features.

    A missing sidecar yields an empty-feature row so downstream code can
    branch on ``n_events==0`` without a special-case null check.
    """
    rows: List[dict] = []
    for wav_rel in clip_wavs:
        wav_rel = str(wav_rel)
        if not wav_rel:
            continue
        sidecar = replay_root / wav_rel.replace(".wav", ".json")
        if not sidecar.exists():
            rows.append({
                "clip_wav": wav_rel, "max_bandwidth_khz": 0.0,
                "min_drift_khz": 0.0, "min_path_ratio": 0.0,
                "max_mono_fraction": 0.0, "n_events": 0,
            })
            continue
        feats = _clip_features(sidecar)
        if feats is None:
            rows.append({
                "clip_wav": wav_rel, "max_bandwidth_khz": 0.0,
                "min_drift_khz": 0.0, "min_path_ratio": 0.0,
                "max_mono_fraction": 0.0, "n_events": 0,
            })
            continue
        rows.append({
            "clip_wav":          wav_rel,
            "max_bandwidth_khz": feats.max_bandwidth_khz,
            "min_drift_khz":     feats.min_drift_khz,
            "min_path_ratio":    feats.min_path_ratio,
            "max_mono_fraction": feats.max_mono_fraction,
            "n_events":          feats.n_events,
        })
    return pd.DataFrame(rows)


# --- distribution comparison ------------------------------------------------

_PCTILES = (10, 25, 50, 75, 90)
"""Distribution summary quantiles. Wider than a boxplot's 25/50/75 so
the tails are visible — the trade-off question ("do leakers cluster
near threshold?") lives in the low-percentile end."""


def summarise_feature(df: pd.DataFrame, feature: str) -> Dict[str, float]:
    """Return ``{p10, p25, p50, p75, p90, mean}`` for one feature column."""
    if df.empty or feature not in df.columns:
        return {f"p{q}": float("nan") for q in _PCTILES} | {"mean": float("nan")}
    vals = df[feature].dropna()
    out: Dict[str, float] = {}
    for q in _PCTILES:
        out[f"p{q}"] = float(np.percentile(vals, q)) if len(vals) else float("nan")
    out["mean"] = float(vals.mean()) if len(vals) else float("nan")
    return out


# --- trade-off table --------------------------------------------------------

BANDWIDTH_NUDGES_KHZ = (1.0, 1.1, 1.2, 1.5, 2.0)
"""Candidate min_bandwidth_khz thresholds above the shipping default
(0.9 kHz). Chosen to bracket "tiny nudge" (1.0) → "aggressive" (2.0).
Widening beyond 2.0 kHz starts to threaten Pipistrellus recall — the
narrowest passing Pipistrellus events sit ~1-2 kHz — so the trade-off
becomes uninteresting past that."""


def bandwidth_tradeoff_table(fp_feats: pd.DataFrame,
                             tp_feats: pd.DataFrame,
                             *, nudges_khz: Tuple[float, ...] = BANDWIDTH_NUDGES_KHZ,
                             baseline_khz: float = 0.9) -> pd.DataFrame:
    """For each candidate ``min_bandwidth_khz`` threshold ``T``:

      - **leakers reclaimed**: current FP clips whose
        ``max_bandwidth_khz`` < T (would be rejected under T).
      - **TP lost**: currently-kept bat clips whose ``max_bandwidth_khz``
        < T (would be rejected under T = recall loss).

    Returns a DataFrame ready for markdown rendering — the operator
    reads it as a straight sensitivity/specificity trade-off.

    ``baseline_khz`` is included in the output as the reference row.
    """
    rows: List[dict] = []
    for T in (baseline_khz,) + tuple(nudges_khz):
        n_fp_reclaimed = int((fp_feats["max_bandwidth_khz"] < T).sum()) \
            if not fp_feats.empty else 0
        n_tp_lost      = int((tp_feats["max_bandwidth_khz"] < T).sum()) \
            if not tp_feats.empty else 0
        rows.append({
            "min_bandwidth_khz": T,
            "n_fp_total":        int(len(fp_feats)),
            "n_fp_reclaimed":    n_fp_reclaimed,
            "fp_reclaimed_frac": (n_fp_reclaimed / len(fp_feats)
                                  if len(fp_feats) else float("nan")),
            "n_tp_total":        int(len(tp_feats)),
            "n_tp_lost":         n_tp_lost,
            "tp_lost_frac":      (n_tp_lost / len(tp_feats)
                                  if len(tp_feats) else float("nan")),
        })
    return pd.DataFrame(rows)


# --- rendering --------------------------------------------------------------

def render_profile_md(config_label: str, replay_root: Path,
                      fp_feats: pd.DataFrame,
                      tn_feats: pd.DataFrame,
                      tp_feats: pd.DataFrame,
                      tradeoff: pd.DataFrame,
                      *, n_tp_sampled: Optional[int] = None,
                      n_tn_sampled: Optional[int] = None) -> str:
    """Emit the cricket_fp_profile.md body for one config.

    Distribution table has one row per feature, columns for each
    ``(bucket × quantile)``. Trade-off table is the sensitivity /
    specificity view.
    """
    lines: List[str] = []
    lines.append(f"# Cricket-FP leaker characterisation — {config_label}")
    lines.append("")
    lines.append(
        f"Reads sidecars from `{replay_root}`. No replay re-run.")
    lines.append("")
    lines.append(f"- leakers (FP) profiled: **{len(fp_feats)}**")
    if n_tn_sampled is not None:
        lines.append(f"- TN comparison sample: **{n_tn_sampled}** clips")
    if n_tp_sampled is not None:
        lines.append(f"- TP comparison sample: **{n_tp_sampled}** clips "
                     f"(all TP if no sampling was needed)")
    lines.append("")
    lines.append("## Feature distributions")
    lines.append("")
    lines.append(
        "Per-clip aggregates across the events in the clip. "
        "Aggregation chosen so a threshold-nudge simulation reads "
        "cleanly: a clip passes the gate iff *any* event passes, so we "
        "track the most-passing event per feature "
        "(max bandwidth, min drift, min path_ratio, max mono_fraction). "
        "Distributions are ``p10 / p25 / p50 / p75 / p90 (mean)`` — "
        "the low percentiles are where the "
        "\"do leakers cluster near threshold?\" question lives.")
    lines.append("")
    lines.append("| feature | bucket | p10 | p25 | p50 | p75 | p90 | mean |")
    lines.append("|---|---|---|---|---|---|---|---|")
    display_features = ("max_bandwidth_khz", "min_drift_khz",
                        "min_path_ratio", "max_mono_fraction")
    for feat in display_features:
        for bucket_label, bucket_df in (("FP (leakers)", fp_feats),
                                        ("TN (correctly gated)", tn_feats),
                                        ("TP (kept bats)", tp_feats)):
            s = summarise_feature(bucket_df, feat)
            row = (
                f"| {feat} | {bucket_label} "
                f"| {s['p10']:.3f} | {s['p25']:.3f} | {s['p50']:.3f} "
                f"| {s['p75']:.3f} | {s['p90']:.3f} | {s['mean']:.3f} |")
            lines.append(row)
    lines.append("")
    lines.append("## min_bandwidth_khz trade-off (estimated, not applied)")
    lines.append("")
    lines.append(
        "For each candidate ``min_bandwidth_khz`` threshold ``T``: "
        "clips whose ``max_bandwidth_khz`` < ``T`` would be gate-"
        "rejected under the new threshold. Reclaim = FP flipped to TN; "
        "cost = TP flipped to FN (recall loss). Baseline (shipping) is "
        f"``0.9 kHz``.")
    lines.append("")
    lines.append(
        "| min_bw_khz | leakers reclaimed | reclaim frac | TP lost | "
        "TP lost frac |")
    lines.append("|---|---|---|---|---|")
    for _, r in tradeoff.iterrows():
        lines.append(
            f"| {r['min_bandwidth_khz']:.2f} | "
            f"{int(r['n_fp_reclaimed'])} / {int(r['n_fp_total'])} | "
            f"{100.0 * float(r['fp_reclaimed_frac']):.1f}% | "
            f"{int(r['n_tp_lost'])} / {int(r['n_tp_total'])} | "
            f"{100.0 * float(r['tp_lost_frac']):.2f}% |")
    lines.append("")
    lines.append(
        "**Read the trade-off, do not apply.** The auditor asked for an "
        "estimate; the change itself belongs in a later tuning task "
        "scored against this frozen baseline. See the honesty ceiling "
        "in the top-level ``report.md`` — caveat #4 (no human "
        "adjudication) means the reclaim count is the number of leakers "
        "whose *feature* would fail the tighter gate, not the number "
        "that a human would confirm as truly cricket.")
    lines.append("")
    return "\n".join(lines)


# --- top-level driver -------------------------------------------------------

def characterise_one_config(replay_root: Path,
                            disagreements_csv: Path,
                            replay_df: pd.DataFrame,
                            truth_df: pd.DataFrame,
                            output_dir: Path,
                            *, tn_sample_n: int = 200,
                            tp_sample_n: Optional[int] = None,
                            config_label: str = "",
                            random_seed: int = 42
                            ) -> Path:
    """Characterise one config's cricket-FP leakers.

    ``replay_root`` — the ``<config>/replay_manifest.replay/`` dir the
    C++ tool wrote its sidecar tree into.
    ``disagreements_csv`` — the score.py output, filtered to
    ``category == "cricket_fp"`` for the leakers.
    ``replay_df`` — the per-clip replay manifest.
    ``truth_df`` — the per-file truth manifest.

    Emits ``cricket_fp_features.csv`` + ``cricket_fp_profile.md`` into
    ``output_dir``. Returns the profile.md path.
    """
    from .score import assign_clips, NO_BAT_LABEL

    dg = pd.read_csv(disagreements_csv)
    fp_rows = dg[dg["category"] == "cricket_fp"].copy()
    fp_wavs = list(fp_rows["clip_wav"].dropna().astype(str))
    fp_feats = load_clip_features(replay_root, fp_wavs)

    # Sample TN + TP for feature comparison. TN by construction are
    # discarded clips that did not overlap a bat detection; TP are
    # kept clips that DID overlap. Both live in the assign_clips
    # output, so we derive them from the existing join.
    assignments = assign_clips(truth_df, replay_df)
    tn_wavs = [a.clip_wav for a in assignments
               if (not a.kept) and (not a.bat_present)]
    tp_wavs = [a.clip_wav for a in assignments
               if a.kept and a.bat_present]

    rng = np.random.default_rng(random_seed)
    if tn_sample_n and len(tn_wavs) > tn_sample_n:
        tn_wavs = list(rng.choice(tn_wavs, size=tn_sample_n, replace=False))
    if tp_sample_n and len(tp_wavs) > tp_sample_n:
        tp_wavs = list(rng.choice(tp_wavs, size=tp_sample_n, replace=False))
    tn_feats = load_clip_features(replay_root, tn_wavs)
    tp_feats = load_clip_features(replay_root, tp_wavs)

    fp_feats["bucket"] = "FP"
    tn_feats["bucket"] = "TN"
    tp_feats["bucket"] = "TP"
    all_feats = pd.concat([fp_feats, tn_feats, tp_feats],
                          ignore_index=True)

    tradeoff = bandwidth_tradeoff_table(fp_feats, tp_feats)

    output_dir.mkdir(parents=True, exist_ok=True)
    all_feats.to_csv(output_dir / "cricket_fp_features.csv", index=False)
    md_path = output_dir / "cricket_fp_profile.md"
    md_path.write_text(render_profile_md(
        config_label, replay_root,
        fp_feats.drop(columns=["bucket"], errors="ignore"),
        tn_feats.drop(columns=["bucket"], errors="ignore"),
        tp_feats.drop(columns=["bucket"], errors="ignore"),
        tradeoff,
        n_tp_sampled=len(tp_feats), n_tn_sampled=len(tn_feats)))
    return md_path
