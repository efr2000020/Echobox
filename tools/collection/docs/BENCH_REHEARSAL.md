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

On the analysis machine, from repo root:

```
PYTHONPATH=. python -m tools.collection.verify_stream_a <session>
PYTHONPATH=. python -m tools.collection.verify_ab_identity <session>
PYTHONPATH=. python -m tools.collection.inject_signal verify \
    <session> probe.json --tolerance-ms 50
PYTHONPATH=. python -m tools.collection.verify_recorder_model <session>
```

## Sign-off gate

Before the field session is authorised (§5 in the plan):

- [ ] `verify_stream_a` exits 0. `drops=0`.
- [ ] `verify_ab_identity` exits 0. All event clips byte-identical to
      their Stream A slices.
- [ ] `inject_signal verify` exits 0. Tone detected at expected offset
      within tolerance.
- [ ] `verify_recorder_model` exits 0 **or** every divergence has been
      explained and either the model or the firmware has been fixed.
      Divergences are not "acceptable at low rate" per plan §4.1.
- [ ] Governor exercise passes both max-duration and
      free-space-floor cases.
- [ ] The bench-rehearsal session directory is archived (rename it to
      `bench_rehearsal_<date>.tar.zst` and keep it alongside the field
      session).

If any of the above is not met, do not depart for the field. Fix on the
bench.
