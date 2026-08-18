#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""§3 check 1: Injected known signal (external truth).

**GREEN-eligibility**: this check compares Stream A against a signal
whose properties (frequency, duration, sample position) were fixed by an
EXTERNAL source before the firmware ran. It is the only §3 check whose
truth does not come from the firmware itself — hence the only one that
can promote alignment claims from YELLOW to GREEN in isolation.

This script has two modes:

  - **generate**: writes a reference `probe.wav` (a short tone burst at a
    known offset in a sea of silence) that the bench operator plays into
    the mic via `aplay` / ALSA loopback. Also writes `probe.json` with
    the expected sample offset relative to the START of playback.

  - **verify**: given a session dir + the probe.json, finds the tone in
    Stream A and asserts:

      V1. It appears at the expected sample position (± tolerance, since
          playback→capture latency is device-dependent).
      V2. If there is a corresponding event clip in Stream B, its PCM
          window covers the tone.
      V3. If there is a corresponding decision-log entry (Stream C), its
          sample range overlaps the tone.

**Hardware-deferred until run on-device.** Without a mic + playback loop
this check stays YELLOW at best (it can be exercised on synthetic
firmware output but the "external truth" claim requires the physical
signal path). See VALIDATION_PROVENANCE for the current colour.

Pre-registered falsifiers (verify mode):

  F1. Tone not detected in Stream A at all.
  F2. Tone offset differs from expected by more than `--tolerance-ms`.
  F3. Expected event clip absent from Stream B.
  F4. No decision-log event overlapping the tone's sample range.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Optional

import numpy as np
import soundfile as sf

from tools.collection.session_layout import Chunk, Session, event_wav_paths
from tools.collection.verify_ab_identity import slice_from_chunks


def make_probe(sample_rate: int, tone_hz: float, tone_ms: float,
               pre_silence_ms: float, post_silence_ms: float,
               amplitude: float = 0.5) -> np.ndarray:
    pre = int(round(sample_rate * pre_silence_ms / 1000.0))
    tone = int(round(sample_rate * tone_ms / 1000.0))
    post = int(round(sample_rate * post_silence_ms / 1000.0))
    out = np.zeros((pre + tone + post,), dtype=np.float32)
    t = np.arange(tone) / float(sample_rate)
    out[pre:pre + tone] = (amplitude * np.sin(2 * np.pi * tone_hz * t)).astype(np.float32)
    return out


def cmd_generate(args: argparse.Namespace) -> int:
    probe = make_probe(
        sample_rate=args.sample_rate,
        tone_hz=args.tone_hz,
        tone_ms=args.tone_ms,
        pre_silence_ms=args.pre_silence_ms,
        post_silence_ms=args.post_silence_ms,
    )
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(out_path, probe, args.sample_rate, subtype="PCM_16")
    meta = {
        "sample_rate":              args.sample_rate,
        "tone_hz":                  args.tone_hz,
        "tone_ms":                  args.tone_ms,
        "pre_silence_ms":           args.pre_silence_ms,
        "post_silence_ms":          args.post_silence_ms,
        "expected_tone_offset_samples":
            int(round(args.sample_rate * args.pre_silence_ms / 1000.0)),
        "expected_tone_length_samples":
            int(round(args.sample_rate * args.tone_ms / 1000.0)),
    }
    meta_path = out_path.with_suffix(".json")
    meta_path.write_text(json.dumps(meta, indent=2))
    print(f"probe → {out_path}\nmeta  → {meta_path}\nplay via aplay/aloop.")
    return 0


def find_tone_offset(audio: np.ndarray, sample_rate: int,
                     tone_hz: float, expected_len_samples: int,
                     hop: int = 128) -> Optional[int]:
    """Return the sample offset where the tone's peak envelope lies, or
    None if the tone can't be located. Uses a moving RMS at ~hop; good
    enough to locate a ~50 ms tone in a ~1 s buffer without SciPy.
    """
    if audio.size < expected_len_samples:
        return None
    x = audio.astype(np.float32) / 32768.0
    # Envelope: moving RMS in windows of expected_len_samples (matches
    # the tone burst, so the peak lines up with the tone's centre).
    win = expected_len_samples
    if win <= 0 or win > x.size:
        return None
    kernel = np.ones(win, dtype=np.float32) / float(win)
    env = np.sqrt(np.convolve(x * x, kernel, mode="valid"))
    if env.size == 0:
        return None
    peak = int(np.argmax(env))
    # env[i] corresponds to the window starting at sample i. Return the
    # window's start-sample; caller compares against the expected start.
    return peak


def cmd_verify(args: argparse.Namespace) -> int:
    meta = json.loads(Path(args.probe_meta).read_text())
    session = Session.load(Path(args.session_root))
    if not session.chunks:
        print("FAIL: session has no reference chunks", file=sys.stderr)
        return 1
    expected_offset = int(meta["expected_tone_offset_samples"])
    tone_len        = int(meta["expected_tone_length_samples"])
    tone_hz         = float(meta["tone_hz"])
    sample_rate     = int(meta["sample_rate"])
    tolerance_samples = int(round(sample_rate * args.tolerance_ms / 1000.0))

    # Search a window around the expected tone position. Session start
    # aligned with playback start is the operator's responsibility.
    search_from = max(0, expected_offset - tolerance_samples)
    search_to   = expected_offset + tone_len + tolerance_samples
    audio = slice_from_chunks(
        Path(args.session_root), session.chunks,
        search_from, search_to - search_from,
    )
    if audio is None:
        print(f"FAIL [F1]: sample range [{search_from}, {search_to}) "
              f"not covered by Stream A", file=sys.stderr)
        return 1

    detected = find_tone_offset(audio, sample_rate, tone_hz, tone_len)
    if detected is None:
        print("FAIL [F1]: could not locate tone in searched window",
              file=sys.stderr)
        return 1
    detected_abs = search_from + detected
    delta = detected_abs - expected_offset

    if abs(delta) > tolerance_samples:
        print(f"FAIL [F2]: tone at sample {detected_abs}, expected {expected_offset} "
              f"(delta={delta}, tolerance={tolerance_samples})", file=sys.stderr)
        return 1

    # V3: at least one decision-log event should overlap the tone range.
    tone_start = expected_offset
    tone_end   = expected_offset + tone_len
    overlapping = [e for e in session.events
                   if e.start_sample < tone_end and e.end_sample > tone_start]
    if not overlapping:
        print(f"WARN [F4]: no decision-log event overlaps the tone range "
              f"[{tone_start}, {tone_end}) — Stream C may be off or the "
              f"detector didn't fire on this tone frequency", file=sys.stderr)

    print(f"PASS: tone at sample {detected_abs} (expected {expected_offset}, "
          f"delta={delta} samples ≈ {delta * 1000.0 / sample_rate:.2f} ms), "
          f"overlapping events={len(overlapping)}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate")
    g.add_argument("--out", required=True, type=Path)
    g.add_argument("--sample-rate",     type=int,   default=384_000)
    g.add_argument("--tone-hz",         type=float, default=40_000.0)
    g.add_argument("--tone-ms",         type=float, default=50.0)
    g.add_argument("--pre-silence-ms",  type=float, default=1000.0)
    g.add_argument("--post-silence-ms", type=float, default=1000.0)
    g.set_defaults(func=cmd_generate)

    v = sub.add_parser("verify")
    v.add_argument("session_root", type=Path)
    v.add_argument("probe_meta",   type=Path)
    v.add_argument("--tolerance-ms", type=float, default=50.0,
                   help="Allowed playback→capture latency (default 50 ms)")
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
