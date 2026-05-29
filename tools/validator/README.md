# LiteSpectrum Validator

A single Python package that replaces the old `spectrumValidator.py` (which
streamed audio to a special C++ "dev TCP" mode) and `tracker_ref.py` (which
was an offline port of the obsolete `SweepBlobTracker`). Both files are gone.

The validator now has two front-ends, both backed by the same in-process
Python port of `BandEnergyDetector`:

| Front-end                   | Entry point                          | Purpose |
| --------------------------- | ------------------------------------ | ------- |
| GUI                         | `python -m validator`                | Interactive: spectrogram + annotations (Mode 1) or ESP32 streaming (Mode 2). |
| Batch CLI                   | `python -m validator.cli <cmd> …`    | Headless: tune parameters, score against ground truth, grid-search, render overlay PNGs, stream a WAV to an ESP32. |

## Getting started

The validator lives at `tools/validator/`. Run it from the `tools/`
directory so that `python -m validator` resolves to this package.

```bash
# From the repo root:
cd tools

# GUI
../.venv/bin/python -m validator

# Batch CLI
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

## Modes

### Mode 1 — Offline detection

Loads a WAV from disk, runs the Python port of `BandEnergyDetector` against
it (no C++ binary, no network), and displays the spectrogram with annotation
rectangles. The detector's runtime parameters (`band_snr_threshold`,
flatness range, frequency window, debounce/hangover, …) are editable from
the form on the same panel, so you can re-run with different settings
without leaving the window.

The same Python detector lives in `tools/validator/detector.py` and is a
**line-for-line port** of the C++ `BandEnergyDetector::processFrame`. If you
change either side, mirror the change here so tuning results stay valid.

### Mode 2 — ESP32 streaming

Streams a WAV file to a networked ESP32 / ESP32-S3 that is wired to act as
the microphone for the production binary on the Pi. The Pi runs LiteSpectrum
normally (`--audio-source=alsa` with the ESP32 enumerated as the capture
device), and the GUI simply pushes audio at real-time rate over TCP. This
exercises the full production data path — capture → DSP → recorder → WAV
output — without needing an Ultramic plugged in.

The Python side is the only component this repo provides; the ESP32 firmware
is implemented separately. Use the wire format below.

## CLI reference

```text
python -m validator.cli run     file.wav [--set band_snr_threshold=10] …
python -m validator.cli score   a.wav b.wav -v
python -m validator.cli grid    a.wav b.wav --band-snr-threshold 8 10 12 15
python -m validator.cli overlay file.wav -o out.png
python -m validator.cli diagnose file.wav
python -m validator.cli stream  file.wav --host 192.168.1.42 [--port 4444]
```

`--set FIELD=VALUE` works on any `DetectorConfig` field (see
`validator/detector.py`). For example: `--set alpha_rise=0.99
--set hangover_frames=12`.

## ESP32 wire protocol (canonical reference for the firmware)

Plain TCP, single client at a time, **little-endian** throughout.

```
 offset  size  field
 ------  ----  ----------------------------------------------------
   0      4    magic            uint32  = 0xBA7DEC70
   4      4    sample_rate      uint32  (Hz, e.g. 384000)
   8      2    channels         uint16  (1 = mono; only 1 supported today)
  10      2    bits_per_sample  uint16  (16 = S16_LE; only 16 supported today)
  12      4    total_samples    uint32  (declared count of samples to follow;
                                         0 = open-ended stream)
 ------------- 16-byte header ----------------------------------------
  16    ...    audio payload    raw int16 LE samples, mono interleaved
```

The Python side opens a TCP connection to the ESP32's listening port, sends
the 16-byte header once, then streams the audio payload in `~chunk_ms`-sized
chunks. Each chunk is paced to real-time playback rate (the streamer sleeps
to the wall-clock time at which playback of the chunk would finish) so the
ESP32 never has to buffer more than one chunk worth of data at a time.

End-of-stream is signalled either by reaching `total_samples` or by the
Python side closing the TCP connection.

### Firmware responsibilities

1. Bind a TCP listening socket on a fixed port (default `4444`; the host
   GUI / CLI lets the user override).
2. On accept: read exactly **16 bytes**; validate `magic`, `sample_rate`,
   `channels == 1`, `bits_per_sample == 16`.
3. Loop: `recv(buf, N)` and forward the bytes onto the audio interface the
   ESP32 is presenting (USB-audio gadget, I2S DAC into a USB-audio bridge,
   etc.). A small ring buffer (≈ 4–8 × chunk size) is sufficient because the
   host paces sends to real time.
4. On disconnect, close any open audio interface and return to step 1.

The constants are also exported from `validator/esp32_stream.py`
(`MAGIC`, `DEFAULT_PORT`, `HEADER_FORMAT`, `HEADER_SIZE`) so test fixtures
can import them directly.
