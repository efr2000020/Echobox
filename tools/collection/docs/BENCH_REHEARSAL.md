# Bench rehearsal — the field-session gate

Per `DATA_COLLECTION_IMPL_VALIDATION_PLAN.md` §5, this is a gate not a
nicety. The field session is authorised only after every item below has
been executed on the bench and signed off. Find bugs here; the field
session is one-shot.

## What "the bench" looks like

- Echobox unit (Pi Zero 2 W + Ultramic 384K, or the equivalent build
  target).
- A speaker capable of playing a ~40 kHz tone within a few centimetres
  of the mic (any consumer tweeter that goes to 40 kHz works). ALSA
  loopback (`aloop`) is a viable substitute for the mic input path if
  no ultrasonic speaker is available — document which was used.
- Analysis machine with the repo checked out, `./build_dev.sh` run,
  and the pytest venv installed
  (`pip install -r tools/validator/requirements.txt pytest soundfile`).

## The rehearsal sequence

### 1. Prepare the injected probe

On the analysis machine:

```
PYTHONPATH=. python -m tools.collection.inject_signal generate \
    --out probe.wav \
    --sample-rate 384000 \
    --tone-hz 40000 --tone-ms 50 \
    --pre-silence-ms 2000 --post-silence-ms 2000
```

This writes `probe.wav` and `probe.json`. Copy both onto whatever the
speaker will read (or into `aplay` reach on the unit).

### 2. Cold start the unit

```
./deploy/bin/Echobox \
    --collection-mode on \
    --collection-dir /mnt/collection \
    --collection-max-hours 0.5 \
    --collection-min-free-mb 200 \
    --collection-site-note "bench-rehearsal $(date +%F)" \
    --log-level info
```

Confirm:

- The `SESSION_START` log line appears with a sane `est_hours`.
- `<collection-dir>/SESSION_HEADER.json` exists and lists the streams
  you expect.

### 3. Play the probe

Wait ~5 s after unit start so the reference chunk 0 has samples in it,
then play the probe once through the speaker (or via `aplay` into
`aloop`). Note the wall-clock time you triggered playback.

### 4. Fill a tiny test volume — governor exercise

Point `--collection-dir` at a small tmpfs / loop-mounted volume (say
50 MiB) and let the unit run until the free-space floor trips. Confirm:

- `SESSION_GOVERNOR_TRIP free-space-floor-hit ...` appears in the log.
- `SESSION_END.json` is written with `reason: "free-space-floor-hit"`.
- No orphan `.tmp` files remain in the collection dir.

Also exercise the max-duration path with `--collection-max-hours 0.01`
(~36 s): confirm `reason: "max-duration-reached"`.

### 5. Run the verifiers offline

On the analysis machine, from repo root. **Run `verify_feature_parity`
BEFORE `verify_recorder_model`** — the parity report calibrates the
recorder-model tool's boundary tolerance, so §4.1 without §4.2 will
either produce spurious RED (using a too-tight bound) or a loosely
quantified fall-back.

```
PYTHONPATH=. python -m tools.collection.verify_stream_a <session>
PYTHONPATH=. python -m tools.collection.verify_ab_identity <session>
PYTHONPATH=. python -m tools.collection.inject_signal verify \
    <session> probe.json --tolerance-ms 50
PYTHONPATH=. python -m tools.collection.verify_feature_parity <session>
PYTHONPATH=. python -m tools.collection.verify_recorder_model <session>
```

### 5a. Why `verify_feature_parity` must run on the bench, not just in the field

The offline harness is built for **x86** with `-march=native
-ffast-math` (see the top-level `CMakeLists.txt`); the device runs
**ARM** (Pi Zero 2 W). Same source, different arch, non-IEEE-strict
math on both sides — so `libechobox_validator.so`'s per-event feature
values do NOT exactly match the device's. Near the gate thresholds
(`min_bandwidth_khz = 0.9`, `rep_cv_min/max = 0.50 / 1.30`) that
numerical drift can flip individual per-event verdicts and produce
spurious RED reports in `verify_recorder_model` on the field data.

The bench is where that drift is *measured*, not where it is
*discovered*. If `verify_feature_parity` reports per-feature deltas
larger than the tool's `--max-delta` (default 0.05), the field session
is not authorised until either:

- the drift is understood and the tolerance widened with a written
  justification (and any consumer of the recorder-model output warned
  that it now runs YELLOW-with-quantified-drift), or
- a build variant with tighter numerics is used. **The cheap option is
  `cmake -DECHOBOX_STRICT_MATH=ON`** (available since this branch),
  which replaces `-ffast-math` with `-fno-fast-math` in Release builds
  so both sides use IEEE-strict re-association. If that alone doesn't
  bring deltas within `--max-delta`, cross-compile the offline `.so`
  for ARM (via QEMU or a spare Pi) — closes the arch gap entirely.
  See `VALIDATION_PROVENANCE.md` row 4b for the promotion path.

## Sign-off gate

Before the field session is authorised (§5 in the plan):

- [ ] `verify_stream_a` exits 0. `drops=0`.
- [ ] `verify_ab_identity` exits 0. All event clips byte-identical to
      their Stream A slices.
- [ ] `inject_signal verify` exits 0. Tone detected at expected offset
      within tolerance.
- [ ] `verify_feature_parity` exits 0 (per-feature max delta within
      `--max-delta`). If it reports YELLOW (deltas within tolerance
      but boundary-proximity flippables > 0), the measured tolerance
      value is recorded in `parity_report.json` and passed forward to
      the recorder-model check automatically — a non-zero flippable
      count does NOT alone block field authorisation, but does keep
      row 4b YELLOW.
- [ ] `verify_recorder_model` exits 0. GREEN preferred; YELLOW (all
      divergences explained as boundary-drift under the parity
      tolerance) is acceptable **only if the parity tolerance itself
      is justified**. RED — unexplained divergences — blocks the field
      session unconditionally per plan §4.1.
- [ ] Governor exercise passes both max-duration and
      free-space-floor cases.
- [ ] The bench-rehearsal session directory is archived (rename it to
      `bench_rehearsal_<date>.tar.zst` and keep it alongside the field
      session). The archive must contain `parity_report.json` so a
      later auditor can reproduce the tolerance the field
      recorder-model check used.

If any of the above is not met, do not depart for the field. Fix on the
bench.
