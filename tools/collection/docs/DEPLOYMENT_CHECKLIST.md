# Deployment checklist — data-collection field session

Per `DATA_COLLECTION_IMPL_VALIDATION_PLAN.md` §7 (deliverables) and §9
(risks). A session is one-shot: bugs found in the field cost a night.

## Pre-departure (workshop bench)

- [ ] **Bench rehearsal completed and signed off** — see
      `BENCH_REHEARSAL.md`. Do not skip; the plan (§5) makes it a gate,
      not a nicety.
- [ ] Firmware SHA on the deploy card matches `git rev-parse --short HEAD`
      on `feat/data-collection`.
- [ ] `collection_mode=on` (with the `--collection-*` knobs set) has
      been verified to launch cleanly via `--help` and a short local
      run.
- [ ] Card format known and free space measured. `SESSION_HEADER.json`
      will re-quote it, but running out of `df` headroom mid-flight
      cost hours in prior sessions.
- [ ] Independent co-located recorder identified (see plan §4.4 / §9
      "shared front-end" risk). **If none is available, mark row 4e RED
      in the run report — do not omit.**

## On arrival (site)

- [ ] Mic placement + orientation logged (photo + a line in a paper
      notebook). Not on the card — the card is one point of failure
      away from losing this.
- [ ] Wind, temperature, and nearby noise sources noted for the site
      context log referenced in §7's deliverable list.
- [ ] Gain / clipping check: play a modest ultrasound source (finger
      snap, keys) and confirm no clipping in a 30-second capture. This
      is the last chance to reset gain before dark.

## Before dark (final gate)

- [ ] `df -h` on the collection card shows ≥ `--collection-min-free-mb`
      + a comfortable margin.
- [ ] `--collection-max-hours` set to something conservative given the
      card size (better an early clean-close than a corrupt tail).
- [ ] Writer keep-up check: run the unit for ~5 minutes, then confirm
      `verify_stream_a.py` on that fragment reports `drops=0`. If it
      doesn't, the field session is very likely to spam
      `STREAM_A_OVERFLOW`; abort and fix on the bench.
- [ ] Site notes go into `--collection-site-note "..."` so the session
      header carries them.
- [ ] Independent co-located recorder started, its own clock aligned
      against the Echobox unit's start_wall_iso8601 within a few
      seconds. If skipped, note in the run report.

## During the session (do not touch the unit)

- [ ] Monitor from a distance: heartbeat log line should tick once per
      minute; `dsp_dropped` should stay at 0.
- [ ] If a `STREAM_A_OVERFLOW` fires: note the time; the session is
      unsalvageable for absolute-sample science past that point (§3
      row 3a will report drops > 0). Continue only to preserve the
      audio itself.

## After the session (before touching the card)

- [ ] Confirm `SESSION_END.json` exists on the card. If not, the unit
      crashed or was power-cycled — the tail may be corrupt. Read the
      log for the last `HEARTBEAT`.
- [ ] Copy the entire session directory to two independent disks
      before running any verifier. The verifiers are read-only, but a
      failing card mid-analysis is a real risk.
- [ ] Run, in order, from a clean venv on an analysis machine:
      ```
      PYTHONPATH=. python -m tools.collection.verify_stream_a <session>
      PYTHONPATH=. python -m tools.collection.verify_ab_identity <session>
      PYTHONPATH=. python -m tools.collection.verify_feature_parity <session>
      PYTHONPATH=. python -m tools.collection.verify_recorder_model <session>
      ```
      `verify_feature_parity` MUST run before `verify_recorder_model` —
      it writes `parity_report.json` which calibrates the
      recorder-model tool's boundary-drift tolerance. Every step must
      exit 0 for the session to be report-eligible; any non-zero result
      blocks the row-promotion in `VALIDATION_PROVENANCE.md`.
- [ ] Fold results into the run report: number of clips saved by real
      firmware, model agreement %, drops, front-end caveats (per plan
      §4.4).

## What kills the run epistemically (per plan §9)

- Circular validation → hard-guarded by row 3b + row 4a using the
  firmware's own decisions as reference.
- Silent sample drops → row 3a + Stream D operational log.
- Shared front-end → row 4e; independent recorder or explicit RED.
- Small / unknown card → governor + pre-flight hours estimate.
- One-shot risk → bench rehearsal.
- Overfitting the new data → site-level frozen splits before any
  tuning (out of scope for this branch — flagged for the analysis
  step).
