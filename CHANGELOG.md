# Changelog

## 2.0.0-rc1 — 2026-08-14

> **Numbering note.** This follows the released **1.0.0** and is the
> first release candidate for **v2**. The `0.3.0-rc1` and `0.4.0`
> entries below were development milestones cut between the two on
> `feat/v2-cricket-gate`; they were never tagged or released. Read them
> as the road to this candidate, not as versions preceding 1.0.0.

Per-call recall against BatDetect2 proxy truth goes from **31.5 %
measured on the device** to **97.3 % / 98.1 %** across two full field
nights, with cricket filtering on. Three changes get it there: the
validation harness was fixed (it was not measuring the device at all),
two detector defaults were retuned, and the cricket gate was replaced.

### Cricket gate replaced with a confident-reject noise filter

The sweep-shape gate was evaluated as a classifier for the first time
and does not work. Rank-order separation between bat-positive and
non-bat detector events (AUC; 0.5 = chance):

| the gate's features | AUC | features it ignored | AUC |
|---|---:|---|---:|
| `bandwidth_khz` | 0.556 | `trigger_snr` | **0.781** |
| `drift_khz` | 0.518 | `band_index` | 0.252 → **0.748** inverted |
| `path_ratio` | 0.467 | `trigger_flatness` | 0.262 → **0.738** inverted |
| `mono_fraction` | 0.366 | | |

All four features it decided on are non-discriminative here; two are
inverted, meaning non-bat events scored *more* sweep-like than bats.
Most likely cause: Pipistrellus energy peaks on the narrowband QCF tail,
not the FM downsweep the gate was designed around.

The error was directional. The old gate **kept only what it could
confirm was a bat**, so anything uncharacterisable was destroyed —
−63 pp of per-call recall to remove ~76 % of non-bat clips, roughly 18
real bat events lost per non-bat event removed. The replacement
**rejects only what it can confirm is not a bat**:

```
reject iff  trigger_snr < 12  AND  trigger_flatness > 0.55  AND  band_index == 1
            (weak)                 (broadband)                   (20–45 kHz)
```

Full corpus, both nights, filter ON:

| | session_01 | session_02 |
|---|---:|---:|
| per-call recall | **97.3 %** | **98.1 %** |
| vs filter off | −1.1 pp | −1.3 pp |
| non-bat clips removed | 26.9 % | 26.3 % |
| clip purity | 88.5 → 91.1 % | 88.7 → 91.2 % |

Against the retired gate's 37.5 % / 35.9 % on the same corpus.

Thresholds are runtime tunables (`noise_reject_enabled`,
`noise_snr_max`, `noise_flatness_min`, `noise_band_max`) so a
cricket-heavy site can tighten toward ~54 % rejection for ~4 pp of
recall. `sweep_gate_enabled` is gone; `--cricket-filter` now drives
`noise_reject_enabled` and ships **on**. The four sweep features are
still computed and written to the sidecar as diagnostics, and still
drive the rep-guard's `clearBat` bypass — a keep-only path, where a
weak feature is harmless.

The two-tier provisional/close-time split is removed. Its inputs are all
final at event open, so a second evaluation could add nothing — and
three correctness bugs (A2, A3, A7) had lived in that machinery.
`outState.active` is now the raw detector state; the filter never
suppresses it, so the recorder writes each clip in full and the discard
happens once, at `endRecording()`.

**Scope of the claim.** On both reference nights the non-bat population
is weak *broadband* noise (flatness median 0.63), not tonal cricket
harmonics — much of it admitted by lowering `band_snr_threshold` to 8.
This is validated as a **weak-broadband-trigger filter**. Neither night
contains a cricket-dominated stretch, so a tonal-cricket branch was
deliberately not attempted: tuning one against absent data is exactly
how the retired gate failed. It needs a cricket-heavy field recording.

**Open question.** `noise_snr_max` re-thresholds the same top-K SNR
statistic `band_snr_threshold` already uses, one aggregation later, so
"snr 8 + filter" may be doing little that "snr 12, no filter" would not.
The two differ in principle — frame-level vs event-level — but that has
not been measured. Treat the SNR arm as unproven; the flatness and band
arms carry the independent information.

**Cost.** ~5× the clips of the pre-0.5.0 regime (150 k–241 k per night),
4.0–6.6 GB of audio per night, recorder duty 14.7–24.3 %. With sidecars
that is ~300 k–480 k files per night, which is real inode and
directory-listing pressure on a Pi Zero 2 W; per-hour sharding or a
container format is tracked separately and is not a recall problem.

Note `--save-rejected` requires `--cricket-filter on`. It always did —
with the filter off nothing is cricket-discarded, so `rejected/` stays
empty — and this now fails validation at startup with a message naming
the flag, rather than silently producing nothing.

Measurements: `private_docs/audits/01_per_call_recall_audit.md` §7b.

### Rejected-clip observability re-anchored on the live rule

Two helpers in `Recorder.cpp` were still reasoning about the retired
sweep gate after it was replaced.

- **`--save-rejected boundary`** selected near-misses by comparing an
  event's `bandwidth_khz` against `min_bandwidth_khz` — a threshold that
  no longer rejects anything, so the mode was anchored on a dead axis.
  It now reads `noise_snr_max` and keeps a rejected event whose
  `trigger_snr` lands in `[noise_snr_max - 2.0, noise_snr_max)`: the
  band where the reject rule's SNR clause only just held, so a
  marginally louder call would have been kept. The window is one-sided
  — an event at or above `noise_snr_max` cannot have failed that clause
  at all. Note the margin is in the detector's **linear** SNR (band
  magnitude / noise floor), not dB.
- **`rejected_reason`** in the sidecar could report `"sweep"`, a cause
  that can no longer occur. It now classifies on `veto_applied`:
  `"temporal"` for the rep-guard, `"noise"` for the confident-reject
  rule, matching what `BandEnergyDetector` already prints at event
  close. `"noise"` is a new value for anything reading that field —
  `tools/session_screen/rejected.py` and `manifest.py` pass it through
  unchanged, but their comments still list the old set.

Known cosmetic gap: the `min_bandwidth_khz` tunable's own help text
(surfaced as the GUI tooltip) still claims the recorder's boundary mode
reads it. Not fixed here.

### Replay harness is deterministic and device-faithful

`echobox-replay` fed the pipeline as fast as the CPU allowed (20-30x
real time), but `Recorder` drove its state machine off
`std::chrono::steady_clock`: the poll cadence and the `silenceMs` idle
timeout were wall-clock while `maxLengthMs` was counted in audio
frames. In replay those two clocks were decoupled by a load-dependent
factor, so the tool was non-deterministic and did not reproduce device
behaviour. Three identical single-file replays, same binary, same
flags, produced **635 / 650 / 650** clips.

- `Recorder` takes an optional injected time source
  (`src/recorder/RecorderClock.hpp`). Everything time-derived now goes
  through it: the poll sleep, the `silenceMs` timeout, the
  `RECORDING_SAVED` elapsed trace, the rejected-sink hourly governor,
  and the wall stamp behind recording filenames and the sidecars'
  `capture_iso8601` / `boot_iso8601`.
- `echobox-replay` supplies a virtual clock advanced by samples fed
  (`tools/replay/VirtualClock.hpp`), interlocked with the DSP and
  recorder threads so the recorder observes exactly the snapshot
  sequence a real-time device would. Same three runs now produce
  **998 / 998 / 998** clips with byte-identical WAVs and sidecars. The
  final drain is virtual too, replacing a `sleep_for`.
- Replay is still far faster than real time — unchanged at ~20-30x on
  the reference machine. The lockstep costs no measurable throughput
  because the run is FFT-bound, not sleep-bound.
- Clip counts rise versus older replay runs: the silence timeout now
  actually fires instead of being swamped, so clips close on silence
  rather than always on `maxLengthMs`. **Replay numbers from before
  this change are not comparable to numbers after it.**

**The field build is unaffected.** The seam sits behind the
`ECHOBOX_RECORDER_CLOCK_INJECTION` compile-time gate, set only when the
unit tests or the replay tool are built, so the shipping translation
unit has no `RecorderConfig::clock` member and no dispatch branch.
Verified by building `deploy/bin/Echobox` at the same commit with and
without the change: identical instruction count (44 365) and identical
section sizes to the byte, with the whole disassembly differing by one
mirrored-but-equivalent compare that GCC chose on its own
(`cmp %rax,%rdx; jg` vs `cmp %rdx,%rax; jl`).

### `echobox-replay`: `--log-level`, `--log-dir`, `--console-log`

The tool's comments already claimed the logger was "silent unless the
user opts in via `--log-level`", but the CLI parser never read the
flag, so a replay run had no way to recover the detector's per-event
`[dsp.bed]` trace or the recorder's `RECORDING_OPEN/SAVED/DISCARDED`
lines. Wired up, reusing the shipping `logging::parseLevel` so the
accepted spellings cannot drift from `Echobox`'s. Default is still
`off` — no implicit disk I/O.

`printHelp()` also quoted four pre-0.4.0 defaults that no longer
matched the binary (`--snr-threshold 12.0`, `--preroll-ms 50`,
`--silence-ms 50`, `--max-length-ms 200`); they now read 8.0 / 10 / 20
/ 40. Same stale `--snr-threshold` value fixed in `TOOLING.md` §5.


### Detector retune for per-call recall

Two shipped detector defaults moved. Both were measured on the
deterministic offline harness (production DSP driven frame by frame
with no recorder and no threads, so the numbers are sample-accurate
and reproducible), with the cricket gate disabled, over 24 stratified
bat-positive files per session, scored per-call against BatDetect2:

- `max_flatness` **0.65 → 0.80**
- `band_snr_threshold` **12.0 → 8.0** (`--snr-threshold`)

Detector-level per-call recall, session_01 / session_02:

| config | recall | detector duty |
|---|---:|---:|
| snr 12, flat 0.65 — previous ship | 85.88 % / 90.77 % | 13.8 / 14.3 % |
| snr 12, flat 0.80 | 91.66 % / 94.04 % | 14.8 / 15.0 % |
| **snr 8, flat 0.80 — new ship** | **97.43 % / 97.93 %** | **19.1 / 19.0 %** |

`max_flatness` was free: the entire effect lands in the 0.65 → 0.80
step, and 0.90 and 1.00 (bound off) measure identically to 0.80 — no
real bat frame in this corpus has spectral flatness above 0.80. The old
0.65 ceiling was cutting into the bat population while buying no
broadband rejection that 0.80 doesn't already buy.

`band_snr_threshold` is the real trade: −4 on the threshold buys
+5.8 / +3.9 pp of recall and costs +4.3 / +4.0 pp of duty cycle. Below
8 the recall curve saturates (~98 % at snr 6) while duty keeps rising.

**Storage impact — read this before deploying.** Detector duty cycle
rises from **~14 % to ~19 %**. The target device is a solar-powered
Raspberry Pi Zero 2 W, and duty cycle is roughly proportional to bytes
written per night, so expect **more clips and more bytes per night** —
on the order of a third more recorded audio. This is a deliberate
recall-for-storage trade, not a free win. Sites that are tight on SD
card or battery can pass `--snr-threshold 12.0` to get the old
sensitivity back; the flag is unchanged and well understood.

Clip length is unaffected: the shipped recorder geometry stays
`10 / 20 / 40 ms` (pre-roll / silence / max length), inside the 50 ms
end-to-end product ceiling. These knobs change how *often* the detector
opens an event, never how *long* a clip runs.

This resolves the `max_flatness = 0.65` item filed under 0.4.0's known
limitations. The 0.4.0 sweep had measured the gain per-pass and judged
it below the commit threshold; measuring per-call on a larger corpus
shows the effect is substantially bigger than that round could see.

Also updated to match: the tunable-registry defaults, the
`quiet` / `balanced` / `noisy` preset bundles (`balanced` is documented
as "Echobox defaults" and now reproduces them exactly; `quiet` drops to
snr 6.0 so the ladder keeps three distinct rungs), `--snr-threshold`
help text, `README.md`, `DSP_PIPELINE.md`, and the `shipping` preset in
`tools/session_screen`. The `legacy` and `shorter` screening presets are
frozen history and now pin `snr_threshold` explicitly at 12.0 so a
future default change cannot silently rewrite them.

**Scope note.** These are *detector-level* numbers. End-to-end recall
with the cricket gate on remains ~30 %, because the gate rejects ~82 %
of detector events — that is a separate, known work package and no
amount of base-detector tuning moves it. Expect this change to be
largely invisible in end-to-end figures until the gate is addressed.

### Data-collection overlay, forward-ported onto mainline

The `feat/data-collection` branch — the firmware that produced the
`session_01` / `session_02` field corpora everyone still validates
against — is now part of the mainline instead of a stale side branch
eight commits behind a detector rewrite. It is a validation-only
superset of the shipping recorder, off by default, enabled at **runtime**
with `--collection-mode on`. There is no build flag: one binary serves
both a field unit and a collection deployment.

Four streams, all stamped from one monotonic sample clock so they are
alignable without ever consulting wall time:

| | Stream | Output |
|---|---|---|
| **A** | continuous raw reference audio, tapped off ALSA *before* the float conversion and DSP | rolling 60 s WAV chunks + `chunks.jsonl` |
| **B** | one WAV per detector event — accepted **and** rejected | `events/{accepted,rejected}/<start_sample>.wav` |
| **C** | one record per event, one per recorder clip decision | `decisions.jsonl` |
| **D** | the ordinary operational log | a documentation label, not separate code |

Plus a session governor that stops cleanly on a duration cap or a
free-space floor, and a `SESSION_HEADER.json` carrying the firmware SHA,
the full decision-relevant config, and the `start_wallclock ↔
start_sample` anchor.

**Collection-off is the load-bearing property.** With the default
`--collection-mode off`: nothing is constructed, no thread is spawned,
no ring is allocated, and the audio capture loop's added cost is exactly
**three predictable, never-taken branches per 4096-frame ALSA read** —
verified by disassembling `Application::captureLoop` before and after.
GCC lays all three collection taps out as cold, out-of-line blocks, so
the off path executes no atomic RMW, no call, and no memory write. The
recorder pays one null check per clip close.

Two places the branch disagreed with current code, and what was done:

- **`EventFeatures` gained six decision-path fields** since the overlay
  was written. Stream C logs the current set and derives a
  `reject_clause` from `veto_applied` — `"temporal"` vs `"noise"`. The
  plan asked Stream C to record "which clause fired"; its original
  vocabulary named the retired sweep-shape gate, so the question is
  re-asked against the rule that actually decides today.
- **The shadow `would_save_R2v3` field never existed.** It was the
  plan's name for the recorder's own per-clip verdict record. That
  record is kept — it is the only device-side ground truth for what the
  recorder half of the cricket filter did — and the `R2v3` label is
  dropped, because it named a gate this firmware no longer has.

Not ported: `verify_recorder_model.py`, `verify_feature_parity.py`, and
the `ECHOBOX_STRICT_MATH` CMake option. All three served
`tools/validator/recorder_model.py`, a Python shadow model of the
recorder that is stale and unused now that `tools/session_screen` drives
the real C++ through `echobox-replay`. The docs that referenced them are
updated rather than left pointing at absent tools. The cost is recorded
rather than glossed: the ARM-vs-x86 numerical question `§4.2` measured
is now **unmeasured, not answered**, and `VALIDATION_PROVENANCE.md`
row 4b keeps it YELLOW.

Two bugs found in the stale code and fixed on the way in:

- The collection event mirror was pushed to **unconditionally**, so a
  shipping unit accumulated a second copy of every event in a vector
  nothing ever drained — ~80k events of unbounded growth over the 9.7 h
  field session, on a 512 MB Pi Zero 2 W. It is now armed by the
  collection poller's first drain and never armed in a shipping unit.
- `--save-rejected` preserving a cricket-discarded clip is an exit path
  from `endRecording()` that postdates the overlay. Left unpublished,
  every rejected-sink clip vanished from Stream C whenever that mode was
  on.

Shipping-config per-call recall on `session_02` is unchanged by the
port. `verify_stream_a` gains falsifier **F6**: a session whose header
declares Stream A enabled but whose manifest lists no chunks now fails
instead of passing vacuously.

### Test suite

Repaired five unit tests that had been red since the 0.3.0-rc1 / 0.4.0
rounds changed shipped defaults without updating the assertions (the
pre-A6 40 ms silence floor, the pre-0.4.0 clip geometry, sidecar
`format_version`, the rep-guard default, and `computeSweepShape` calls
that passed a value positionally into the `anchorBin` parameter A1
introduced). No production code was involved. `ctest` is 60/60 green,
and the short-clip test now explicitly guards the 50 ms clip ceiling.

## 0.4.0 — 2026-08-09

The short-clip release: cricket-gate correctness fixes + a much
shorter default clip geometry chosen for the customer's
solar-powered downstream BatDetect2 workflow. Supersedes the
untagged 0.3.0-rc1.

### Cricket-gate correctness (A1–A4, A7)

Four correctness defects in the sweep-shape / cricket gate were
silently discarding real bat clips. Fixes landed as one commit each:

- **A1** — The 10-dB bandwidth walk was anchored on a re-scanned
  global arg-max across the full 20–192 kHz range, so a louder
  narrowband source elsewhere in-band could drag the walk onto
  itself and rate the actual bat event as narrowband. Now anchored
  on the winning sub-band's dominant bin at the loudest-snapshot
  frame, confined to that sub-band's bin range.
- **A2** — Split the single `m_gateRejected` verdict into a
  non-binding `m_provisionalSuppressed` (fast-drop for the
  recorder) and a binding close-time `m_gateRejected`. The
  close-time re-evaluation now runs unconditionally on gate-on
  runs, so a provisional reject taken at ~8 ms in is no longer
  permanent.
- **A3** — Dropped the one-shot latch on the provisional gate.
  It now re-evaluates every `GATE_DECISION_FRAMES` (6) hot frames
  and can flip both ways, so a bat call arriving inside a
  still-open cricket event has a chance to reopen the recorder's
  window.
- **A7** — The sidecar's `provisional_rejected` field is no
  longer sticky-once; it now reflects the event's final
  suppression state after A3's re-evaluation.

Measured on the reference corpus (585 × 1 min WAVs, session
08-04-2026) at the previous 200 ms geometry:

- per-file recall  97.5 % → 98.6 %  (+1.1 pp; +5 files recovered)
- per-pass recall  64.6 % → 66.1 %  (+1.5 pp; +34 passes recovered)
- rejected clips   12 953 → 12 459  (−494 wrongly rejected)
- cricket-FP clips 301 → 310         (+9; expected, no compensating tuning)

### Short-clip defaults (A5, A6, geometry change)

New defaults, chosen by the Part B six-config sweep on the same
corpus:

- `--preroll-ms  50 → 10`
- `--silence-ms  50 → 20`
- `--max-length-ms 200 → 40`

End-to-end clip length: **40 ms** (from ~200 ms). Enabled by two
supporting changes:

- **A5** — Recorder poll interval `5 ms → 1 ms`. Term in the
  cricket-filter silence floor; also fixes a leading-edge miss on
  events whose active window is shorter than the old poll.
- **A6** — Recomputed the cricket-filter silence floor. Old:
  `ceil(8 · frame_ms) + 5 + 24 = 40 ms`. New:
  `ceil(8 · frame_ms) + 1 + 4 = 16 ms`. Old safety margin was
  arbitrary padding.

Measured effect on the same corpus, against the 0.3.0-rc1 200 ms
geometry, with A1–A7 in both arms:

- **MB/night   2 213 → 1 008  (−54 %)**  — customer's cost metric
- per-file recall  98.6 % → 99.8 %       (+1.2 pp)
- per-pass recall  66.1 % → 71.8 %       (+5.7 pp)
- cricket-FP rate  31.9 % → 35.1 %       (+3.2 pp)
- accepted-cricket count 310 → 3 902     (fragmentation: one long
  cricket sequence becomes ~6-7 short clips at the new geometry)

Long-pass profile (R1/R2v2-style grouping) remains available as an
opt-in:

```
--preroll-ms 1000 --silence-ms 2000 --max-length-ms 5000
```

### Instrumentation carried forward from 0.3.0-rc1

The rep-guard still ships **off by default** and the six decision-path
sidecar fields introduced in 0.3.0-rc1 remain unchanged
(`format_version: 2`).

### Caveats to read alongside these numbers

- **Re-check on ARM.** All figures are x86 replay with `-ffast-math`
  and `-march=native`. Near-threshold per-event values are a tuning
  proxy, not device-exact. Any threshold decision derived from
  these numbers should be re-verified via
  `tools/collection/verify_feature_parity.py` before landing on
  device.
- **BatDetect2 is a proxy screen, not ground truth.** The reference
  corpus is Pipistrellus-only — no CF/QCF species. Deltas between
  runs are trustworthy; absolute rates are soft. CF recall is
  untested on this session.

### Known limitations (not fixed this round)

- The 10-dB bandwidth walk is amplitude-relative, not
  noise-floor-relative. A future round should sweep this.
- `tests/unit/test_SweepShape.cpp` uses the pre-A1 signature and
  no longer compiles. Deliberately out of scope; do not gate
  release on the unit test suite.
- The global-band spectral-flatness upper bound (`max_flatness =
  0.65`) is a binding constraint on some short-geometry bat passes.
  Raising it to 0.80 recovered +5 pp per-pass in the sweep, but at
  only +0.2 pp per-file it did not clear the plan's committing
  threshold — filed for the next round.
- The rejected-clip pool grew ~5× at the new geometry (fragmentation).
  Rejected clips do not exfiltrate, so this is a bytes non-issue,
  but any per-clip analytics or dashboards should be sanity-checked.

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
