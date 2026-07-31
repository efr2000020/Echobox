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

## Running the pytest suite

```
pip install -r tools/validator/requirements.txt   # soundfile, numpy, pytest
PYTHONPATH=. python -m pytest tools/collection/tests -q
```

(The absolute-import path — `tools.collection.session_layout` — means
the repo root has to be on `PYTHONPATH`. The `PYTHONPATH=.` prefix is
the least-intrusive way to satisfy that without touching global state.)
