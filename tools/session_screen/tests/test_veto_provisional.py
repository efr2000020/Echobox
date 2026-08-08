# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for the veto+provisional recovery analysis.

Pin the classification logic (event_would_pass) against the real C++
gate semantics (BandEnergyDetector.cpp:348-425), and the pool-split /
sweep math for the auditor's recommendation table.
"""
from __future__ import annotations

import pandas as pd
import pytest

from tools.session_screen import veto_provisional as vp


def _event(**overrides) -> dict:
    """Minimal event dict with sensible defaults."""
    base = {
        "bandwidth_khz":       1.5,
        "drift_khz":           0.0,
        "path_ratio":          0.0,
        "mono_fraction":       1.0,
        "gate_rejected":       False,
        "sweep_bat_like":      True,
        "veto_applied":        False,
        "provisional_rejected": False,
        "rep_rate_hz":         0.0,
        "rep_cv":              0.0,
        "rep_n_onsets":        0,
    }
    base.update(overrides)
    return base


# --- OR clause + veto reproductions -----------------------------------------

def test_or_clause_bandwidth_path_alone_passes() -> None:
    """Path A: bandwidth >= min_bandwidth_khz. Drift-clause fails."""
    g = vp.GateTunables()
    e = _event(bandwidth_khz=1.5, drift_khz=0.0, path_ratio=99, mono_fraction=0.1)
    assert vp.event_would_pass(e, g, vp.VetoTunables())


def test_or_clause_drift_path_alone_passes() -> None:
    """Path B: bandwidth fails, drift+path+mono all pass."""
    g = vp.GateTunables()
    e = _event(bandwidth_khz=0.2, drift_khz=10.0, path_ratio=1.0, mono_fraction=0.9)
    assert vp.event_would_pass(e, g, vp.VetoTunables())


def test_temporal_veto_flips_or_passing_event_to_reject() -> None:
    """OR passes, but rep-stats are metronomic AND no clearBat bypass."""
    g = vp.GateTunables()
    v = vp.VetoTunables()
    # OR passes via Path A (bw=1.5), NOT clearBat (bw < 10, drift-clause fails).
    # rep_rate_hz=10 inside [1,20], rep_cv=0.9 inside [0.5,1.3], onsets=3 >= 2.
    e = _event(bandwidth_khz=1.5, drift_khz=0.0,
               path_ratio=99, mono_fraction=0.1,
               rep_rate_hz=10.0, rep_cv=0.9, rep_n_onsets=3)
    assert not vp.event_would_pass(e, g, v)


def test_clearbat_bypass_protects_wide_events_from_veto() -> None:
    """A wide-band event (bw >= rep_broadband_keep_khz) bypasses veto."""
    g = vp.GateTunables()
    v = vp.VetoTunables()   # rep_broadband_keep_khz=10 default
    e = _event(bandwidth_khz=12.0, drift_khz=0.0,
               path_ratio=99, mono_fraction=0.1,
               rep_rate_hz=10.0, rep_cv=0.9, rep_n_onsets=3)
    assert vp.event_would_pass(e, g, v), \
        "clearBat (bw>=10) should protect this event from the veto"


def test_provisional_rejected_event_stays_rejected_unless_disabled() -> None:
    g = vp.GateTunables()
    v = vp.VetoTunables()
    e = _event(provisional_rejected=True,
               bandwidth_khz=1.5)   # would OR-pass at close
    assert not vp.event_would_pass(e, g, v)
    assert vp.event_would_pass(e, g, v, disable_provisional=True)


def test_loosening_rep_cv_min_recovers_a_flipped_event() -> None:
    """Tighten the CV window so the event no longer looks metronomic."""
    g = vp.GateTunables()
    # This event's cv=0.4; default cv_min=0.5 → 0.4 < 0.5 → not metronomic
    # → event was NEVER veto-flipped in reality. Bad fixture.
    # Instead: pin cv INSIDE the default window (0.5-1.3), then narrow it
    # in the loosening sim.
    e = _event(bandwidth_khz=1.5, drift_khz=0.0,
               path_ratio=99, mono_fraction=0.1,
               rep_rate_hz=10.0, rep_cv=0.9, rep_n_onsets=3)
    tight = vp.VetoTunables()                # cv=0.9 inside [0.5,1.3] → vetoed
    loose = vp.VetoTunables(rep_cv_max=0.8)  # cv=0.9 outside [0.5,0.8] → not vetoed
    assert not vp.event_would_pass(e, g, tight)
    assert vp.event_would_pass(e, g, loose)


# --- pool split -------------------------------------------------------------

def _clip(events, source="a.wav", start_ms=0, end_ms=200, kept=False):
    return vp.ClipRec(
        source_basename=source, clip_wav=f"{source}.clip.wav",
        clip_start_ms=start_ms, clip_end_ms=end_ms, clip_duration_ms=end_ms - start_ms,
        kept=kept, events=events)


def test_pool_split_veto_only_when_any_event_is_veto_flipped() -> None:
    """One veto-only event in a clip labels the whole clip veto_only,
    regardless of the other events' rejection paths."""
    c1 = _clip([_event(veto_applied=True),
                _event(veto_applied=False, provisional_rejected=False)])
    c2 = _clip([_event(provisional_rejected=True)], source="b.wav")
    c3 = _clip([_event()], source="c.wav")  # or_fail
    det_map = {}
    p = vp.split_rejected_pool([c1, c2, c3], det_map)
    assert p.n_clips_veto_only         == 1
    assert p.n_clips_provisional_only  == 1
    assert p.n_clips_or_fail           == 1


def test_pool_split_labels_both_when_events_split_across_paths() -> None:
    """A veto-flipped event AND a provisional-only event in the same
    clip → labelled ``both`` in our precedence."""
    c = _clip([_event(provisional_rejected=True),
               _event(veto_applied=True, provisional_rejected=True)])
    p = vp.split_rejected_pool([c], {})
    # First event is prov_only → clip_bucket=prov_only; second is veto+prov
    # → both. Final label = "both".
    assert p.n_clips_both == 1


# --- pick_recommended -------------------------------------------------------

def test_pick_recommended_material_floor_filters_tiny_picks() -> None:
    """A knob nudge that recovers 5 calls at high efficiency is noise —
    the efficient pick should skip it and land on a material row."""
    df = pd.DataFrame([
        {"knob": "baseline", "value": 0.0,
         "n_reclaimed_clips_raw": 0, "n_calls_recovered_raw": 0,
         "kb_added_raw": 0, "n_cricket_leaks_raw": 0,
         "n_reclaimed_clips_net": 0, "n_calls_recovered_net": 0,
         "kb_added_net": 0.0, "n_cricket_leaks_net": 0,
         "calls_per_kb_net": None},
        {"knob": "rep_cv_max", "value": 0.8,
         "n_reclaimed_clips_raw": 2, "n_calls_recovered_raw": 5,
         "kb_added_raw": 100, "n_cricket_leaks_raw": 0,
         "n_reclaimed_clips_net": 2, "n_calls_recovered_net": 5,
         "kb_added_net": 100.0, "n_cricket_leaks_net": 0,
         "calls_per_kb_net": 0.05},
        {"knob": "rep_cv_max", "value": 1.0,
         "n_reclaimed_clips_raw": 500, "n_calls_recovered_raw": 800,
         "kb_added_raw": 50000, "n_cricket_leaks_raw": 20,
         "n_reclaimed_clips_net": 500, "n_calls_recovered_net": 800,
         "kb_added_net": 50000.0, "n_cricket_leaks_net": 20,
         "calls_per_kb_net": 0.016},
    ])
    picks = vp.pick_recommended(df)
    assert picks["max_efficiency"]["n_calls_recovered_net"] == 800
    assert picks["max_recovery"]["n_calls_recovered_net"] == 800
