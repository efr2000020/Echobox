# Echobox tooling guide

Everything you can run in this repo, in the order you'd run them.

**Scope:** developer/operator onboarding. If you just want to record bats
on a Raspberry Pi, `README.md` is enough — this file is for people who
also want to *build*, *validate*, or *tune* the detector.

---

## 0. One-time prerequisites

### System packages (Debian / Raspberry Pi OS)

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake ninja-build \
    libasound2-dev libsndfile1-dev pkg-config
```

### Python virtualenv (only needed for the Python tools)

Echobox itself is a C++ project and ships **no `.venv/`** at the repo
root. The Python tools (`tools/validator/`, `tools/session_screen/`)
share a virtualenv at
`/home/slim/projects/LwPiBatRec/.venv` (per project convention). Any
recent CPython 3.11+ venv with each tool's `requirements.txt`
installed will work; substitute your own venv path if you host it
elsewhere.

```bash
# From your own scratch dir (NOT inside the Echobox tree):
python -m venv .venv
source .venv/bin/activate

# Then, from the Echobox repo root:
pip install -r tools/session_screen/requirements.txt
pip install -r tools/validator/requirements.txt
```

The `session_screen` requirements pull in `torch` + `batdetect2` (~2 GB
of downloads), so first-install is slow.

### ALSA device name

Find your ultrasonic mic's ALSA name — you'll need it for every
recording command:

```bash
arecord -l
```

Look for the mic in square brackets, e.g. `[UltraMic384K]`, then use
`plughw:UltraMic384K,1,0` or `plughw:CARD=UltraMic384K` as the
`--device` argument.

---

## 1. Build scripts (root of repo)

Three shell scripts. All run from repo root; the current working
directory matters because they build into `./build/` and install into
`./deploy/`.

### `build_and_deploy.sh` — production Release build

```bash
./build_and_deploy.sh
```

- **What:** Release build (`-O2 -DNDEBUG`) of `Echobox` only. Installs
  into `./deploy/bin/Echobox`.
- **Does NOT build:** `echobox-replay` (the offline tool), and does
  NOT enable dynamic plugins.
- **When to use:** shipping to a device, or any run that has to match
  what a field unit would do.

### `build_dev.sh [--replay]` — Debug build for local development

```bash
./build_dev.sh           # debug build, no replay tool
./build_dev.sh --replay  # debug build + echobox-replay
```

- **What:** Debug build (`-O0 -g`) with `ECHOBOX_DYNAMIC_PLUGINS=ON`.
  Installs into `./deploy/bin/Echobox` **plus** the algorithm plugins
  as shared libraries under `./deploy/bin/algorithms/`.
- **With `--replay`:** also builds `echobox-replay` at
  `./deploy/bin/echobox-replay`.
- **When to use:** any time you're going to run `tools/validator/`
  (the validator dlopens the algorithm plugins from
  `deploy/bin/algorithms/`, so it needs `ECHOBOX_DYNAMIC_PLUGINS=ON`)
  OR any time you need `echobox-replay`.

### `clean.sh` — wipe build + deploy

```bash
./clean.sh
```

Removes `./build/` and `./deploy/`. Run this before switching between
Release and Debug builds; CMake caches the old build type otherwise.

### One-off: manual Release with `echobox-replay`

Neither `build_and_deploy.sh` nor `build_dev.sh` produces a **Release**
build of `echobox-replay`. If you want the offline replay tool built
Release (faster, matches the ARM device more closely), invoke CMake
directly into a separate tree so `./deploy/` stays untouched:

```bash
cmake -S . -B build_release -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DECHOBOX_BUILD_REPLAY=ON \
      -DCMAKE_INSTALL_PREFIX="$(pwd)/deploy_release"
cmake --build   build_release -j$(nproc)
cmake --install build_release
# -> ./deploy_release/bin/{Echobox,echobox-replay}
```

`build_release/` and `deploy_release/` are gitignored.

---

## 2. Running the shipping binary

Two convenience scripts, both wrapping `./deploy/bin/Echobox`.

### `run.sh` — field-representative launch

```bash
./run.sh                                              # silent, prod defaults
ECHOBOX_DEVICE=plughw:CARD=UltraMic384K ./run.sh
./run.sh --log-level info                             # any flags pass through
./run.sh --save-rejected boundary                     # keep near-miss rejects
```

- **Prereq:** `./build_and_deploy.sh` (or the manual Release recipe).
- **Defaults:** whatever's baked into `src/app/Config.hpp` — currently
  10 ms preroll / 20 ms silence / 40 ms max, cricket filter on.
- **Passes flags verbatim** after the device: `./run.sh <flags>` ⇒
  `./deploy/bin/Echobox --device <auto> <flags>`.

### `run_debug.sh` — false-positive investigation

```bash
./run_debug.sh
./run_debug.sh --snr-threshold 8.0
```

- **Prereq:** `./build_and_deploy.sh` (uses the same `./deploy/bin/Echobox`).
- **What differs from `run.sh`:**
  - `--log-level debug` (per-frame diagnostics)
  - `--preroll-ms 3000` (well past the 40-frame warmup gate)
  - `--silence-ms 5000` (capture the full tail)
  - `--max-length-ms 60000` (60 s hard cap so sustained interferers
    aren't truncated)
  - Writes into `./recordings_debug/` + `./logs_debug/` so debug data
    doesn't mix with production captures.
- **When to use:** investigating a specific false-positive site, not
  representative of shipping behaviour.

### Running the binary directly (no wrapper)

```bash
./deploy/bin/Echobox --help
./deploy/bin/Echobox --device plughw:UltraMic384K,1,0 \
                     --output-dir ./recordings \
                     [other flags]
```

Every flag has a default matching the shipped `Config`; run with
`--help` to see them all. For the fully-explicit invocation, see the
end of this file.

---

## 3. Offline validation tools

Two Python packages under `tools/`. Both drive the **production** C++
DSP + detector — no Python re-implementation.

### 3a. `tools/session_screen/` — whole-session rough-recall screen

Rough per-clip / per-pass / per-file recall + cricket-FP against
BatDetect2 as a proxy for truth. Runs the shipping `DspPipeline +
Recorder` under `echobox-replay`, so what it measures is what a real
device would produce (modulo x86 vs ARM).

**Replay is deterministic** (since 2026-08-14). It used to feed the
pipeline as fast as the CPU allowed while the recorder measured
`--silence-ms` against the wall clock, so the same input gave a
different answer every run — 635 / 650 / 650 clips over three runs of
one 60 s file — and a 20 ms silence window became hundreds of ms of
audio, which meant clip boundaries were set by `--max-length-ms` alone.
`echobox-replay` now drives the recorder from a virtual clock advanced
by samples fed, in lockstep with the DSP thread, so byte-identical
input gives byte-identical output. Two consequences worth knowing:

- A/B deltas below ~5 % are now meaningful. Before this, they were
  inside the run-to-run noise floor, so any tuning conclusion drawn
  from a pre-2026-08-14 replay run is worth re-measuring.
- Clip counts went **up** versus old replay runs (the silence timeout
  actually fires now). Do not compare a new run against an old one.

**Prereqs (in order):**
1. Python venv with `tools/session_screen/requirements.txt` installed
   (see §0).
2. `echobox-replay` binary. Easiest: `./build_dev.sh --replay`. For a
   Release-build replay (faster, ARM-closer), use the manual Release
   recipe in §1 and point at it via `ECHOBOX_REPLAY_BIN`:
   ```bash
   export ECHOBOX_REPLAY_BIN=$(pwd)/deploy_release/bin/echobox-replay
   ```
3. A directory of `.wav` recordings (same-sample-rate mono).

**Subcommands** (`python -m tools.session_screen.validate <cmd> --help`):

| subcommand  | what it does                                                  | notes |
|-------------|---------------------------------------------------------------|-------|
| `truth`     | Runs BatDetect2 over every WAV, caches per-clip detections    | ~90 min on CPU for 585 files; auto-picks CUDA if visible |
| `replay`    | Runs `echobox-replay` over every WAV, writes per-clip manifest | 8-way sharding via `--jobs 8` cuts a 585-file night to ~10 min; also writes `run.json` for provenance |
| `score`     | Joins truth + replay, emits report + confusion CSVs           | Reads two parquet caches; no re-run |
| `all`       | `truth` → `replay` → `score` for one config                   | Sequences the three above |
| `sweep`     | Runs `shipping` + `shorter` configs side-by-side and diffs them | Shares one truth manifest across both |
| `followup`  | Adds per-file / per-pass recall + cricket-FP profile          | No re-run — reads existing manifests |
| `followup2` | Veto/provisional pool split + per-knob sweep                  | Needs v2-format sidecars (0.3.0-rc1+) |

**Named `--config` presets** (as of 2026-08):

| preset     | preroll_ms | silence_ms | max_length_ms | notes |
|------------|-----------|-----------|---------------|-------|
| `shipping` | 10        | 20        | 40            | Mirrors `src/app/Config.hpp` (0.4.0 short-clip). Use this to measure what the field device actually does. |
| `legacy`   | 50        | 50        | 200           | Pre-0.4.0 long-clip geometry. Was named `baseline` until 2026-08; renamed to stop it being mistaken for the shipping config. |
| `shorter`  | 20        | 40        | 200           | Historical intermediate; retained for continuity with older sweeps. |

Any explicit `--preroll-ms` / `--silence-ms` / `--max-length-ms` /
`--tunable` flag overrides the preset — presets are just a shorthand.

**Typical single-config run:**

```bash
source /home/slim/projects/LwPiBatRec/.venv/bin/activate

# All-in-one (uses defaults / plan's "baseline" preset):
python -m tools.session_screen.validate all \
    --input  path/to/wav-dir \
    --output-dir validation_out \
    --jobs 8
```

**Preferred: point at a dataset dir instead** — a *dataset* is any
directory containing `dataset.json`, a `reference/` of WAVs, and (once
built) `truth.parquet`. Passing `--dataset <path>` fills in `--input`
from `reference/`, uses (or writes) the dataset's own `truth.parquet`,
and drops replay outputs into a run-specific dir under sibling `runs/`
— so the reference data stays immutable and every replay lives in its
own timestamped folder:

```bash
python -m tools.session_screen.validate all \
    --dataset /mnt/data/datasets/session_02 \
    --config shipping --jobs 8
# → truth  read from /mnt/data/datasets/session_02/truth.parquet
# → outputs (+ run.json for provenance) written to
#   /mnt/data/runs/session_02__shipping__<yyyymmdd-hhmmss>/
```

The dataset layout used above (as of 2026-08):

```
/mnt/data/
├── datasets/<id>/
│   ├── dataset.json     # entry point for scripts + agents
│   ├── reference/*.wav  # BatDetect2 inputs
│   ├── truth.parquet    # BatDetect2 output (shared across all runs)
│   └── on_device/       # events/, decisions.jsonl, SESSION_*.json
├── runs/<id>__<tag>__<ts>/   # every replay output lands here
└── backups/                   # per-dataset .tar / .tar.gz snapshots
```

The truth ↔ replay join is by WAV **basename**, not absolute path — so
a dataset stays valid if you move `/mnt/data/` to a different mount
point. New `truth.parquet` / `replay_manifest.parquet` files write
basenames directly; older manifests can be rewritten in place with a
one-liner (`df["file"] = df["file"].map(os.path.basename); df.to_parquet(...)`).

**Reusing a cached truth manifest without `--dataset`** — the BatDetect2
pass is the expensive stage. Symlink the cache into the target output
dir before running `replay`/`sweep`/`all`:

```bash
mkdir -p validation_out
ln -s /absolute/path/to/cached/truth_manifest.parquet \
      validation_out/truth_manifest.parquet
```

**Detector-tunable overrides in a replay run** (added in the 0.4.0
release cycle):

```bash
python -m tools.session_screen.validate replay \
    --input path/to/wav-dir --output-dir validation_out/experiment \
    --cricket-filter on --tunable max_flatness=0.80 \
    --jobs 8
```

Repeatable — pass `--tunable KEY=VALUE` any number of times.

Full details: `tools/session_screen/README.md`.

### 3b. `tools/validator/` — precise per-clip tuning + GUI

Curated-corpus per-event precision/recall + grid-search over tunables,
plus a GUI that overlays detections on a spectrogram. This is for
tuning **one detector on one recording**, not for measuring whole
sessions.

**Prereqs:**
1. Python venv with `tools/validator/requirements.txt` installed
   (`PyQt6`, `pyqtgraph`, `numpy`, `scipy`; `matplotlib` for the
   `overlay` CLI subcommand).
2. **`./build_dev.sh`** (must be `--dev`, i.e. dynamic plugins ON —
   the validator dlopens the algorithms from `./deploy/bin/algorithms/`).

**Run from the `tools/` directory** so `python -m validator` resolves:

```bash
./build_dev.sh                         # from repo root, once
cd tools

# GUI:
../.venv/bin/python -m validator

# Batch CLI:
../.venv/bin/python -m validator.cli describe        # list algorithms + tunables
../.venv/bin/python -m validator.cli run recording.wav
../.venv/bin/python -m validator.cli --help          # see all subcommands
```

Substitute your own venv path (e.g.
`/home/slim/projects/LwPiBatRec/.venv/bin/python`) if you host the
venv outside the repo tree.

Full details: `tools/validator/README.md`.

---

## 4. Recap — "I want to do X, what do I run?"

| Goal | Steps |
|------|-------|
| **Record bats on a Pi** | `./build_and_deploy.sh` → `./run.sh --device <mic>` |
| **Investigate a false positive** | `./build_and_deploy.sh` → `./run_debug.sh --device <mic>` |
| **Field observability of rejected clips** | `./build_and_deploy.sh` → `./run.sh --device <mic> --save-rejected boundary` |
| **Score one recording against BatDetect2** | `./build_dev.sh --replay` → `python -m tools.session_screen.validate all --input <dir> --output-dir <dir>` |
| **Score a dataset against BatDetect2** | `./build_dev.sh --replay` → `python -m tools.session_screen.validate all --dataset <path> --config shipping` (auto-derives `runs/<id>__shipping__<ts>/`) |
| **Sweep short-clip geometries** | Manual Release recipe (§1) → set `ECHOBOX_REPLAY_BIN` → invoke `validate replay` per-config |
| **Interactive tuning on a single WAV** | `./build_dev.sh` (no `--replay` needed) → `cd tools && python -m validator` |
| **Compare two detector-tunable settings** | `./build_dev.sh --replay` → `validate replay --tunable KEY=VALUE` twice with different values |
| **Reset to a known state** | `./clean.sh` then repeat the appropriate build |

Order-of-operations rule of thumb:

```
        build_and_deploy.sh    (Release, shipping)
       ├── run.sh
       └── run_debug.sh

        build_dev.sh           (Debug, dynamic plugins)
       ├── tools/validator/    (needs plugins)
       └── build_dev.sh --replay  ─┐
                                    ├──► tools/session_screen/
        manual Release (§1)  ──────┘   (needs echobox-replay)
```

---

## 5. Fully-explicit shipping-defaults launch

For the record — every option spelled out with its current default. If
the customer runs with no flags, this is the effective invocation:

```bash
./deploy/bin/Echobox \
    --device plughw:UltraMic384K,1,0 \
    --sample-rate 384000 \
    --algorithm BandEnergyDetector \
    --fft-size 4096 \
    --hop-size 512 \
    --freq-lo-hz 20000 \
    --freq-hi-hz 192000 \
    --snr-threshold 8.0 \
    --output-dir ./recordings \
    --preroll-ms 10 \
    --silence-ms 20 \
    --min-length-ms 0 \
    --max-length-ms 40 \
    --cricket-filter on \
    --save-rejected off \
    --save-rejected-sample-n 500 \
    --save-rejected-max-per-hour 200 \
    --log-dir ./logs \
    --log-level off \
    --console-log on \
    --heartbeat-sec 60
```

Notes:
- `--save-rejected-sample-n 500` is consulted only when the mode is
  `sample`; harmless otherwise.
- Swap `boundary` → `sample` for a random 1-in-N cross-section
  instead of near-threshold near-misses. `boundary` is the
  highest-value field mode per `tools/session_screen/README.md`.
