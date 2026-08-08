# Echobox session-screen — rough algorithm assessment

Runs `echobox-replay` (the C++ offline tool that reuses the shipping
DspPipeline + Recorder) over a corpus of raw recordings and joins the
per-clip `accepted/` + `rejected/` output against BatDetect2 detections
by **time overlap**, producing a per-clip confusion matrix.

**This is a rough tuning proxy, NOT a customer-facing recall claim.**
See "Honesty ceiling" below; the same four points ride verbatim at the
top of every generated report.

## Not the same thing as `tools/validator/`

- `tools/validator/` — precise, device-exact detector tuning against a
  **curated, hand-annotated corpus** of single clips (per-event
  precision/recall, grid-search over tunables, GUI/CLI).
- `tools/session_screen/` (this dir) — **rough, per-clip** recall / FP
  screen over a **whole field session's raw recordings**, using
  BatDetect2 as a proxy for truth.

## Quick start

```bash
# 1. install (one-time; pulls torch, ~2 GB)
python -m venv .venv
source .venv/bin/activate
pip install -r tools/session_screen/requirements.txt

# 2. build the C++ replay binary (one-time)
./build_dev.sh                              # produces deploy/bin/echobox-replay

# 3. run the plan's two configs (baseline + shorter) end to end
python -m tools.session_screen.validate sweep \
    --input field_samples/collection_08-04-2026/collection/reference \
    --output-dir validation_out
```

Output ends up in `validation_out/`:

- `report.md` — honesty header, config-comparison table, pointers to per-config drill-downs.
- `truth_manifest.parquet` — cached BatDetect2 output (reused across configs).
- `baseline/` and `shorter/` — one per plan config:
  - `report.md` — honesty header, per-species confusion, three headline rates.
  - `results.csv` — the per-species confusion matrix.
  - `disagreements.csv` — every FN / FP / CF-review row, ranked by top confidence.
  - `replay_manifest.parquet` — per-clip cache (one row per accepted/rejected clip).
  - `replay_manifest.replay/` — the raw sidecar tree from `echobox-replay` (kept for debugging).

The parquet files are the caches: re-running `sweep` skips a stage whose
output already exists (add `--force` to recompute).

## Subcommands

Each stage can also run on its own:

```bash
# BatDetect2 over every WAV.
python -m tools.session_screen.validate truth  --input <dir> [--force]

# echobox-replay over every WAV for one config. --config picks a named
# preset (baseline | shorter); the per-knob flags override.
python -m tools.session_screen.validate replay --input <dir> \
    --config baseline
    # or: --preroll-ms 20 --silence-ms 40 --max-length-ms 200 ...

# Compare the two parquet caches; emit report + CSVs into --output-dir.
python -m tools.session_screen.validate score \
    --truth  validation_out/truth_manifest.parquet \
    --replay validation_out/replay_manifest.parquet \
    --output-dir validation_out

# One config end to end.
python -m tools.session_screen.validate all --input <dir> --config baseline

# Both plan configs + diff table.
python -m tools.session_screen.validate sweep --input <dir>
```

### Follow-up: presence recall + cricket-FP profile (no re-run)

After a `sweep` completes, `followup` re-buckets the existing per-clip
join to file granularity and characterises the cricket-FP leakers. No
replay or BatDetect2 re-run — reads the sweep artefacts + the sidecar
tree already on disk.

```bash
python -m tools.session_screen.validate followup \
    --output-dir validation_out
```

Outputs (added under `validation_out/`):

- `recall_summary.md` — the honest bracket: per-file → per-pass →
  per-clip recall, side by side.
- `<config>/recall_per_file.csv` — every source WAV + its presence bucket.
- `<config>/presence_fn_files.csv` — bat-present sources with zero saved
  clips, sorted by BatDetect2 detection count (human-ear queue).
- `<config>/cricket_fp_features.csv` — per-clip sweep-gate feature
  aggregates for the leakers, plus a TN + TP comparison sample.
- `<config>/cricket_fp_profile.md` — feature-distribution table +
  `min_bandwidth_khz` trade-off table (estimated, not applied — the
  auditor pattern is to weigh the numbers, not to tune).

Why the bracket matters: **per-clip recall is an *aggressiveness* metric
on bat-adjacent audio, not a presence metric.** A single bat pass
produces many BatDetect2 detections and only a few recorder clips, so
one discarded clip counts as many FN in the per-clip bucket. The
product goal (did we notice a bat in this file?) maps to per-file, and
that number is much higher than per-clip.

### Follow-up 2: veto + provisional recovery (v2 sidecars only)

Requires a replay run built against the instrumented C++ (sidecar
`format_version >= 2`). Splits every rejected clip into `veto_only /
provisional_only / both / or_fail` buckets, then sweeps every temporal
rep-guard knob + provisional gate on/off, scored per-call.

```bash
python -m tools.session_screen.validate followup2 \
    --output-dir validation_out/baseline_v2 \
    --config-label baseline
```

Outputs:

- `veto_provisional_recovery.md` — pool split + sweep table + two
  recommendations (efficient / max-recovery).
- `veto_provisional_recovery.csv` — the sweep, one row per (knob, value).

The sweep math is **exact** for the six veto knobs (`rep_cv_min/max`,
`rep_rate_min/max_hz`, `rep_min_events`, `rep_broadband_keep_khz`) —
it re-evaluates the C++ metronomic + clearBat conditions on the exact
per-event stored inputs. The provisional gate is reported as a single
on/off point (partial-event features aren't in the sidecar; a per-knob
`GATE_DECISION_FRAMES` sweep would need those added).

### Reusing a cached truth manifest across sweeps

The BatDetect2 truth stage takes ~90 minutes on CPU for 585 files, and
doesn't depend on the recorder config. To reuse an existing cache, drop
or symlink `truth_manifest.parquet` into your target `--output-dir`
before running:

```bash
mkdir -p validation_out
ln -s /absolute/path/to/cached/truth_manifest.parquet \
      validation_out/truth_manifest.parquet
python -m tools.session_screen.validate sweep \
    --input <dir> --output-dir validation_out
```

## Honesty ceiling — printed verbatim at the top of every report

1. **BatDetect2 is a screen, not truth.** On this corpus it labels
   everything Pipistrellus and finds no CF/QCF species — CF recall is
   NOT ASSESSED here.
2. **x86 replay != ARM device.** `echobox-replay` builds on x86 with
   `-ffast-math` / `-march=native`. Per-event / near-threshold numbers
   are a tuning proxy, not device-exact.
3. **Two false-negative classes.** `rejected/` captures "the gate
   discarded it". It does NOT capture "the base detector never
   triggered on it" — that needs continuous-reference labelling we
   don't have.
4. **No human adjudication yet.** Disagreements are counted and listed,
   not scored. Absolute rates are soft; run-to-run deltas are the
   trustworthy signal.

## How per-clip attribution works

`echobox-replay` writes each accepted / rejected clip as a WAV +
sidecar JSON. The sidecar's `device.frames_processed` is a monotonic
hop-frame counter within a single-source run, sampled at clip-write
time — so it lands (approximately) at the end of the clip in
source-hop coords. We compute:

```
clip_end_ms   = frames_processed × hop_size × 1000 / sample_rate
clip_start_ms = clip_end_ms - clip_duration_ms   # duration from WAV header
```

To keep attribution unambiguous, `replay.py` runs `echobox-replay` in
**one subprocess per source WAV** — the outer loop knows exactly which
source produced each sidecar without reverse-engineering a batched
`frames_processed` counter.

Per-clip → truth join: `score.py` widens each clip window by
`CLIP_EDGE_SLOP_MS` (20 ms) and asks "did any BatDetect2 detection in
this source WAV overlap the clip window?". If yes → bat clip (species
= top overlapping detection); if no → cricket clip.

## Rejected-capture wiring on-device

The shipping app has four `--save-rejected` modes:

- `off` — default; behaviour byte-identical to the pre-feature build.
- `all` — keep every rejected clip. **Storage-heavy**, used by
  `echobox-replay` internally to make offline scoring exhaustive.
- `sample --save-rejected-sample-n 500` — 1-in-500 sample. Cheap
  continuous observability.
- `boundary` — save only near-threshold near-misses (within ±0.30 kHz
  of the sweep-shape gate's `min_bandwidth_khz`). **Highest-value field
  mode.**

Field deployments should cap: `--save-rejected-max-per-hour 200` is
the shipping default and matches a busy night's ceiling. Replay runs
with `--save-rejected-max-per-hour 0` (unlimited) because offline
scoring is exhaustive by design.

## Faster-than-real-time / drop-free

- `replay` shells out to `echobox-replay`, which reuses the shipping
  DspPipeline + Recorder with a drop-free backpressured capture loop —
  no ALSA, no ring-buffer drops by construction.
- `truth` batches all files through one loaded BatDetect2 model.
  Auto-picks a CUDA device if visible; `--device cpu` forces CPU.
- Rough runtime on a Threadripper-class x86: ~14 s per source WAV for
  replay (≈4× real time), plus ~10 s per WAV for truth on CPU
  (~1× real time). A 585-file corpus is a nighttime job.

## Resampling caveat, explicit

BatDetect2 targets **256 kHz** (Net2DFast_UK_same). The recordings are
384 kHz. BatDetect2 resamples internally; the effective rate lands in
every `truth_manifest` row (`resample_hz` column) so the report never
hides it. The Echobox side does not resample — `echobox-replay` runs at
384 kHz end to end.

## What can't be validated by this tool

- **BatDetect2 accuracy on our 200 ms short clips.** BatDetect2 was
  trained on longer full-recording windows; recall on very short clips
  may itself be lossy. The report says so.
- **Device-vs-x86 numerical margins.** Files near a gate threshold can
  flip between the two builds — see caveat 2. Use the §4.2 tool
  (`tools/collection/verify_feature_parity.py`) when it matters.
- **Silent-detector FNs.** A bat call the base detector never triggered
  on doesn't produce a clip in either `accepted/` or `rejected/`, so it
  can't appear in FN. Caveat 3.

## Tests

```bash
pytest tools/session_screen/tests -q
```

Covers the scorer's per-clip overlap join, per-species grouping, CF
forced-review, the disagreements sort order, and the manifest schema
round-trip. The ML model itself isn't tested here — that's BatDetect2's
suite's job, and a fixture-free ML test would be flakier than useful.

## No `Co-Authored-By`

Per repo convention.
