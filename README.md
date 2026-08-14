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
| `--silence-ms <n>`    | Quiet time after a call before the recording closes (ms). With the cricket filter on (the default) it must be ≥ a derived floor (~16 ms at 384 kHz / hop-512); the default `20` already clears it. | `20`           |
| `--min-length-ms <n>` | Drop any recording shorter than this (`0` = off)          | `0`            |
| `--max-length-ms <n>` | Close a recording as soon as it reaches this length (`0` = no cap) | `40`   |
| `--freq-lo-hz <n>`    | Bottom of the frequency range to listen for               | `20000`        |
| `--freq-hi-hz <n>`    | Top of the frequency range to listen for (cannot exceed Nyquist of your mic's `--sample-rate`) | `192000`       |
| `--snr-threshold <x>` | SNR a frame must clear to count as a detection (see below) | `8.0`          |
| `--cricket-filter on\|off` | Reject weak, broadband, low-frequency events before they become WAVs. On by default; costs ~1 % of calls and removes ~27 % of junk (see [Cricket filter](#cricket-filter) below) | `on` |

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

Two behaviours to be aware of:

1. A multi-call bat pass becomes **several one-call clips** instead of a
   single grouped WAV — presence is still preserved because each call
   yields its own clip.
2. Because the cricket filter now ships **off** (see below), nothing is
   rejected on-device: expect a lot of files on the SD card, including
   cricket clips, and plan the downstream classifier pass accordingly.
   Roughly 150 k–240 k clips per night were recorded on the two
   reference sessions, at 4.0–6.6 GB.

If you turn the filter back on with `--cricket-filter on`, its decision
still runs on live FFT frames, so shorter clips do not weaken it — a
cricket sequence that used to become one long rejected clip becomes
several short rejected clips instead (visible only with
`--save-rejected`). Total rejected clip count is larger; total bytes are
smaller.

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
| `6.0`       | Very quiet sites where you want every last faint call and can afford the storage. Recall gain over `8.0` is small (the curve has flattened by here) but the extra recording is not. |
| `8.0`       | Default. Catches ~97 % of the calls a reference classifier finds, at a detector duty cycle of ~19 %. |
| `12.0`      | Previous default. Noticeably deafer to faint, distant calls (~86–91 % on the same corpus) but writes about a quarter less audio. |
| `18.0`      | Windy, suburban, near-roadway, or rustling-foliage sites. Strict; only loud unambiguous calls survive. Use this if you see lots of false positives. |

**The trade-off is storage.** Lowering this threshold makes the
detector open more events, so it writes more clips and more bytes per
night. The default moved from `12.0` to `8.0` because the recall gain
was large (roughly +6 percentage points of per-call recall) and the
extra recording was affordable on the reference deployment — but if
your unit is tight on SD card or battery, `12.0` is a reasonable and
well-understood setting to go back to.

Examples:

```bash
./run.sh --device plughw:CARD=UltraMic384K --snr-threshold 18.0
./run.sh --device plughw:CARD=UltraMic384K --snr-threshold 6.0
```
---

## Cricket filter

Sites with active crickets can easily fill an SD card with junk
recordings before bats show up. Echobox ships a filter for this, **on by
default**. It rejects an event only when all three of these are true:

- **weak** — trigger SNR below 12
- **broadband** — spectral flatness above 0.55
- **low-band** — the event won in the 20–45 kHz sub-band

Anything that looks bat-like on even one of the three is kept. The
rejection itself happens in the recorder: a clip whose window saw no
kept event is discarded before it becomes a WAV.

Measured on two full field nights against a reference classifier:

| | filter on | filter off |
| --- | ---: | ---: |
| Night 1 — calls captured | **97.3 %** | 98.4 % |
| Night 2 — calls captured | **98.1 %** | 99.1 % |
| Junk clips removed | ~27 % | 0 % |

So the filter costs about one call in a hundred and removes about a
quarter of the junk.

### What this filter actually is

Read this before relying on it at a cricket-heavy site.

On both reference nights, the non-bat recordings were mostly **weak
broadband noise, not tonal cricket chirps**. So what has been *measured*
is a weak-broadband-trigger filter. It should help with crickets — they
are usually quiet and low-frequency — but that has not been proven,
because neither night contained a stretch where crickets dominated.

Closing that gap needs a field recording from a genuinely cricket-heavy
site. Until then, treat the cricket performance as untested.

**An earlier version of this filter was far worse**, and the reason is
worth knowing if you tune it. It asked "can I prove this is a bat?" and
binned anything it could not prove — which cost about three out of every
four real bat calls (26 % captured, vs 98 % with it off). A filter has
to reject only what it can positively identify, and keep whatever it is
unsure about.

To turn it off:

```bash
./run.sh --device plughw:CARD=UltraMic384K --cricket-filter off
```

That buys roughly 1 % more calls and keeps ~27 % more junk.
`--save-rejected` requires the filter to be **on**, since with it off
nothing is ever rejected and `rejected/` would stay empty.

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
