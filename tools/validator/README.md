# Echobox Validator

A Python package for offline detector tuning and ALSA-loopback emulation, used
to develop, score, and sanity-check Echobox's detection algorithms on a Linux
dev machine without microphone hardware.

The validator does **not** carry any algorithm logic of its own. STFT, the
HPF biquad, and every detector are part of the production C++ build; the
Python side only plumbs them through `libechobox_validator.so` (a thin C API
shipped under `tools/validator/native/`). Algorithms are discovered at import
time by scanning a directory of plugin `.so` files, and each plugin
self-declares both its name and its tunable knobs. Adding or renaming a
detector in C++ requires zero Python changes.

| Front-end       | Entry point                       | Purpose                                                                                                                                                |
| --------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| GUI             | `python -m validator`             | Interactive: spectrogram + detection rectangles (offline mode) or ALSA Loopback (fake-mic). The parameter form is rebuilt dynamically from the active algorithm's tunable manifest. |
| Batch CLI       | `python -m validator.cli <cmd> …` | Headless: describe the algorithm, run on WAVs, score against ground truth, grid-search tunables, render overlay PNGs, or stream a WAV to ALSA loopback. |

## Getting started

The validator lives at `tools/validator/`. Run it from the `tools/` directory
so that `python -m validator` resolves to this package.

```bash
# Build the native lib + plugins first.
./build_dev.sh

# From the repo root:
cd tools

# GUI
../.venv/bin/python -m validator

# Batch CLI
../.venv/bin/python -m validator.cli describe
../.venv/bin/python -m validator.cli run path/to/recording.wav
../.venv/bin/python -m validator.cli --help
```

Or activate the venv once for the shell session and drop the prefix:

```bash
source .venv/bin/activate
cd tools
python -m validator
```

Runtime dependencies (already installed in `.venv/`): `numpy`, `scipy`,
`PyQt6`, `pyqtgraph`. The `overlay` CLI subcommand additionally needs
`matplotlib` (also already installed).

## Algorithm discovery

At import time, the wrapper calls `eb_init` on the directory pointed to by
`$ECHOBOX_ALGORITHMS_DIR` if set, otherwise `<repo>/deploy/bin/algorithms/`
(the same path the production binary walks in dynamic-plugin mode). Every
`.so` it finds is dlopened and registered by the name its `get_tracker_name`
entry point returns.

```bash
python -m validator.cli describe            # show registered algorithms + tunables
ECHOBOX_ALGORITHMS_DIR=/tmp/plugins python -m validator    # use a custom plugin dir
```

## Modes

### Offline detection (GUI mode 1 / CLI `run`, `score`, `grid`, `overlay`, `diagnose`)

Loads a WAV from disk, runs it through the production STFT + HPF, and feeds
each frame to the active detector. The GUI overlays detection rectangles on
the spectrogram and lists every event. The detector's tunables are editable
from the form on the same panel, so the same WAV can be re-run with different
settings without leaving the window. The form's fields are built from the
algorithm's own tunable manifest — switching algorithms in the dropdown
rebuilds the form.

### ALSA Loopback (GUI mode 2 / CLI `loopback`)

Feeds a WAV file into an ALSA loopback device (`snd-aloop`) so that the
production C++ binary captures it **exactly as if a real microphone were
plugged in**. This is used to exercise the full production path
(capture → DSP → recorder) on a Linux dev machine without hardware. It
requires the `snd-aloop` kernel module to be loaded:

```bash
sudo modprobe snd-aloop id=UltraMic384K
```

## CLI reference

```text
python -m validator.cli describe [--algorithm NAME]
python -m validator.cli run      file.wav [--algorithm NAME] [--sensitivity NAME] [--set KEY=VALUE]…
python -m validator.cli score    a.wav b.wav -v
python -m validator.cli grid     a.wav b.wav --sweep KEY V1 V2 V3 [--sweep …]
python -m validator.cli overlay  file.wav -o out.png
python -m validator.cli diagnose file.wav
python -m validator.cli loopback file.wav [--loop]
```

- `--algorithm NAME` selects which registered plugin to run with; omitted, the
  first registered one is used.
- `--sensitivity NAME` (**experimental**) applies a coarse preset before any
  individual `--set` overrides take effect. See `describe`'s "Sensitivity
  presets" section for the names the active algorithm recognises.
- `--set FIELD=VALUE` works on any `DetectorConfig` field (see
  `validator/native.py`) or any tunable the algorithm exposes. Example:
  `--set freq_lo_hz=22000 --set band_snr_threshold=10`.
- `--sweep KEY V1 V2 …` (grid only) sweeps an algorithm tunable across the
  given values. Pass `--sweep` multiple times for multi-axis sweeps. Tunable
  keys come from `describe`'s output.
- `diagnose` reports per-call causal peak SNR; it is specific to floor-based
  detectors (i.e. detectors that expose `alpha_rise`, `alpha_fall`, and
  `min_abs_floor` tunables) and will refuse to run against an algorithm that
  doesn't.

The GUI has an equivalent **Sensitivity (experimental)** dropdown above the
tunables form. Picking a preset loads its values into the spinboxes so you
can see (and tweak) exactly what changed.
