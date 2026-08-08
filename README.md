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

A saved file looks like this: `20260529_213544842.wav`

| Part         | Meaning                                                        |
| ------------ | -------------------------------------------------------------- |
| `20260529`   | Date — 29 May 2026                                             |
| `213544842`  | Time — 21:35:44.842 (millisecond precision)                    |

---

## Useful options

Run `./Echobox --help` to see the full list.

| Option                | What it does                                              | Default        |
| --------------------- | --------------------------------------------------------- | -------------- |
| `--device <name>`     | Which microphone to listen to                             | `default`      |
| `--output-dir <p>`    | Where recordings are saved                                | `./recordings` |
| `--preroll-ms <n>`    | How much audio to keep from *before* each call (ms)       | `10`           |
| `--silence-ms <n>`    | Quiet time after a call before the recording closes (ms). With `--cricket-filter on` it must be ≥ a derived floor (~16 ms at 384 kHz / hop-512); `--cricket-filter off` removes the floor. | `20`           |
| `--min-length-ms <n>` | Drop any recording shorter than this (`0` = off)          | `0`            |
| `--max-length-ms <n>` | Close a recording as soon as it reaches this length (`0` = no cap) | `40`   |
| `--freq-lo-hz <n>`    | Bottom of the frequency range to listen for               | `20000`        |
| `--freq-hi-hz <n>`    | Top of the frequency range to listen for (cannot exceed Nyquist of your mic's `--sample-rate`) | `192000`       |
| `--snr-threshold <x>` | SNR a frame must clear to count as a detection (see below) | `12.0`         |
| `--cricket-filter on\|off` | Reject cricket-like signals before they become WAVs (see [Cricket filter](#cricket-filter) below) | `on` |

Lengths above are end-to-end (pre-roll + detected activity + silence-after).
Echobox refuses to start if `--max-length-ms` is smaller than
`--preroll-ms + --silence-ms`, so a recording can never close before the
detected call has had a chance to play out.

### Short-clip recording (default)

The shipped defaults produce **one clip per bat call, hard-capped at
40 ms end-to-end** (10 ms pre-roll + up to ~10 ms detected call + 20 ms
trailing silence). The customer runs BatDetect2 daily on a
solar-powered, resource-limited device, so total audio bytes per night
is the currency that matters — the 40 ms cap cuts overnight storage
by roughly half against the previous 200 ms cap while measurably
improving presence recall on the reference corpus.

The cricket-filter decision still runs on live FFT frames, so shorter
clips do not weaken it. Three behaviours to be aware of:

1. A multi-call bat pass becomes **several one-call clips** instead of a
   single grouped WAV — presence is still preserved because the surviving
   bat-like event yields at least one clip.
2. A cricket sequence that used to become one long rejected clip now
   becomes **several short rejected clips** (still discarded by the
   gate; visible only if you turn on `--save-rejected`). Total clip
   count on `rejected/` is larger; total bytes are smaller.
3. `--cricket-filter off` disables the on-device cricket rejection, so
   expect more files on the SD card / downstream classifier when running
   with the filter off.

### Long-pass profile (opt-in)

To restore R1/R2v2-style grouping (one WAV per pass, up to 5 s), pass:

```bash
./run.sh --device plughw:CARD=UltraMic384K \
         --preroll-ms 1000 --silence-ms 2000 --max-length-ms 5000
```

Existing deployments upgrading from R1 or R2v2 that rely on the old
1000/2000/5000 defaults should either adopt the short-clip behaviour or
pin the long-pass profile explicitly.

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

## Cricket filter

Sites with active crickets can easily fill an SD card with cricket
recordings before bats show up. Echobox recognises the shape of a
cricket chirp — narrow bandwidth, a metronomic repetition pattern — and
rejects it in two places:

- The **detector** downgrades cricket-shaped events during frame
  processing, so they never open a recording in the first place.
- The **recorder** discards any clip whose window saw no bat-like event,
  so an isolated event that survives the detector still gets dropped
  before it becomes a WAV.

The filter is **on by default** and requires no configuration.

If a deployment site produces bat calls the filter can't recognise, turn
it off — one flag disables both halves:

```bash
./run.sh --device plughw:CARD=UltraMic384K --cricket-filter off
```

With `--cricket-filter off` the recorder behaves as if the filter were
never present.

### Temporal rep-guard (advanced) — OFF by default as of 0.3.0-rc1

Sitting inside the cricket filter is a secondary check called the
**temporal repetition-rate guard**: it looks at the timing of recent
events and vetoes an otherwise-passing sweep when surrounding onsets
form a metronomic pattern. On the Pipistrellus validation corpus we
measured that this guard was discarding about **2,500 real bat calls
per night** that our own sweep-shape gate had already accepted —
enough that it's no longer worth its cricket-rejection contribution
on typical Pipistrellus deployments.

As of the veto-recovery pre-release (**0.3.0-rc1**) the temporal
rep-guard defaults to **off**. The measured effect, on our reference
corpus:

- **+~2,500 BatDetect2-visible bat calls recovered per night** (calls
  the recorder previously discarded near cricket-like onset patterns).
- **+~14% output volume** (more `.wav` files reach the SD card and,
  downstream, whatever the operator feeds them into).
- **~30% relative increase in cricket-shaped false positives**
  (129 → ~168 leaked cricket clips per night on this corpus).

The guard is **retained in the codebase** — it's a boolean tunable, not
a code deletion. Sites that see metronomic-calling species (notably
**Rhinolophus / horseshoe** and **Nyctalus** CF species, on which we
have no validation data) may want to turn it back on:

```
# In your Echobox tunables / session-header:
rep_guard_enabled = 1
```

`rep_guard_enabled=1` restores the pre-0.3.0 temporal-veto behaviour
(the veto's decision logic is unchanged; validation observed ~1%
event-count drift between the CLI-override path and the pre-flip
code-default path on x86 fast-math builds — re-check on ARM before
treating it as bit-exact at a new site).

**Caveats you should read before shipping this default to a new site:**

- The recovery number was measured on **x86 with `-ffast-math`**; the
  device is ARM. Verify per-site with
  `tools/collection/verify_feature_parity.py`. The on/off is a boolean
  so it's arch-robust, but the leak/recovery magnitudes should be
  confirmed on hardware before a shipping decision at a new site.
- The measurements come from a **BatDetect2 screen** and cover
  **Pipistrellus only** — no CF/QCF species are present on our
  reference corpus. The rep-guard exists specifically to reject
  metronomic patterns, which is the mechanism most likely to matter at
  Rhinolophus/Nyctalus sites. If you're deploying at a CF-heavy site,
  turn it back on until you've collected a per-site validation set.
- The measurements are against a screen, not human ground truth.

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

When logging is on, Echobox also mirrors each record to `stderr` in a
human-readable form (colourised on a real terminal) and emits an
`app: HEARTBEAT …` line every 60 s that carries uptime, kept-event
count, free disk, available memory, and any DSP overflow count. Both
are useful when SSH'd into a running unit. Adjust or disable with:

| Option                   | What it does                                       | Default |
| ------------------------ | -------------------------------------------------- | ------- |
| `--console-log on\|off`  | Mirror log records to `stderr` while logging is on | `on`    |
| `--heartbeat-sec <n>`    | Seconds between `app: HEARTBEAT` lines (`0` = off) | `60`    |

- **Device errors** — Double-check the `--device` name with `arecord -l`.
- **No recordings** — Ensure the mic actually captures the ultrasonic range you've configured (`--sample-rate` set to match, `--freq-hi-hz` no higher than its Nyquist) and that ultrasonic activity is present.
- **Permission denied** — Add your user to the audio group: `sudo usermod -aG audio $USER`, then log out and back in.

---

## For developers

The build produces a standalone binary in `deploy/bin/`. By default the detector is statically linked. To use hot-swappable plugins, pass `-DECHOBOX_DYNAMIC_PLUGINS=ON` to the build script. Tools for offline tuning and simulation are in `tools/`.

### Offline replay

`./build_dev.sh --replay` also builds `deploy/bin/echobox-replay`, a workstation-only
tool that streams a directory of WAV recordings through the same DspPipeline + Recorder
the shipping binary uses, so a corpus of field recordings produces the same
`accepted/` + `rejected/` layout the device would. Plain `./build_dev.sh` is unchanged
and the shipping binary is byte-identical either way — none of the replay code lives
in `src/`.

```sh
deploy/bin/echobox-replay \
  --input  field_samples/collection_08-04-2026/collection/reference \
  --output ./replay_out \
  --save-rejected all
```

If a `SESSION_HEADER.json` sits in `--input` (or a parent directory), replay
uses its capture config (preroll, silence, thresholds, freq window, etc.) as
the baseline; CLI flags still override. Every run also writes
`<output>/replay_manifest.json` recording the platform (`x86-replay`), build
type, effective config, and where that config came from — the scoring pipeline
should read it to distinguish device runs from replay runs.

Caveat: replay is x86 (Release adds `-ffast-math`); the device is ARM. Feature
values are a tuning proxy, not device-exact.

---

## License

Echobox is released under the **GNU General Public License v3.0 or later**
(GPL-3.0-or-later).

You are free to use, study, modify, and redistribute it, provided that any
derivative work is also released under the GPL. See [LICENSE](LICENSE) for
the full text and [AUTHORS](AUTHORS) for the list of contributors.

Copyright (C) 2026 The Echobox Authors.
