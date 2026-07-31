# `tools/collection/` — offline verifiers for the data-collection overlay

Run these against a session directory produced by
`--collection-mode on` (see the plan doc,
`private_docs/plans/DATA_COLLECTION_IMPL_VALIDATION_PLAN.md`, §3–4).

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
sample-range overlap, and prints a per-clip diff.

- **Every real decision matches** ⇒ **GREEN**. The model is promoted
  from YELLOW (Python-only self-consistency) to GREEN (independent
  agreement with the C++ implementation on real inputs).
- **Any divergence** ⇒ **RED**. Per the plan's §4.1 rule: "flag it,
  quantify the disagreement, and correct the model before any further
  use." This tool exits non-zero on a single mismatch. Do not average.

Unit tests for the correlation logic (mismatch flag, missing-real,
missing-model) live in `tests/test_verify_recorder_model.py` — those
run without the native lib. End-to-end (real chunks + real firmware
decisions) requires the bench-rehearsal path documented in
`private_docs/plans/DATA_COLLECTION_IMPL_VALIDATION_PLAN.md` §5.

## §4.2 harness-vs-plugin — GREEN by construction, with one caveat

The plan calls for comparing the offline detector harness against the
production plugin build. Because `tools/validator/native.py` loads the
**production `libechobox_validator.so`** via `ctypes` (which in turn
uses the same production STFT/HPF and the same `BandEnergyDetector.so`
the shipping binary loads), the offline harness IS the plugin — there
is no shadow port to diff against.

Provenance ladder:

- **STFT + HPF + detector**: **GREEN by construction**. The ctypes
  bridge marshals frames into the same `.so` the production binary
  links against; a bit-flip in the algorithm would show up in both at
  once.
- **The remaining risk** is limited to the ctypes marshalling layer
  (endianness, struct packing, float alignment). That risk is bounded
  by the `_pack_ = 1` declaration on `_Annotation` in `native.py` and
  by the fact that the API's core call sites are exercised on every
  `tools/validator/cli.py` run — a marshalling drift would break the
  daily validator workflow, not silently corrupt one specific test.

**No separate cross-check tool ships in this commit.** The equivalent
test would be running the standalone `Echobox` binary and the ctypes
harness on the same audio and diffing per-frame features; because they
share the entire code path (STFT + detector), the diff is guaranteed to
be empty modulo the marshalling bounds above. `VALIDATION_PROVENANCE.md`
records this posture explicitly.

## Running the pytest suite

```
pip install -r tools/validator/requirements.txt   # soundfile, numpy, pytest
PYTHONPATH=. python -m pytest tools/collection/tests -q
```

(The absolute-import path — `tools.collection.session_layout` — means
the repo root has to be on `PYTHONPATH`. The `PYTHONPATH=.` prefix is
the least-intrusive way to satisfy that without touching global state.)
