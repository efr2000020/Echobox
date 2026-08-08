# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Gate re-analysis using the REAL 4-feature OR gate — no replay re-run.

Previous auditor report used a single-knob mental model of the gate
(bandwidth ≥ min_bw). The real gate at
``BandEnergyDetector.cpp:348-352`` is:

    pass = (bw ≥ min_bandwidth_khz)
         OR (drift ≥ sweep_drift_khz
             AND path_ratio    ≤ sweep_path_ratio_max
             AND mono_fraction ≥ sweep_mono_frac_min)

Plus a temporal rep-guard veto that only *downgrades* accepts. Plus a
6-frame provisional decision using the same OR clause.

This module simulates the OR gate against every REJECTED clip's
sidecar events, at swept tunables, and for each candidate setting
reports:

  - **calls recovered** — sum of BatDetect2 detections whose windows
    overlap the newly-kept clip windows (the per-call objective the
    plan asked for).
  - **output volume** — clip count and total bytes (energy cost on the
    solar Pi).
  - **cricket leaks** — newly-kept clips with no bat overlap.
  - **calls-per-KB** — the selection metric.

**Known limitation — temporal rep-guard veto is not simulated.** The
rep-guard's rate/CV stats (``rep.rate_hz``, ``rep.cv_idi``, onset ring)
are not persisted in current sidecars, so a clip that would be
resurrected by the OR clause could still be re-vetoed on device by the
temporal guard. That makes the ``calls_recovered`` number here an
**upper bound**. Adding the veto features to sidecars + one replay
re-run is the honest fix if the recommended setting is on the borderline.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Tuple

import numpy as np
import pandas as pd


# --- shipping-default tunables (from a sidecar's detector.tunables) ---------

@dataclass
class GateTunables:
    """The 4 knobs the OR gate reads. Defaults = shipping values."""
    min_bandwidth_khz:    float = 0.9
    sweep_drift_khz:      float = 8.0
    sweep_path_ratio_max: float = 1.6
    sweep_mono_frac_min:  float = 0.7

    def passes(self, bw_khz: float, drift_khz: float,
               path_ratio: float, mono_frac: float) -> bool:
        """One event's gate decision. Matches the C++ OR clause verbatim."""
        return ((bw_khz >= self.min_bandwidth_khz)
                or (drift_khz >= self.sweep_drift_khz
                    and path_ratio    <= self.sweep_path_ratio_max
                    and mono_frac     >= self.sweep_mono_frac_min))


# --- rejected-clip simulation ----------------------------------------------

def _clip_would_pass_or(events: List[dict], tune: GateTunables) -> bool:
    """A clip passes if any of its events pass the OR gate.

    This mirrors the recorder-side aggregation: the recorder keeps a
    clip iff at least one event fired that the detector accepted (i.e.,
    ``gate_rejected=false``). We repeat that here under the swept tunables.
    """
    for e in events:
        if tune.passes(
            bw_khz    = float(e.get("bandwidth_khz", 0.0)),
            drift_khz = float(e.get("drift_khz", 0.0)),
            path_ratio= float(e.get("path_ratio", 0.0)),
            mono_frac = float(e.get("mono_fraction", 0.0)),
        ):
            return True
    return False


def _load_sidecar_events(json_path: Path) -> List[dict]:
    try:
        return list(json.loads(json_path.read_text()).get("events") or [])
    except Exception:
        return []


@dataclass
class ClipRecord:
    """Minimal info per rejected clip for the sweep."""
    source_basename:   str
    clip_wav:          str
    clip_start_ms:     float
    clip_end_ms:       float
    clip_duration_ms:  float
    events:            List[dict]


def load_rejected_clips(replay_root: Path,
                        replay_df: pd.DataFrame) -> List[ClipRecord]:
    """Enumerate every rejected clip in the replay manifest and load its
    sidecar events. ``replay_root`` is ``<config>/replay_manifest.replay/``.
    """
    rejected = replay_df[
        (~replay_df["kept"].astype(bool))
        & (~replay_df["no_clips"].fillna(False).astype(bool))
    ]
    out: List[ClipRecord] = []
    for _, r in rejected.iterrows():
        wav_rel = str(r["clip_wav"] or "")
        if not wav_rel:
            continue
        sidecar = replay_root / wav_rel.replace(".wav", ".json")
        if not sidecar.exists():
            continue
        events = _load_sidecar_events(sidecar)
        out.append(ClipRecord(
            source_basename=Path(str(r["source_file"])).name,
            clip_wav=wav_rel,
            clip_start_ms=float(r["clip_start_ms"]),
            clip_end_ms=float(r["clip_end_ms"]),
            clip_duration_ms=float(r["clip_duration_ms"]),
            events=events,
        ))
    return out


# --- per-call scoring ------------------------------------------------------

def _detections_by_basename(truth: pd.DataFrame) -> Dict[str, List[Tuple[float, float]]]:
    """Return basename → list of ``(start_ms, end_ms)`` bat detection windows."""
    out: Dict[str, List[Tuple[float, float]]] = {}
    for _, r in truth.iterrows():
        if not bool(r["bat_present"]):
            continue
        basename = Path(str(r["file"])).name
        try:
            dets = json.loads(r["detections_json"] or "[]")
        except Exception:
            dets = []
        out[basename] = [
            (float(d.get("start_time_s", 0.0)) * 1000.0,
             float(d.get("end_time_s",   0.0)) * 1000.0)
            for d in dets
        ]
    return out


def count_overlapping_calls(clip: ClipRecord,
                            det_windows: List[Tuple[float, float]],
                            *, slop_ms: float = 20.0) -> int:
    """Count BatDetect2 detection windows in this source WAV that
    overlap the clip window (± slop). This is the number of *calls*
    the clip would preserve if it were kept."""
    lo = clip.clip_start_ms - slop_ms
    hi = clip.clip_end_ms + slop_ms
    n = 0
    for d_lo, d_hi in det_windows:
        if not (d_hi < lo or d_lo > hi):
            n += 1
    return n


def clip_bytes(clip: ClipRecord, sample_rate: int = 384_000,
               bytes_per_sample: int = 2) -> int:
    """Bytes on disk / bytes-fed-to-BatDetect2-on-Pi for this clip.

    Matches canonical 16-bit PCM mono WAV. Header (44 bytes) is O(1)
    per clip and we ignore it here — the customer's BatDetect2 call
    cost scales with the audio payload, not the RIFF chunk overhead."""
    n_samples = int(clip.clip_duration_ms * sample_rate / 1000.0)
    return n_samples * bytes_per_sample


# --- sweep -----------------------------------------------------------------

@dataclass
class RecoveryPoint:
    """One row of the sweep table."""
    knob:                str
    value:               float
    n_reclaimed_clips:   int
    n_calls_recovered:   int
    kb_added:            float
    n_cricket_leaks_added: int
    calls_per_kb:        float


def simulate_setting(rejected_clips: List[ClipRecord],
                     det_map: Dict[str, List[Tuple[float, float]]],
                     tune: GateTunables,
                     *, sample_rate: int = 384_000,
                     ) -> Tuple[int, int, float, int]:
    """For one candidate setting, count (reclaimed_clips, calls_recovered,
    kb_added, cricket_leaks_added).

    "Cricket leak added" = reclaimed clip whose window overlaps NO
    BatDetect2 detection in its source WAV — a clip we'd emit that
    the customer's BatDetect2 pass would process for nothing.
    """
    n_clips = 0
    n_calls = 0
    bytes_added = 0
    n_leaks = 0
    for c in rejected_clips:
        if not _clip_would_pass_or(c.events, tune):
            continue
        n_clips += 1
        bytes_added += clip_bytes(c, sample_rate=sample_rate)
        windows = det_map.get(c.source_basename, [])
        overlapping = count_overlapping_calls(c, windows)
        if overlapping > 0:
            n_calls += overlapping
        else:
            n_leaks += 1
    kb_added = bytes_added / 1024.0
    return n_clips, n_calls, kb_added, n_leaks


def _percall_setting(n_calls: int, kb_added: float) -> float:
    if kb_added <= 0:
        return float("nan")
    return n_calls / kb_added


BANDWIDTH_LOOSENING_KHZ = (0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3)
"""Candidate ``min_bandwidth_khz`` values, shipping-first then downward.
Below ~0.3 kHz the gate is effectively off — cricket leak explodes and
the trade becomes obvious. This range brackets the useful territory."""

DRIFT_LOOSENING_KHZ = (8.0, 7.0, 6.0, 5.0, 4.0)
"""Candidate ``sweep_drift_khz`` (Path B). Loosening = lower value (an
event needs less drift to qualify)."""

PATH_LOOSENING = (1.6, 1.8, 2.0, 2.5, 3.0)
"""Candidate ``sweep_path_ratio_max``. Loosening = higher value (more
"wobbly" sweeps admitted)."""

MONO_LOOSENING = (0.70, 0.60, 0.50, 0.40)
"""Candidate ``sweep_mono_frac_min``. Loosening = lower value."""


def run_sweep(rejected_clips: List[ClipRecord],
              det_map: Dict[str, List[Tuple[float, float]]],
              *, sample_rate: int = 384_000) -> pd.DataFrame:
    """Sweep each knob independently around its shipping default with
    the other three pinned. Yields a table with one row per (knob, value).

    Each row carries both the raw sim numbers and NET-vs-baseline
    columns. The baseline row is not "zero reclaims" as one might
    expect — it captures the "veto/provisional overhead": clips whose
    events pass the OR clause but were rejected on-device by the
    temporal rep-guard or the 6-frame provisional decision. Those
    clips cannot be recovered by loosening the OR knobs; the ``_net``
    columns strip them out so the loosening effect is honest.
    """
    rows: List[dict] = []

    baseline_n_clips = 0
    baseline_n_calls = 0
    baseline_kb      = 0.0
    baseline_leaks   = 0

    def _append(knob: str, value: float, tune: GateTunables) -> None:
        nonlocal baseline_n_clips, baseline_n_calls, baseline_kb, baseline_leaks
        n_clips, n_calls, kb_added, n_leaks = simulate_setting(
            rejected_clips, det_map, tune, sample_rate=sample_rate)
        is_baseline = (knob == "baseline")
        if is_baseline:
            baseline_n_clips = n_clips
            baseline_n_calls = n_calls
            baseline_kb      = kb_added
            baseline_leaks   = n_leaks
        net_clips = n_clips - baseline_n_clips
        net_calls = n_calls - baseline_n_calls
        net_kb    = kb_added - baseline_kb
        net_leaks = n_leaks - baseline_leaks
        rows.append({
            "knob": knob,
            "value": value,
            "n_reclaimed_clips_raw": n_clips,
            "n_calls_recovered_raw": n_calls,
            "kb_added_raw":          kb_added,
            "n_cricket_leaks_raw":   n_leaks,
            "n_reclaimed_clips_net": net_clips,
            "n_calls_recovered_net": net_calls,
            "kb_added_net":          net_kb,
            "n_cricket_leaks_net":   net_leaks,
            "calls_per_kb_net":      _percall_setting(net_calls, net_kb),
        })

    # 0. Shipping baseline. Non-zero counts here are the
    # veto/provisional overhead — see docstring above.
    _append("baseline", 0.0, GateTunables())

    # 1. Sweep each knob individually.
    for v in BANDWIDTH_LOOSENING_KHZ:
        _append("min_bandwidth_khz", v, GateTunables(min_bandwidth_khz=v))
    for v in DRIFT_LOOSENING_KHZ:
        _append("sweep_drift_khz", v, GateTunables(sweep_drift_khz=v))
    for v in PATH_LOOSENING:
        _append("sweep_path_ratio_max", v, GateTunables(sweep_path_ratio_max=v))
    for v in MONO_LOOSENING:
        _append("sweep_mono_frac_min", v, GateTunables(sweep_mono_frac_min=v))

    return pd.DataFrame(rows)


# --- rendering --------------------------------------------------------------

def render_md(config_label: str, sweep_df: pd.DataFrame,
              *, current_kept_clips: int, current_kept_kb: float,
              recommended: Optional[dict] = None) -> str:
    """Emit the gate_loosening_percall.md body for one config."""
    from .score import HEADLINE_CAVEATS
    baseline_row = sweep_df.iloc[0]
    lines: List[str] = []
    lines.append(f"# Gate loosening (per-call, energy-aware) — {config_label}")
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(
        "**Correction to the previous auditor report.** The old "
        "``min_bandwidth_khz`` tightening table used a single-knob "
        "gate model. The real gate at "
        "``BandEnergyDetector.cpp:348-352`` is a **4-feature OR** "
        "(bandwidth OR drift-clause), so a nudge to "
        "``min_bandwidth_khz`` alone does not reject clips that "
        "pass via the drift/path/mono branch. This table uses the "
        "real OR gate.")
    lines.append("")
    lines.append(
        "**Two simulation gaps.** The temporal rep-guard veto "
        "(``BandEnergyDetector.cpp:411-425``) and the 6-frame "
        "provisional gate (line 500) are **not simulated** — the "
        "rep-guard's rate/CV state and the provisional decision are "
        "not persisted in current sidecars. Empirically, the sim's "
        f"**baseline row** below reveals ~"
        f"{int(baseline_row['n_reclaimed_clips_raw'])} clips (carrying "
        f"{int(baseline_row['n_calls_recovered_raw'])} calls) that "
        "already pass the OR clause at shipping tunables. Those clips "
        "are gate-rejected on device by the veto or the provisional "
        "decision, and **cannot be recovered by loosening the OR "
        "knobs**. The ``_net`` columns strip them out so the "
        "loosening effect is honest.")
    lines.append("")
    lines.append(
        "**How to attack the veto/provisional pool.** Add "
        "``rep_rate_hz`` / ``rep_cv`` / ``rep_n_onsets`` plus a "
        "``provisional_rejected`` flag to the sidecar; re-run replay "
        "(~45 min at Release + single-subprocess). That would let a "
        "future analysis separate OR-fail from veto-fail from "
        "provisional-fail. Not in scope for this task.")
    lines.append("")
    lines.append(
        f"**Reference — current shipping.** {current_kept_clips} "
        f"kept clips; {current_kept_kb:.1f} KB of audio emitted to "
        "the Pi (baseline output volume).")
    lines.append("")
    lines.append("## Per-knob sweep")
    lines.append("")
    lines.append(
        "``_raw`` columns are the sim's absolute counts (include "
        "veto-overhead reclaims); ``_net`` columns subtract the "
        "baseline overhead so the row measures the true OR-loosening "
        "effect. Selection metric: **``calls_per_kb_net``** — "
        "recovered calls per KB extra audio the customer's Pi runs "
        "BatDetect2 on. Higher is better; ties resolved toward more "
        "calls recovered.")
    lines.append("")
    lines.append(
        "| knob | value | raw reclaim | raw calls | raw KB | raw leaks "
        "| net reclaim | net calls | net KB | net leaks | net calls/KB |")
    lines.append(
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for _, r in sweep_df.iterrows():
        cpk = r["calls_per_kb_net"]
        cpk_s = "n/a" if pd.isna(cpk) else f"{cpk:.4f}"
        lines.append(
            f"| {r['knob']} | {r['value']:.2f} "
            f"| {int(r['n_reclaimed_clips_raw'])} "
            f"| {int(r['n_calls_recovered_raw'])} "
            f"| {r['kb_added_raw']:.0f} "
            f"| {int(r['n_cricket_leaks_raw'])} "
            f"| {int(r['n_reclaimed_clips_net'])} "
            f"| {int(r['n_calls_recovered_net'])} "
            f"| {r['kb_added_net']:.0f} "
            f"| {int(r['n_cricket_leaks_net'])} "
            f"| {cpk_s} |")
    lines.append("")
    if recommended is not None:
        lines.append("## Recommendations (NOT applied)")
        lines.append("")
        lines.append(
            "Two picks, per the plan's 'recommend, do not apply' "
            "contract. Auditor picks which fits the customer's "
            "energy budget.")
        lines.append("")
        max_eff = recommended.get("max_efficiency")
        max_rec = recommended.get("max_recovery")
        if max_eff is not None:
            lines.append(
                f"### Efficient good-enough — "
                f"``{max_eff['knob']} = {max_eff['value']:.2f}``")
            lines.append(
                f"- highest ``calls_per_kb_net`` among material rows "
                f"(net calls ≥ {MIN_MATERIAL_CALLS}).")
            lines.append(
                f"- **{int(max_eff['n_calls_recovered_net'])} calls "
                f"recovered net** at "
                f"{max_eff['kb_added_net']:.0f} KB extra audio to "
                f"the Pi.")
            lines.append(
                f"- Efficiency: "
                f"**{max_eff['calls_per_kb_net']:.4f} calls / KB**. "
                f"Cricket leaks added: "
                f"{int(max_eff['n_cricket_leaks_net'])}.")
        else:
            lines.append(
                "### Efficient good-enough — **NONE.** No knob "
                f"loosening recovers ≥{MIN_MATERIAL_CALLS} net calls "
                "on this corpus.")
        lines.append("")
        if max_rec is not None:
            lines.append(
                f"### Max recovery — "
                f"``{max_rec['knob']} = {max_rec['value']:.2f}``")
            lines.append(
                f"- most ``n_calls_recovered_net`` in the sweep, "
                "regardless of efficiency.")
            lines.append(
                f"- **{int(max_rec['n_calls_recovered_net'])} calls "
                f"recovered net** at "
                f"{max_rec['kb_added_net']:.0f} KB extra audio "
                "(roughly "
                f"+{100.0 * max_rec['kb_added_net'] / current_kept_kb:.1f}% "
                "output volume vs shipping baseline).")
            lines.append(
                f"- Efficiency: "
                f"**{max_rec['calls_per_kb_net']:.4f} calls / KB**. "
                f"Cricket leaks added: "
                f"{int(max_rec['n_cricket_leaks_net'])}.")
        lines.append("")
        lines.append(
            "Do NOT apply. The device is ARM, this simulation is x86 "
            "with fast-math — re-check on device via "
            "``tools/collection/verify_feature_parity.py`` before any "
            "shipping change. And the temporal rep-guard + "
            "provisional gate are not simulated (see above), so the "
            "recovered-calls numbers are an upper bound; real device "
            "recovery will be somewhat lower.")
    return "\n".join(lines)


MIN_MATERIAL_CALLS = 100
"""Minimum net calls a candidate must recover before it's considered
"material". Below this threshold the calls-per-KB metric is dominated
by sampling noise (a knob nudge that flips 3 clips can look "efficient"
without being useful). 100 calls ~= 3% of the veto/provisional
overhead pool, chosen so any candidate we surface actually moves the
per-call number the plan cares about."""


def pick_recommended(sweep_df: pd.DataFrame) -> Dict[str, Optional[dict]]:
    """Return TWO recommendations, per the plan's "recommend, do not
    apply" contract:

    - ``max_efficiency`` — highest ``calls_per_kb_net`` among
      **material** rows (net calls ≥ ``MIN_MATERIAL_CALLS``). This is
      the "efficient good-enough" pick.
    - ``max_recovery`` — most ``n_calls_recovered_net``, regardless of
      efficiency. This is the "keep more bat calls" pick, the plan's
      lean-toward-recovery direction.

    Returning both lets the auditor see the trade explicitly. Tiny
    high-efficiency picks (a knob nudge that flips 3 clips) are
    excluded from ``max_efficiency`` by the material threshold — they
    are noise, not recommendations.
    """
    non_baseline = sweep_df[
        (sweep_df["knob"] != "baseline")
        & (sweep_df["n_reclaimed_clips_net"] > 0)
    ].copy()
    if non_baseline.empty:
        return {"max_efficiency": None, "max_recovery": None}

    material = non_baseline[
        non_baseline["n_calls_recovered_net"] >= MIN_MATERIAL_CALLS
    ]
    max_eff = None
    if not material.empty:
        max_eff = material.sort_values(
            ["calls_per_kb_net", "n_calls_recovered_net"],
            ascending=[False, False]).iloc[0].to_dict()

    max_rec = non_baseline.sort_values(
        ["n_calls_recovered_net", "calls_per_kb_net"],
        ascending=[False, False]).iloc[0].to_dict()
    return {"max_efficiency": max_eff, "max_recovery": max_rec}
