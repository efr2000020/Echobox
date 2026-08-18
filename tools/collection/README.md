# `tools/collection/` — offline verifiers for the data-collection overlay

Run these against a session directory produced by
`--collection-mode on` (see the plan doc,
`private_docs/plans/03_DATA_COLLECTION_IMPL_VALIDATION_PLAN.md`, §3–4).

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
- **F6** (Stream A): header declares Stream A enabled but the manifest
  lists no chunks — the writer produced nothing (or the reference audio
  was pruned from this copy). Without F6 such a session passes
  vacuously, which is how `session_02/on_device/` passed check 2 while
  containing no Stream A at all.
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

## What is NOT here: the §4.1 / §4.2 model cross-checks

The plan's §4.1 (`verify_recorder_model.py`) and §4.2
(`verify_feature_parity.py`) both existed to validate
`tools/validator/recorder_model.py` — a Python shadow model of the C++
recorder — and to measure the x86-harness ↔ ARM-device numerical drift
that could make that model look wrong when it was not.

Both tools are gone, along with the `ECHOBOX_STRICT_MATH` CMake option
that served them, because the thing they validated is no longer used.
`tools/session_screen` drives the **real C++ recorder** through
`echobox-replay`, so there is no second implementation left to disagree
with the first. The circularity the plan's §4.1 was designed to break —
"a Python model agrees with the C++ it was copied from" — is not broken
by a better cross-check; it is dissolved by deleting the copy.

What that costs, stated rather than glossed: the ARM-vs-x86 numerical
question §4.2 measured is now **unmeasured**, not answered. Replay runs
on x86; the field unit runs on ARM. Near the gate's hard thresholds that
drift can still flip a per-event verdict, and nothing in this directory
currently quantifies it. Treat any replay-derived per-event verdict as
**YELLOW** on that axis. The §3 checks below are unaffected — they
compare the device against itself and against an externally injected
signal, never against a re-implementation.

## Running the pytest suite

```
pip install -r tools/validator/requirements.txt   # soundfile, numpy, pytest
PYTHONPATH=. python -m pytest tools/collection/tests -q
```

(The absolute-import path — `tools.collection.session_layout` — means
the repo root has to be on `PYTHONPATH`. The `PYTHONPATH=.` prefix is
the least-intrusive way to satisfy that without touching global state.)
