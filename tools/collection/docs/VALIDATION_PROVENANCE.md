# VALIDATION_PROVENANCE — data-collection overlay

Every claim this branch's tooling can emit, mapped to its backing truth
and its colour on the plan's provenance ladder. Structure and colour
policy come from **R2** in
`private_docs/plans/DATA_COLLECTION_IMPL_VALIDATION_PLAN.md`:

- **GREEN** — checked against an independent truth (real hardware
  output, an externally-injected known signal, or human labels). May be
  reported as "validated".
- **YELLOW** — cross-checked only against another model or
  re-implementation that shares assumptions. Report as "self-consistent,
  not independently validated".
- **RED** — cannot currently be tied to any independent truth. Report
  as **"unverified — flagged"**.

## Snapshot as of this commit

**No field or bench run has happened yet.** The status below reflects
what the code alone can currently prove. Any GREEN cell qualified with
"once run" stays YELLOW/RED until the operator has actually executed
that check on hardware. `verify_recorder_model.py` reporting GREEN on a
real session promotes rows 4a and 4b to GREEN retroactively.

| # | Emitted metric / claim | Backing truth | Current colour | How to promote |
|---|---|---|---|---|
| 1 | Sample-clock monotonicity (`SampleClock`) | Unit test: multi-thread advance/read, atomic RMW ordering | **GREEN** (unit-verified) | — |
| 2 | Session-header pre-flight math (`estimateMaxHours`) | Unit test + hand-derivation from `sampleRate × 2 B/frame × 3600 s/h` | **GREEN** (unit-verified) | — |
| 3a | Stream A zero-gap continuity | `verify_stream_a.py`, self-consistent w/ its own manifest | **YELLOW-strong** on a real run; **RED** with no run yet | Once you run a session end-to-end and the script exits 0, row is YELLOW-strong. Row 3b + row 3a together → GREEN. |
| 3b | Stream A ↔ Stream B byte-identity | `verify_ab_identity.py`, comparing outputs of *independent writers* on real samples | **GREEN once run on real firmware output**; **RED with no run** | Run a session, run the script, non-zero exits are session-invalidating. |
| 3c | Injected-signal alignment (Stream A ↔ external truth) | `inject_signal.py verify` against a `probe.wav` played into the mic | **RED** (no hardware run yet). See "hardware-deferred items" below. | Play the probe into the mic on the bench, run `verify`, then on the field mic. Both passes required. |
| 4a | `recorder_model.py` vs shipping recorder verdicts | `verify_recorder_model.py`, per-clip diff on Stream C ↔ Stream A | **YELLOW** (Python model, no independent field-data check yet). Every audit that quotes `recorder_model` inherits YELLOW until this runs. | Run a session, run the script; **must** be all-agree GREEN. Any divergence → RED, fix the model. |
| 4b | Offline harness ↔ production plugin (STFT / detector) | `tools/validator/native.py` loads `libechobox_validator.so` via ctypes — the offline path IS the plugin. See `tools/collection/README.md` §4.2. | **GREEN by construction**; single caveat is the ctypes marshalling layer (bounded by `_pack_ = 1` on `_Annotation` + daily exercise by the validator CLI). | Nothing to promote beyond what already ships; the ctypes bridge itself is exercised on every `tools/validator/cli.py` invocation. |
| 4c | ML labeller error rates (§4.3 in the plan) | Human verification of BatDetect2 / customer classifier on strata that matter (CF/QCF, gate disagreements, random sample) | **RED / not-yet-relevant**. No labeller has been applied to a session; the analyst does this after the run. Every FN/FP number that consumes a labeller carries the labeller's error bar. | Label Stream A; hand-verify the CF stratum in particular (cricket↔horseshoe-bat confusion is documented); report each labeller's measured error rate on the produced 200 ms clips. |
| 4d | Shipping-gate false-negative rate | Row 4c labels + Stream C decisions, per species / SNR, with CIs | **RED**. Not computable until row 4c completes. | Post-labeller analysis. Report **"relative to what our front-end could hear"** unless an independent co-located recorder rules out front-end masking (which this branch does not require or ship — see row 4e). |
| 4e | Absolute (front-end-independent) FN rate | Independent co-located recorder covering the same site + time | **RED / unverifiable this session** unless a co-located device is deployed. Stated up front, not hidden. | Deploy a second recorder alongside the Echobox unit for the session. Compare their event traces. |
| 5 | `collection_mode=off` byte-identity with R2v3 | Unit test: `Session::start()` is a no-op when disabled; capture-loop skips the sample-clock RMW; no collection objects constructed | **GREEN** (unit-verified, plus commits are additive under kill-switch discipline) | — |

## Hardware-deferred items (spelled out, not glossed)

Per R2's rule "if a claim cannot be tied to an independent source of
truth and the agent cannot build a tool that would — raise a RED flag
and stop claiming that thing is validated" — these are the RED items
that cannot be closed on this dev machine and require the bench or
field:

- **Row 3c** (injected-signal). Needs a mic + playback loop. Documented
  in `BENCH_REHEARSAL.md`.
- **Row 3a / 3b** upgrades. Need a real session's output tree. The
  scripts and their falsifiers are wired; only the run is missing.
- **Row 4a** upgrade. Same story.
- **Row 4c / 4d / 4e**. Post-hoc offline analysis + optional co-located
  recorder. Landing scope is out of the firmware branch by design.

## What "done" means for this branch

A green firmware build with an **all-YELLOW** provenance table is
**not done** — R2's phrase for the failure mode. This branch is
ready-to-run-a-session when:

- The code + verifier scripts land (this commit set); **yes**.
- The bench rehearsal (`BENCH_REHEARSAL.md`) has been executed
  end-to-end; **not yet — no hardware**.
- A field session has been recorded and every §3 script exits 0; **not
  yet**.

Until the second and third bullets flip, every claim on this table is
either GREEN (unit-verified, doesn't need hardware) or **flagged**.
