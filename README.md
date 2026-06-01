# Echobox

Echobox listens to an ultrasonic microphone and automatically saves a
sound recording every time a bat calls. It runs quietly in the background:
when bats are active it writes `.wav` files; when they're not, it does nothing.
Each recording includes a short lead-in *before* the call, so you never miss
the start of a pass.

It's designed to run unattended on a small computer (such as a Raspberry Pi Zero 2 W)
out in the field, powered by a battery.

---

## Getting Started

### Step 1 — Install Dependencies

You'll need a C++20 compiler and several development libraries. On Raspberry Pi
OS (or any Debian-based system), install them with:

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    libasound2-dev \
    libsndfile1-dev \
    pkg-config
```

### Step 2 — Build Echobox

Run the build script from the project root to compile and install the program:

```bash
./build_and_deploy.sh Release
```

This creates a `deploy/` folder containing the compiled `Echobox` binary. You can find it at `deploy/bin/Echobox`.

### Step 3 — Find your microphone name

Plug in your microphone and run:

```bash
arecord -l
```

Look for your microphone's **card name** in square brackets (e.g., `[UltraMic384K]`). You build the device name Echobox needs like this: `plughw:CARD=UltraMic384K`.

### Step 4 — Run Echobox

You can run it directly from the project root using the convenience script:

```bash
./run.sh --device plughw:CARD=UltraMic384K
```

Or run the binary from the `deploy` folder:

```bash
cd deploy/bin
./Echobox --device plughw:CARD=UltraMic384K
```

Recordings are saved into a `recordings` folder, organised into a sub-folder per day. To stop, press **Ctrl + C**.

---

## Understanding the recording file names

A saved file looks like this: `20260529_213544842_18ms.wav`

| Part         | Meaning                                                        |
| ------------ | -------------------------------------------------------------- |
| `20260529`   | Date — 29 May 2026                                             |
| `213544842`  | Time — 21:35:44.842 (millisecond precision)                    |
| `18ms`       | How long the recording lasted (18 milliseconds, end to end)    |

---

## Useful options

Run `./Echobox --help` to see the full list.

| Option                | What it does                                              | Default        |
| --------------------- | --------------------------------------------------------- | -------------- |
| `--device <name>`     | Which microphone to listen to                             | `default`      |
| `--output-dir <p>`    | Where recordings are saved                                | `./recordings` |
| `--preroll-ms <n>`    | How much audio to keep from *before* each call (ms)       | `1000`         |
| `--silence-ms <n>`    | Quiet time after a call before the recording closes (ms)  | `2000`         |
| `--min-length-ms <n>` | Drop any recording shorter than this (`0` = off)          | `0`            |
| `--max-length-ms <n>` | Close a recording as soon as it reaches this length (`0` = no cap) | `5000`  |
| `--freq-lo-hz <n>`    | Bottom of the frequency range to listen for               | `20000`        |
| `--freq-hi-hz <n>`    | Top of the frequency range to listen for                  | `192000`       |
| `--snr-threshold <x>` | SNR a frame must clear to count as a detection (see below) | `12.0`         |

Lengths above are end-to-end (pre-roll + detected activity + silence-after).
Echobox refuses to start if `--max-length-ms` is smaller than
`--preroll-ms + --silence-ms`, so a recording can never close before the
detected call has had a chance to play out.

---

## Tuning for your site

The detector compares signal energy against an adaptive noise floor and only
calls a frame "hot" when that ratio clears `--snr-threshold`. Lower values
catch fainter calls but let more false positives through; higher values are
strict and only loud unambiguous calls survive.

| Setting     | When to use                                                                  |
| ----------- | ---------------------------------------------------------------------------- |
| `8.0`       | Low-noise sites (sheltered garden, rural attic). Maximum recall; tolerates more false positives in exchange for catching faint distant calls. |
| `12.0`      | Default. Suits a typical unattended deployment.                              |
| `18.0`      | Windy, suburban, near-roadway, or rustling-foliage sites. Strict; only loud unambiguous calls survive. Use this if you see lots of false positives. |

Examples:

```bash
./run.sh --device plughw:CARD=UltraMic384K --snr-threshold 18.0
./run.sh --device plughw:CARD=UltraMic384K --snr-threshold 8.0
```
---

## Troubleshooting

Logging is **off by default** — a shipped unit writes nothing to `./logs/` so
the SD card sees no logging I/O at all. Opt in when you're diagnosing
something:

```bash
./run.sh --device plughw:CARD=UltraMic384K --log-level info
./run.sh --device plughw:CARD=UltraMic384K --log-level debug   # per-frame detail
```

`debug` captures the detector's per-frame reasoning around each saved WAV and
is the right level when chasing false positives, but it writes several hundred
thousand lines per day — don't leave a long-running field unit on `debug`.

- **Device errors** — Double-check the `--device` name with `arecord -l`.
- **No recordings** — Ensure the mic supports 384 kHz and that ultrasonic activity is present.
- **Permission denied** — Add your user to the audio group: `sudo usermod -aG audio $USER`, then log out and back in.

---

## For developers

The build produces a standalone binary in `deploy/bin/`. By default the detector is statically linked. To use hot-swappable plugins, pass `-DECHOBOX_DYNAMIC_PLUGINS=ON` to the build script. Tools for offline tuning and simulation are in `tools/`.
