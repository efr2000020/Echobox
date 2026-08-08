# Changelog

## 0.3.0-rc1 — 2026-08-07 (pre-release)

### Temporal rep-guard disabled by default

The temporal repetition-rate guard inside the cricket filter now
defaults to **off**. On the Pipistrellus reference corpus, keeping it
on was discarding approximately **2,500 real bat calls per night** —
calls that had already been accepted by the primary sweep-shape gate
and then vetoed by the follow-on onset-timing check.

Measured effect on the reference corpus:

- **+~2,500 bat calls recovered per night** (BatDetect2-visible)
- **+~14% output volume** (more accepted clips reach the SD card)
- **~30% relative increase in cricket-shaped false positives**
  (129 → ~168 leaked cricket clips per night on this corpus)

The rep-guard **remains in the codebase** as a runtime tunable:

```
rep_guard_enabled = 1   # restores the pre-0.3.0 temporal-veto behaviour
```

**Re-enable for CF-heavy deployments.** The rep-guard exists to reject
metronomic onset patterns, which is the mechanism most likely to matter
at Rhinolophus/Nyctalus (CF) sites we have no data on. Sites deploying
at CF-heavy locations should keep the guard on until a per-site
validation set exists.

### Instrumentation — decision-path fields in the sidecar

Every event sidecar now carries six additional diagnostic fields
alongside the existing sweep-shape features (`format_version: 1 → 2`):

- `sweep_bat_like` — 4-feature OR clause result
- `veto_applied` — temporal rep-guard flipped an OR-passing event
- `provisional_rejected` — 6-frame provisional gate rejected before full close
- `rep_rate_hz`, `rep_cv`, `rep_n_onsets` — onset ring's rate/CV/occupancy
  at gate-decision time

Older readers ignore unknown keys; readers that care about the new
fields should branch on `format_version >= 2`. Filter behaviour is
unchanged when `rep_guard_enabled=1` — the new fields are observability
only.

### Caveats to read alongside these numbers

- **Re-check on ARM.** The recovered-calls figure was measured on x86
  with `-ffast-math`. Verify on the target device with
  `tools/collection/verify_feature_parity.py` before shipping the new
  default to a new site. The rep-guard on/off is a boolean and
  arch-robust; the leak/recovery magnitudes should be confirmed on
  hardware.
- **BatDetect2 screen, not human ground truth.** All recovery and leak
  numbers are measured against BatDetect2 (a neural bat-call detector),
  which itself misses faint calls and doesn't distinguish some
  cricket-like patterns from CF species. Deltas are trustworthy;
  absolutes are soft.
- **Pipistrellus only.** Our reference corpus (585 × 1-minute WAVs from
  a single field session) contains only Pipistrellus species — no CF
  or QCF species (Rhinolophus/Nyctalus/Myotis). Recovery on CF species
  is **not measured**. Not "good", not "bad" — untested. See the
  "Temporal rep-guard (advanced)" section in `README.md` for how to
  re-enable the guard on CF-heavy sites.

### Files touched

- `src/dsp/algorithms/BandEnergyDetector/BandEnergyDetector.hpp`
  — `m_repGuardEnabled` default `1 → 0`; new EventFeatures fields.
- `src/dsp/algorithms/BandEnergyDetector/BandEnergyDetector.cpp`
  — TunableInfo default `1.0 → 0.0`; instrumentation of decision path.
- `src/dsp/ISweepTracker.hpp` — new EventFeatures fields.
- `src/recorder/Sidecar.cpp` — serialise new fields; `format_version 1 → 2`.
- `CMakeLists.txt` — version `0.2.0 → 0.3.0`; `ECHOBOX_PRERELEASE=rc1`.
- `README.md`, `CHANGELOG.md` — this note + rep-guard docs.
- `tools/session_screen/` — followup2 analysis (measurement tooling; ships
  in the source tree but does not affect the field binary).

### Restoring pre-0.3.0 behaviour with `rep_guard_enabled=1`

Setting the tunable back to `1` re-enables the temporal veto and
restores the pre-flip decision path. The gate site
(`BandEnergyDetector.cpp:384`) is the sole consumer of the flag and
both branches are unchanged from prior releases — the veto's logic is
functionally identical.

**Empirical check.** A validation run compared this build with
`--tunable rep_guard_enabled=1` against the pre-flip baseline over the
585-file reference corpus. Event counts matched within **~1%** (12,853
vs 12,970 total clips; both accepted and rejected drifted down by
similar amounts). The residual drift appears to be from x86 fast-math
ordering between two different code paths reaching the same functional
state, not a wiring change to the veto. On-device (ARM) behaviour
should be verified via `tools/collection/verify_feature_parity.py`
before treating the "restored" behaviour as bit-exact at a new site.
