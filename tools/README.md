# tools/

Development-only helpers. **Nothing here is required to build, install, or run
LiteSpectrum on the Raspberry Pi** — end users can ignore this directory.

| Subdirectory     | What it is                                                                 |
| ---------------- | -------------------------------------------------------------------------- |
| [`validator/`](validator/) | Python GUI + CLI for offline detector tuning. Hosts a hand-maintained Python port of the C++ `BandEnergyDetector` for experimenting with detection thresholds against recorded WAVs. |
| [`fake-mic/`](fake-mic/)   | A virtual microphone (Linux `snd-aloop` loopback) with a small GUI that streams a WAV into the ALSA capture path, so the production binary can be exercised end-to-end without a real Ultramic plugged in. |

## Why these live in the same repo

The Python detector in `validator/detector.py` is a hand-maintained shadow of
the C++ `BandEnergyDetector` in `src/dsp/algorithms/BandEnergyDetector/`.
Keeping both sides in one repository means a change to either lands in the same
commit and PR, so the two can't silently drift apart.

## Build scope

The top-level CMake project does **not** descend into `tools/`; adding files
here never affects the production build. The Python tools run from a local
virtualenv (e.g. `pip install -r fake-mic/requirements.txt`).
