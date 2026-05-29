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

A saved file looks like this: `20260529_213544_842ms_45-80kHz.wav`

| Part        | Meaning                                                        |
| ----------- | -------------------------------------------------------------- |
| `20260529`  | Date — 29 May 2026                                             |
| `213544`    | Time — 21:35:44                                                |
| `842ms`     | How long the recorded activity lasted (842 milliseconds)       |
| `45-80kHz`  | The frequency range the calls fell into (45 to 80 kHz)         |

---

## Useful options

Run `./Echobox --help` to see the full list.

| Option              | What it does                                              | Default        |
| ------------------- | --------------------------------------------------------- | -------------- |
| `--device <name>`   | Which microphone to listen to                             | `default`      |
| `--output-dir <p>`  | Where recordings are saved                                | `./recordings` |
| `--preroll-ms <n>`  | How much audio to keep from *before* each call (ms)       | `1000`         |
| `--silence-ms <n>`  | Quiet time after a call before the recording closes (ms)  | `2000`          |
| `--freq-lo-hz <n>`  | Bottom of the frequency range to listen for               | `20000`        |
| `--freq-hi-hz <n>`  | Top of the frequency range to listen for                  | `190000`       |

---

## Troubleshooting

If something seems wrong, enable logging:

```bash
./run.sh --device plughw:CARD=UltraMic384K --log-level info
```

- **Device errors** — Double-check the `--device` name with `arecord -l`.
- **No recordings** — Ensure the mic supports 384 kHz and that ultrasonic activity is present.
- **Permission denied** — Add your user to the audio group: `sudo usermod -aG audio $USER`, then log out and back in.

---

## For developers

The build produces a standalone binary in `deploy/bin/`. By default the detector is statically linked. To use hot-swappable plugins, pass `-DECHOBOX_DYNAMIC_PLUGINS=ON` to the build script. Tools for offline tuning and simulation are in `tools/`.
