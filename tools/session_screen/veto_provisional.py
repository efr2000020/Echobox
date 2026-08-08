# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Analyse the veto + provisional rejection pool, per-call, energy-aware.

Reads sidecars written by ``echobox-replay`` at ``format_version >= 2``
(shipping post-instrumentation build). For every rejected clip's events:

  - **or_fail**            — event failed the 4-feature OR clause. Not
                             recoverable by veto/provisional tuning.
  - **veto_flipped**       — event PASSED the OR clause but the temporal
                             rep-guard flipped it to reject. Recoverable
                             by loosening the veto knobs.
  - **provisional_reject** — event was rejected by the 6-frame
                             provisional gate before full data arrived.
                             Recoverable by disabling / weakening the
                             provisional gate.

At the CLIP level, a clip is "kept" if any of its events was kept. So
the recoverable-clip population is: **rejected clips whose ALL events
failed OR, except that at least one event was flipped by veto or
provisional**. Loosening the veto or provisional lets that event pass
→ clip flips to kept.

Per-knob sweep uses the **exact stored inputs** — no math approximation
— for the six veto knobs (rep_cv_min/max, rep_rate_min/max_hz,
rep_min_events, rep_broadband_keep_khz). The provisional gate is
reported as a single on/off point because provisional-time features
aren't in the sidecar (only close-time features are); a fuller sweep
of ``GATE_DECISION_FRAMES`` would require logging the provisional
snapshot too.

Every threshold surfaced as a recommendation must be **re-checked on
ARM** via ``tools/collection/verify_feature_parity.py`` before shipping.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Tuple

import pandas as pd


# --- shipping tunables (mirrors the sidecar's detector.tunables block) ------

@dataclass
class VetoTunables:
    """The knobs the temporal rep-guard reads. Defaults = shipping."""
    rep_cv_min:             float = 0.5
    rep_cv_max:             float = 1.3
    rep_rate_min_hz:        float = 1.0
    rep_rate_max_hz:        float = 20.0
    rep_min_events:         int   = 2
    rep_broadband_keep_khz: float = 10.0


@dataclass
class GateTunables:
    """The OR-clause knobs (kept for the clearBat re-check)."""
    min_bandwidth_khz:    float = 0.9
    sweep_drift_khz:      float = 8.0
    sweep_path_ratio_max: float = 1.6
    sweep_mono_frac_min:  float = 0.7


def _or_clause(bw_khz: float, drift_khz: float, path_ratio: float,
               mono_frac: float, g: GateTunables) -> bool:
    """4-feature OR clause. Mirrors BandEnergyDetector.cpp:348-352 verbatim."""
    return ((bw_khz >= g.min_bandwidth_khz)
            or (drift_khz    >= g.sweep_drift_khz
                and path_ratio    <= g.sweep_path_ratio_max
                and mono_frac     >= g.sweep_mono_frac_min))


def _metronomic(rep_rate_hz: float, rep_cv: float, rep_n_onsets: int,
                v: VetoTunables) -> bool:
    """Rep-guard metronomic test. Mirrors BandEnergyDetector.cpp:391-395."""
    return (rep_n_onsets >= v.rep_min_events
            and rep_rate_hz >= v.rep_rate_min_hz
            and rep_rate_hz <= v.rep_rate_max_hz
            and rep_cv      >= v.rep_cv_min
            and rep_cv      <= v.rep_cv_max)


def _clear_bat(bw_khz: float, drift_khz: float, path_ratio: float,
               mono_frac: float, v: VetoTunables, g: GateTunables) -> bool:
    """clearBat bypass. Mirrors BandEnergyDetector.cpp:406-410."""
    return ((bw_khz >= v.rep_broadband_keep_khz)
            or (drift_khz    >= g.sweep_drift_khz
                and path_ratio    <= g.sweep_path_ratio_max
                and mono_frac     >= g.sweep_mono_frac_min))


def event_would_pass(e: dict, g: GateTunables, v: VetoTunables,
                     *, disable_provisional: bool = False) -> bool:
    """Would this event pass the full gate (OR + veto), under swept tunables?

    Semantics for provisional-rejected events:
      - If ``disable_provisional=False``, a provisional-rejected event
        is treated as rejected regardless of its features. That's the
        actual on-device behaviour.
      - If ``disable_provisional=True``, we simulate "provisional gate
        off": evaluate the full close-time check on the sidecar's
        (close-time) features and let the event pass iff both OR
        clause and veto agree.

    All other events run the OR + veto check straight from stored
    features + rep stats.
    """
    prov = bool(e.get("provisional_rejected", False))
    if prov and not disable_provisional:
        return False

    bw    = float(e.get("bandwidth_khz",  0.0))
    drift = float(e.get("drift_khz",      0.0))
    path  = float(e.get("path_ratio",     0.0))
    mono  = float(e.get("mono_fraction",  0.0))

    if not _or_clause(bw, drift, path, mono, g):
        return False

    # OR passed → check the temporal veto.
    r_hz  = float(e.get("rep_rate_hz",    0.0))
    r_cv  = float(e.get("rep_cv",         0.0))
    r_n   = int(e.get("rep_n_onsets",     0))
    metro = _metronomic(r_hz, r_cv, r_n, v)
    clear = _clear_bat(bw, drift, path, mono, v, g)
    return not (metro and not clear)


# --- clip loader ------------------------------------------------------------

@dataclass
class ClipRec:
    """Per-clip event bundle for the sweep."""
    source_basename:  str
    clip_wav:         str
    clip_start_ms:    float
    clip_end_ms:      float
    clip_duration_ms: float
    kept:             bool
    events:           List[dict] = field(default_factory=list)


def load_clips(replay_root: Path, replay_df: pd.DataFrame,
               *, only_rejected: bool = True) -> List[ClipRec]:
    """Read every clip's sidecar events off disk.

    Set ``only_rejected=False`` to also load kept clips (needed for the
    "veto-hurts-real-bats" leakage estimate). Missing sidecars are
    skipped silently — a v1 sidecar returns empty events under the new
    reader path so the clip contributes nothing to the sweep.
    """
    df = replay_df.copy()
    df = df[~df["no_clips"].fillna(False).astype(bool)]
    if only_rejected:
        df = df[~df["kept"].astype(bool)]
    out: List[ClipRec] = []
    for _, r in df.iterrows():
        rel = str(r["clip_wav"] or "")
        if not rel:
            continue
        sidecar = replay_root / rel.replace(".wav", ".json")
        if not sidecar.exists():
            continue
        try:
            doc = json.loads(sidecar.read_text())
        except Exception:
            continue
        out.append(ClipRec(
            source_basename=Path(str(r["source_file"])).name,
            clip_wav=rel,
            clip_start_ms=float(r["clip_start_ms"]),
            clip_end_ms=float(r["clip_end_ms"]),
            clip_duration_ms=float(r["clip_duration_ms"]),
            kept=bool(r["kept"]),
            events=list(doc.get("events") or []),
        ))
    return out


# --- bat-overlap join --------------------------------------------------------

def detections_by_basename(truth: pd.DataFrame
                           ) -> Dict[str, List[Tuple[float, float]]]:
    """Same shape as gate_recovery.py: basename → detection windows (ms)."""
    out: Dict[str, List[Tuple[float, float]]] = {}
    for _, r in truth.iterrows():
        if not bool(r["bat_present"]):
            continue
        basename = Path(str(r["file"])).name
        try:
            dets = json.loads(r["detections_json"] or "[]")
        except Exception:
            dets = []
        out[basename] = [(float(d.get("start_time_s", 0.0)) * 1000.0,
                          float(d.get("end_time_s",   0.0)) * 1000.0)
                         for d in dets]
    return out


def _overlap_count(c: ClipRec, windows: List[Tuple[float, float]],
                   slop_ms: float = 20.0) -> int:
    """BatDetect2 detection windows overlapping this clip's window.

    Zero overlap = the clip is a candidate cricket leak if kept.
    """
    lo = c.clip_start_ms - slop_ms
    hi = c.clip_end_ms   + slop_ms
    return sum(1 for d_lo, d_hi in windows if not (d_hi < lo or d_lo > hi))


# --- pool split (Part 2, "vetoed vs provisional vs both") -------------------

@dataclass
class PoolSplit:
    n_clips_provisional_only: int
    n_calls_provisional_only: int
    n_clips_veto_only:        int
    n_calls_veto_only:        int
    n_clips_both:             int
    n_calls_both:             int
    n_clips_or_fail:          int
    n_calls_or_fail:          int


def split_rejected_pool(clips: Iterable[ClipRec],
                        det_map: Dict[str, List[Tuple[float, float]]]
                        ) -> PoolSplit:
    """For every rejected clip, classify by why it was rejected.

    Precedence at the CLIP level: a clip's "best" recoverability comes
    from its "most recoverable" event, in this order:
    ``veto_only > provisional_only > both > or_fail``. That is, a clip
    with even one veto-flipped-only event is called "veto_only".
    """
    counts = dict(prov_only=0, veto_only=0, both=0, or_fail=0)
    calls  = dict(prov_only=0, veto_only=0, both=0, or_fail=0)
    for c in clips:
        clip_bucket = "or_fail"
        for e in c.events:
            veto = bool(e.get("veto_applied", False))
            prov = bool(e.get("provisional_rejected", False))
            if veto and not prov:
                clip_bucket = "veto_only"; break
            if veto and prov:
                if clip_bucket in ("or_fail", "prov_only"): clip_bucket = "both"
            elif prov and not veto:
                if clip_bucket == "or_fail": clip_bucket = "prov_only"
        counts[clip_bucket] += 1
        n_calls = _overlap_count(c, det_map.get(c.source_basename, []))
        calls[clip_bucket] += n_calls
    return PoolSplit(
        n_clips_provisional_only=counts["prov_only"],
        n_calls_provisional_only=calls["prov_only"],
        n_clips_veto_only=counts["veto_only"],
        n_calls_veto_only=calls["veto_only"],
        n_clips_both=counts["both"],
        n_calls_both=calls["both"],
        n_clips_or_fail=counts["or_fail"],
        n_calls_or_fail=calls["or_fail"],
    )


# --- sweep -----------------------------------------------------------------

_CV_LO_NUDGES  = (0.5, 0.6, 0.7, 0.8, 0.9)
_CV_HI_NUDGES  = (1.3, 1.2, 1.1, 1.0, 0.9)
"""Loosening the CV window (narrowing it) makes the veto fire less
often — passes fewer clips as "metronomic". Sweep in from both edges."""

_RATE_LO_NUDGES = (1.0, 2.0, 3.0, 5.0)
_RATE_HI_NUDGES = (20.0, 15.0, 12.0, 10.0)
_MIN_EVENTS_NUDGES = (2, 3, 4, 5, 8)
_BROADBAND_KEEP_NUDGES = (10.0, 8.0, 6.0, 4.0, 2.0)


def _simulate(clips: List[ClipRec],
              det_map: Dict[str, List[Tuple[float, float]]],
              g: GateTunables, v: VetoTunables,
              *, disable_provisional: bool,
              sample_rate: int = 384_000
              ) -> Tuple[int, int, float, int]:
    """Count (reclaimed_clips, calls_recovered, kb_added, leaks_added)."""
    n_clips = 0
    n_calls = 0
    bytes_added = 0
    n_leaks = 0
    for c in clips:
        if any(event_would_pass(e, g, v,
                                disable_provisional=disable_provisional)
               for e in c.events):
            n_clips += 1
            bytes_added += int(c.clip_duration_ms * sample_rate / 1000.0) * 2
            n_over = _overlap_count(c, det_map.get(c.source_basename, []))
            if n_over > 0:
                n_calls += n_over
            else:
                n_leaks += 1
    return n_clips, n_calls, bytes_added / 1024.0, n_leaks


def run_sweep(clips: List[ClipRec],
              det_map: Dict[str, List[Tuple[float, float]]]
              ) -> pd.DataFrame:
    """Sweep every veto knob independently + the provisional on/off point.

    Baseline row = shipping tunables, ``provisional_rejected`` respected.
    ``_net`` columns subtract baseline to strip whatever "always
    reclaimable" residual exists (should be near zero here; if not,
    the baseline row itself is worth investigating).
    """
    rows: List[dict] = []
    g0 = GateTunables()
    v0 = VetoTunables()

    b_clips = b_calls = b_leaks = 0
    b_kb    = 0.0

    def _row(knob: str, value: float, v: VetoTunables,
             *, disable_provisional: bool = False) -> None:
        nonlocal b_clips, b_calls, b_kb, b_leaks
        n_clips, n_calls, kb, n_leaks = _simulate(
            clips, det_map, g0, v,
            disable_provisional=disable_provisional)
        if knob == "baseline":
            b_clips, b_calls, b_kb, b_leaks = n_clips, n_calls, kb, n_leaks
        net_c    = n_clips - b_clips
        net_k    = n_calls - b_calls
        net_kb   = kb - b_kb
        net_lk   = n_leaks - b_leaks
        cpk = (net_k / net_kb) if net_kb > 0 else float("nan")
        rows.append({
            "knob": knob, "value": value,
            "n_reclaimed_clips_raw": n_clips,
            "n_calls_recovered_raw": n_calls,
            "kb_added_raw": kb,
            "n_cricket_leaks_raw": n_leaks,
            "n_reclaimed_clips_net": net_c,
            "n_calls_recovered_net": net_k,
            "kb_added_net": net_kb,
            "n_cricket_leaks_net": net_lk,
            "calls_per_kb_net": cpk,
        })

    _row("baseline", 0.0, v0)

    for cv in _CV_LO_NUDGES:
        _row("rep_cv_min", cv,
             VetoTunables(rep_cv_min=cv))
    for cv in _CV_HI_NUDGES:
        _row("rep_cv_max", cv,
             VetoTunables(rep_cv_max=cv))
    for r in _RATE_LO_NUDGES:
        _row("rep_rate_min_hz", r,
             VetoTunables(rep_rate_min_hz=r))
    for r in _RATE_HI_NUDGES:
        _row("rep_rate_max_hz", r,
             VetoTunables(rep_rate_max_hz=r))
    for n in _MIN_EVENTS_NUDGES:
        _row("rep_min_events", float(n),
             VetoTunables(rep_min_events=int(n)))
    for b in _BROADBAND_KEEP_NUDGES:
        _row("rep_broadband_keep_khz", b,
             VetoTunables(rep_broadband_keep_khz=b))

    # Veto entirely OFF — all veto knobs matched so metronomic can never fire.
    _row("veto_off", 0.0, VetoTunables(rep_min_events=99999))
    # Provisional gate OFF — treat provisional_rejected events as if
    # they'd never been provisionally checked (evaluate full close-time
    # check on close-time features instead).
    _row("provisional_off", 1.0, v0, disable_provisional=True)
    # BOTH off — upper bound of recoverable.
    _row("veto+provisional_off", 1.0,
         VetoTunables(rep_min_events=99999),
         disable_provisional=True)
    return pd.DataFrame(rows)


# --- selection --------------------------------------------------------------

MIN_MATERIAL_CALLS = 100
"""Same threshold as gate_recovery.py — filters out noise picks."""


def pick_recommended(sweep_df: pd.DataFrame) -> Dict[str, Optional[dict]]:
    non_baseline = sweep_df[
        (sweep_df["knob"] != "baseline")
        & (sweep_df["n_reclaimed_clips_net"] > 0)
    ].copy()
    if non_baseline.empty:
        return {"max_efficiency": None, "max_recovery": None}
    material = non_baseline[
        non_baseline["n_calls_recovered_net"] >= MIN_MATERIAL_CALLS
    ]
    eff = None
    if not material.empty:
        eff = material.sort_values(
            ["calls_per_kb_net", "n_calls_recovered_net"],
            ascending=[False, False]).iloc[0].to_dict()
    rec = non_baseline.sort_values(
        ["n_calls_recovered_net", "calls_per_kb_net"],
        ascending=[False, False]).iloc[0].to_dict()
    return {"max_efficiency": eff, "max_recovery": rec}


# --- rendering --------------------------------------------------------------

def render_md(config_label: str, pool: PoolSplit, sweep_df: pd.DataFrame,
              picks: Dict[str, Optional[dict]],
              *, current_kept_kb: float) -> str:
    from .score import HEADLINE_CAVEATS
    lines: List[str] = []
    lines.append(f"# Veto + provisional recovery — {config_label}")
    lines.append("")
    lines.append("> " + HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(
        "**Additional caveat.** Any recommended threshold below is "
        "x86-derived; re-check on ARM via "
        "``tools/collection/verify_feature_parity.py`` before "
        "shipping. Filter behaviour has been left unchanged for this "
        "measurement round — only observability was added.")
    lines.append("")
    lines.append("## Pool split (rejected clips)")
    lines.append("")
    lines.append(
        "Each rejected clip is labelled by why it was thrown away. "
        "Bucket precedence (per clip): a single ``veto_only`` event "
        "makes the clip ``veto_only``; a single "
        "``provisional_only`` event (no veto flips) makes it "
        "``provisional_only``; ``both`` means separate events hit "
        "veto and provisional; ``or_fail`` means every event failed "
        "the OR clause outright.")
    lines.append("")
    lines.append("| bucket | clips | BatDetect2 calls in overlap |")
    lines.append("|---|---:|---:|")
    lines.append(
        f"| veto_only        | {pool.n_clips_veto_only:>5}   "
        f"| {pool.n_calls_veto_only:>5} |")
    lines.append(
        f"| provisional_only | {pool.n_clips_provisional_only:>5}   "
        f"| {pool.n_calls_provisional_only:>5} |")
    lines.append(
        f"| both             | {pool.n_clips_both:>5}   "
        f"| {pool.n_calls_both:>5} |")
    lines.append(
        f"| or_fail          | {pool.n_clips_or_fail:>5}   "
        f"| {pool.n_calls_or_fail:>5} |")
    lines.append("")
    lines.append(
        f"**Recoverable pool** = veto_only ∪ provisional_only ∪ both = "
        f"{pool.n_clips_veto_only + pool.n_clips_provisional_only + pool.n_clips_both} "
        f"clips carrying "
        f"{pool.n_calls_veto_only + pool.n_calls_provisional_only + pool.n_calls_both} "
        "BatDetect2 calls (upper bound on what veto/provisional "
        "tuning can add back). ``or_fail`` clips are outside the "
        "reach of this task.")
    lines.append("")
    lines.append("## Per-knob sweep (net vs baseline)")
    lines.append("")
    lines.append(
        "``_net`` columns subtract the baseline row (shipping) to "
        "isolate the loosening effect. ``calls_per_kb_net`` is the "
        "energy-efficiency metric — recovered bat calls per KB extra "
        "audio for the customer's Pi to run BatDetect2 on.")
    lines.append("")
    lines.append(
        "| knob | value | net reclaim | net calls | net KB | net leaks "
        "| net calls/KB |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|")
    for _, r in sweep_df.iterrows():
        cpk = r["calls_per_kb_net"]
        cpk_s = "n/a" if pd.isna(cpk) else f"{cpk:.4f}"
        lines.append(
            f"| {r['knob']} | {r['value']:.2f} "
            f"| {int(r['n_reclaimed_clips_net'])} "
            f"| {int(r['n_calls_recovered_net'])} "
            f"| {r['kb_added_net']:.0f} "
            f"| {int(r['n_cricket_leaks_net'])} "
            f"| {cpk_s} |")
    lines.append("")
    lines.append("## Recommendations (NOT applied; ship next round)")
    lines.append("")
    lines.append(
        "Two picks per the plan's leaning-keep-calls tiebreak. "
        f"Baseline output volume for reference: {current_kept_kb:.0f} KB.")
    lines.append("")
    eff = picks.get("max_efficiency")
    rec = picks.get("max_recovery")
    if eff is not None:
        lines.append(
            f"**Efficient good-enough — ``{eff['knob']} = "
            f"{eff['value']:.2f}``**")
        lines.append(
            f"- +{int(eff['n_calls_recovered_net'])} bat calls at "
            f"+{eff['kb_added_net']:.0f} KB "
            f"(+{100*eff['kb_added_net']/current_kept_kb:.1f}% output "
            "volume vs shipping).")
        lines.append(
            f"- Efficiency: {eff['calls_per_kb_net']:.4f} calls / KB; "
            f"cricket leaks added: {int(eff['n_cricket_leaks_net'])}.")
        lines.append("")
    else:
        lines.append(
            "**Efficient good-enough — NONE** (no knob nudge recovers "
            f"≥{MIN_MATERIAL_CALLS} net calls).")
        lines.append("")
    if rec is not None:
        lines.append(
            f"**Max recovery — ``{rec['knob']} = {rec['value']:.2f}``**")
        lines.append(
            f"- +{int(rec['n_calls_recovered_net'])} bat calls at "
            f"+{rec['kb_added_net']:.0f} KB "
            f"(+{100*rec['kb_added_net']/current_kept_kb:.1f}% output "
            "volume vs shipping).")
        lines.append(
            f"- Efficiency: {rec['calls_per_kb_net']:.4f} calls / KB; "
            f"cricket leaks added: {int(rec['n_cricket_leaks_net'])}.")
        lines.append("")
    lines.append(
        "**Ship note.** Whichever pick is chosen, re-check on ARM "
        "with ``tools/collection/verify_feature_parity.py`` before "
        "landing the tunable change. Fast-math + O3 can shift "
        "``rep_cv`` and ``bandwidth_khz`` at the third decimal, "
        "which matters at gate boundaries.")
    return "\n".join(lines)


# --- top-level driver -------------------------------------------------------

def characterise(replay_root: Path,
                 truth_df: pd.DataFrame,
                 replay_df: pd.DataFrame,
                 output_dir: Path,
                 *, config_label: str = "") -> Dict[str, object]:
    """Read v2 sidecars under ``replay_root``, split the rejected pool,
    sweep the veto knobs + provisional on/off, emit CSV + MD.

    Returns the picks dict (efficient / max_recovery) so a caller can
    stamp them into a combined summary if desired.
    """
    from .score import assert_basenames_unique
    assert_basenames_unique(truth_df,  column="file",        source="truth")
    assert_basenames_unique(replay_df, column="source_file", source="replay")

    rejected_clips = load_clips(replay_root, replay_df, only_rejected=True)
    det_map = detections_by_basename(truth_df)

    pool = split_rejected_pool(rejected_clips, det_map)
    sweep_df = run_sweep(rejected_clips, det_map)
    picks = pick_recommended(sweep_df)

    # Reference: current kept output volume, for the "% of baseline"
    # context in the recommendation lines.
    kept = replay_df[replay_df["kept"].astype(bool)]
    current_kept_kb = float(kept["clip_duration_ms"].sum() * 384000 / 1000 * 2 / 1024)

    output_dir.mkdir(parents=True, exist_ok=True)
    sweep_df.to_csv(output_dir / "veto_provisional_recovery.csv", index=False)
    md = render_md(config_label, pool, sweep_df, picks,
                   current_kept_kb=current_kept_kb)
    (output_dir / "veto_provisional_recovery.md").write_text(md)
    return {"pool": pool, "sweep": sweep_df, "picks": picks,
            "current_kept_kb": current_kept_kb}
