# Echobox session-screen (validation quick-win)

A **rough** offline recall / cricket-FP screen for local tuning. Compares
Echobox's would-save decisions against BatDetect2 on the same raw
recordings — file by file, per-species broken out — and drops a
one-screen report plus three CSVs into an output dir.

**This is not a certification.** See "Two caveats" below; they also ride
at the top of every generated report.

## Not the same thing as `tools/validator/`

- `tools/validator/` — precise, device-exact detector tuning against a
  **curated, hand-annotated corpus** of single clips (per-event
  precision/recall, grid-search over tunables, GUI/CLI).
- `tools/session_screen/` (this dir) — **rough, per-file** recall / FP
  screen over a **whole field session's raw recordings**, using
  BatDetect2 as a proxy for truth. Different workflow, different data,
  different accuracy claim.

Use the validator when you need to justify a knob change with numbers;
use session-screen when you need to know whether a night's field
recording looks broadly sane.

## Quick start

```bash
# 1. install (one-time; pulls torch, ~2 GB)
python -m venv .venv
source .venv/bin/activate
pip install -r tools/session_screen/requirements.txt

# 2. run all three stages
python -m tools.session_screen.validate all \
    --input field_samples/collection_08-04-2026/collection/reference \
    --output-dir validation_out
```

Output ends up in `validation_out/`:

- `report.md` — headline recall + FP + per-species table with the two caveats at the top.
- `results.csv` — the per-species table as CSV.
- `disagreements.csv` — every file where the two sides disagreed, plus every CF/Rhinolophus hit for human review.
- `truth_manifest.parquet` — cached BatDetect2 output.
- `replay_manifest.parquet` — cached Echobox offline-harness output.

The two `.parquet` files are the caches: re-running `all` skips a stage
whose output already exists (add `--force` to recompute).

## Subcommands

Each stage can also run on its own:

```bash
# BatDetect2 over every WAV.
python -m tools.session_screen.validate truth  --input <dir> [--force]

# Echobox offline harness over every WAV.
python -m tools.session_screen.validate replay --input <dir> [--force]

# Compare the two parquet caches; emit report.md + CSVs.
python -m tools.session_screen.validate score \
    --truth  validation_out/truth_manifest.parquet \
    --replay validation_out/replay_manifest.parquet \
    --output-dir validation_out
```

### Ingesting the shipping app's `rejected/` capture

Pass `--rejected-dir <path>` to `score` (or `all`) to have the report
scan sidecars written by the shipping app's `--save-rejected` feature
and add a "Rejected-set — device's own near-miss population" section.

The `rejected/` capture is the **more trustworthy signal** than the
BatDetect2 comparison alone: it is exactly what the app threw away, so
the near-threshold count is the direct tuning target.

```bash
# Field unit ran with --save-rejected all (or boundary), producing:
#   /path/to/recordings/rejected/YYYY-MM-DD/*.wav + .json

python -m tools.session_screen.validate score \
    --truth   validation_out/truth_manifest.parquet \
    --replay  validation_out/replay_manifest.parquet \
    --output-dir validation_out \
    --rejected-dir /path/to/recordings/rejected

# Optional: also run BatDetect2 over the rejected clips, then join. The
# report gains "N of M rejected clips were real bats" — the direct
# 'recall we lost' number.
python -m tools.session_screen.validate truth \
    --input  /path/to/recordings/rejected \
    --output validation_out/rejected_truth.parquet
python -m tools.session_screen.validate score \
    --truth   validation_out/truth_manifest.parquet \
    --replay  validation_out/replay_manifest.parquet \
    --output-dir validation_out \
    --rejected-dir   /path/to/recordings/rejected \
    --rejected-truth validation_out/rejected_truth.parquet
```

### On-device — turning `--save-rejected` on

The shipping app has four modes, chosen by trade-off:

- `--save-rejected off` — default; behaviour byte-identical to today.
- `--save-rejected all` — keep every rejected clip. **Local /
  offline-validation mode**; storage-heavy. Pair with
  `--save-rejected-max-per-hour 0` on a scratch machine.
- `--save-rejected sample --save-rejected-sample-n 500` — random
  1-in-500 sample. Cheap continuous observability.
- `--save-rejected boundary` — save only near-threshold near-misses
  (within ±0.30 kHz of the sweep-shape gate's `min_bandwidth_khz`).
  **Highest-value field mode** — most tuning signal per megabyte.

Field deployments should always cap: `--save-rejected-max-per-hour 200`
is the default and matches a busy night's ceiling.

The `replay` subcommand auto-discovers `<input>/../SESSION_HEADER.json`
so the offline harness runs with the config the device actually used;
pass `--session-header PATH` to override, or omit for built-in defaults.

`--help` on each subcommand documents every flag; `--force` recomputes a
cached stage.

## Two caveats — always printed at the top of `report.md`

1. **BatDetect2 is NOT ground truth.** It is documented to confuse
   crickets with horseshoe (Rhinolophus / CF) bats — the exact weak
   spot we're trying to tune around. `disagreements.csv` always
   includes every CF/Rhinolophus hit so a human can spot-check these;
   do not treat the headline numbers as authoritative on CF cases.

2. **Replay is x86 with `-ffast-math`; the device is ARM.** File-level
   agree/disagree numbers are fine for local tuning. Per-event margins
   near the sweep-shape gate thresholds are indicative only. Use the
   §4.2 feature-parity tool (`tools/collection/verify_feature_parity.py`)
   for a numerical harness-vs-device comparison when it matters.

## Faster-than-real-time / drop-free

- `replay` uses the offline harness (`tools/validator/native.py` +
  `recorder_model.py`) — the production STFT + detector via FFI, with
  the audio thread bypassed. No ALSA, no ring buffer, no dropped
  samples by construction.
- `truth` batches all files through one loaded BatDetect2 model. Auto-picks
  a CUDA device if visible; `--device cpu` forces CPU.

## Resampling caveat, explicit

BatDetect2 targets **256 kHz** (Net2DFast_UK_same). The recordings are
384 kHz. BatDetect2 resamples internally; the effective rate is written
into every `truth_manifest` row (`resample_hz` column) so the report
never hides it. The Echobox side does not resample — the offline harness
runs at 384 kHz end to end.

## What can't be validated by this tool

- **BatDetect2 accuracy on our 200 ms short clips.** BatDetect2 was
  trained on longer full-recording windows; recall on very short clips
  may itself be lossy. The report says so.
- **Device-vs-x86 numerical margins.** Files near a gate threshold can
  flip between the two builds — see caveat 2. Use the §4.2 tool.
- **Per-event timing precision.** We compare per-file, not per-event —
  BatDetect2's event boundaries and Echobox's clip boundaries won't
  align sample-for-sample, so the CSV keeps timestamps but the
  headline is a coarser per-file agree/disagree.

## Tests

```bash
pytest tools/session_screen/tests -q
```

Covers the scorer + manifest IO on deterministic fixtures. The ML model
itself is NOT tested here — that's BatDetect2's own test suite's job,
and a fixture-free ML test would be flakier than useful.

## No `Co-Authored-By`

Per repo convention.
