# `tools/collection/` — offline verifiers for the data-collection overlay

Run these against a session directory produced by
`--collection-mode on` (see the plan doc,
`private_docs/plans/DATA_COLLECTION_IMPL_VALIDATION_PLAN.md`, §3–4).

Operator-facing docs live in `docs/`:

- [`docs/VALIDATION_PROVENANCE.md`](docs/VALIDATION_PROVENANCE.md) —
  every metric this branch can emit, its backing truth, and its current
  colour on the provenance ladder. **Read this before quoting any
  number this branch produced.**
- [`docs/DEPLOYMENT_CHECKLIST.md`](docs/DEPLOYMENT_CHECKLIST.md) —
  pre-departure / on-site / before-dark / during / after checklist for
  a one-shot field session.
- [`docs/BENCH_REHEARSAL.md`](docs/BENCH_REHEARSAL.md) — the executable
  gate that authorises the field session.

## The three §3 firmware-integrity checks

| # | Check | Provenance | Script |
|---|---|---|---|
| 1 | **Injected known signal** — a probe WAV played into the mic appears at the predicted sample offset in Stream A, is contained by the corresponding Stream B clip, and is logged in Stream C. | **GREEN** once run on hardware. **YELLOW/RED until then.** | `inject_signal.py generate` / `inject_signal.py verify` |
| 2 | **Zero-gap / continuity** — the Stream A manifest is self-consistent and every WAV's frame count matches. Alone: **YELLOW-strong.** | Self-consistent | `verify_stream_a.py` |
| 3 | **Cross-stream byte-identity** — every Stream B event clip is bit-identical to the same sample range extracted from Stream A. Because A and B are written by separate code paths off separate rings, agreement on real samples is independent evidence. **GREEN.** | Independent (separate writers) | `verify_ab_identity.py` |

## Pre-registered falsifiers

The verifiers exit non-zero on any of these; the pytest suite
(`tools/collection/tests/test_verifiers.py`) mutates the fixture to
trigger each one so the falsifier list can't silently rot into
documentation-only.

- **F1** (Stream A): missing manifest-listed WAV.
- **F2** (Stream A): WAV frames ≠ manifest frames.
- **F3** (Stream A): non-contiguous chunks with no drop increment.
- **F4** (Stream A): `drops_snapshot` decreases across chunks.
- **F5** (Stream A): `SESSION_END.end_sample` < last chunk's `end_sample`.
- **F1** (A/B): event WAV sample range not covered by A.
- **F2** (A/B): single-sample PCM divergence between B and the A slice.
- **F3** (A/B): event WAV length ≠ configured window width.
- **F1** (injected): tone not detected at all.
- **F2** (injected): tone offset off by more than `--tolerance-ms`.
- **F3** (injected): expected Stream B clip absent.
- **F4** (injected): no Stream C event overlaps the tone range.

## Provenance summary

If a session passes **check 2 + check 3**, its sample-clock arithmetic
and its per-writer WAV outputs are **GREEN**. Passing check 1 as well
promotes the front-end (mic + HPF + capture path) from unverified to
GREEN — the only §3 element requiring hardware to close.

## §4.1 recorder-model cross-check

`verify_recorder_model.py <session_root>` — the "does the Python
`recorder_model.py` agree with the shipping C++ recorder on real field
data?" test. Reads decisions from Stream C, runs `recorder_model` on
Stream A chunk-by-chunk, correlates predictions with real decisions by
sample-range overlap, and classifies each divergence:

- **Every real decision matches** ⇒ **GREEN**. Model promoted from
  YELLOW to GREEN.
- **Divergence explained by boundary-proximity numerical drift**
  (event feature within the parity check's `max_delta` of a gate
  threshold) ⇒ **YELLOW**. Reported separately, does NOT fail. Fix
  the numerical drift (see §4.2 / row 4b), not the model logic.
- **Unexplained divergence** ⇒ **RED**. Real state-machine drift.
  Fix the model. Per the plan's §4.1 rule: "flag it, quantify the
  disagreement, and correct the model before any further use." This
  tool does NOT apply a blanket percentage tolerance.

This tool reads `<session_root>/parity_report.json` — produced by
`verify_feature_parity.py` — to size the "boundary-proximity" bound.
Run parity first; without it, the tool announces a conservative
fallback and warns.

Unit tests for the correlation + boundary logic
(`tests/test_verify_recorder_model.py`) run without the native lib.
End-to-end (real chunks + real firmware decisions) requires the
bench-rehearsal path documented in
`private_docs/plans/DATA_COLLECTION_IMPL_VALIDATION_PLAN.md` §5.

## §4.2 harness-vs-device — YELLOW; measured by `verify_feature_parity.py`

The plan asks whether the offline detector harness computes the same
per-frame features as the device. `tools/validator/native.py` loads
`libechobox_validator.so` via ctypes, so the offline path **shares
source** with the device (`BandEnergyDetector.cpp` and the STFT + HPF
code). But sharing source is not the same as producing identical
outputs:

- The dev `.so` is built **x86** with `-march=native -ffast-math` (see
  the top-level `CMakeLists.txt`). The device is **ARM** (Pi Zero 2 W).
  Different ISAs, different SIMD lanes, non-IEEE-strict math on both
  sides.
- Near the gate's hard thresholds — `min_bandwidth_khz = 0.9`,
  `rep_cv_min = 0.50`, `rep_cv_max = 1.30` — small numerical drift can
  flip a per-event verdict. If that happens, `verify_recorder_model.py`
  (§4.1) will report divergences that are numerical, not model-logic
  bugs.

`verify_feature_parity.py` is the tool that measures this drift so we
can distinguish "numerical noise near a threshold" from "the model is
wrong". It:

1. Replays each Stream A chunk through the offline detector.
2. Correlates each harness event with the device's own event record in
   Stream C by `start_frame` (± a small tolerance).
3. Emits max + p50/p95/p99 deltas per feature
   (`bandwidth_khz / drift_khz / path_ratio / mono_fraction / cv_idi`).
4. Counts **boundary-proximity events** — how many device events fall
   within `max_delta` of any gate threshold, i.e. how many verdicts
   could flip under the measured harness↔device drift.

**Promotion rule**:

- **YELLOW** by default. This is the honest starting position.
- Promoted to **GREEN** only when the parity check runs on real bench
  data (x86 harness output vs an actual ARM device on the same audio),
  every per-feature delta is within tolerance, AND the
  boundary-proximity count is zero.
- Non-zero boundary-proximity count keeps 4b YELLOW *and* calibrates
  the tolerance §4.1's tool uses to distinguish boundary drift from
  model-logic errors.

**Pre-registered falsifier**: any single event with a feature delta
larger than `--max-delta`, or any boundary-proximity flip that
`verify_recorder_model.py` cannot explain, is treated as a failure —
do not average it away.

## Running the pytest suite

```
pip install -r tools/validator/requirements.txt   # soundfile, numpy, pytest
PYTHONPATH=. python -m pytest tools/collection/tests -q
```

(The absolute-import path — `tools.collection.session_layout` — means
the repo root has to be on `PYTHONPATH`. The `PYTHONPATH=.` prefix is
the least-intrusive way to satisfy that without touching global state.)
