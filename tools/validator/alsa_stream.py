# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""ALSA loopback streaming logic, ported from fake-mic.

Allows feeding a WAV file into an ALSA loopback device (snd-aloop) so that
the production binary can capture it as if it came from a real microphone.
"""
from __future__ import annotations

import wave
from dataclasses import dataclass
from typing import Optional, Callable

try:
    import alsaaudio
    _HAVE_ALSA = True
except ImportError:
    _HAVE_ALSA = False

# --- contract with the production app --------------------------------------
CARD_ID         = "UltraMic384K"          # set via `modprobe snd-aloop id=...`
FALLBACK_CARD   = "Loopback"              # default id if loaded without id=
PLAYBACK_DEV    = "hw:{card},0,0"         # we feed here
CAPTURE_DEV     = "plughw:{card},1,0"     # app's --device

TARGET_RATE     = 384_000
TARGET_CH       = 1
TARGET_WIDTH    = 2                       # bytes -> S16_LE
PERIOD_FRAMES   = 1920                    # 5 ms @ 384 kHz


@dataclass
class AlsaWavInfo:
    rate: int
    channels: int
    width: int          # bytes per sample
    frames: int

    @property
    def duration_s(self) -> float:
        return self.frames / self.rate if self.rate else 0.0

    @property
    def is_production_format(self) -> bool:
        return (self.rate == TARGET_RATE
                and self.channels == TARGET_CH
                and self.width == TARGET_WIDTH)


def detect_loopback_card() -> Optional[str]:
    """Return the loopback card id we should use, or None if not loaded."""
    if not _HAVE_ALSA:
        return None
    try:
        cards = alsaaudio.cards()
    except alsaaudio.ALSAAudioError:
        return None
    if CARD_ID in cards:
        return CARD_ID
    if FALLBACK_CARD in cards:
        return FALLBACK_CARD
    return None


def stream_wav_to_alsa(wav_path: str,
                       device: str,
                       loop: bool = False,
                       on_progress: Optional[Callable[[int, int], None]] = None,
                       is_stopping: Optional[Callable[[], bool]] = None) -> None:
    """Stream a WAV to an ALSA device. Real-time paced by ALSA blocking writes."""
    if not _HAVE_ALSA:
        raise RuntimeError("pyalsaaudio not installed")

    with wave.open(wav_path, "rb") as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        width = w.getsampwidth()
        total_frames = w.getnframes()

    pcm = alsaaudio.PCM(
        type=alsaaudio.PCM_PLAYBACK, mode=alsaaudio.PCM_NORMAL,
        rate=rate, channels=channels,
        format=alsaaudio.PCM_FORMAT_S16_LE,
        periodsize=PERIOD_FRAMES, device=device,
    )

    period_bytes = PERIOD_FRAMES * channels * width
    
    try:
        while True:
            with wave.open(wav_path, "rb") as w:
                done = 0
                while True:
                    if is_stopping and is_stopping():
                        return
                    
                    data = w.readframes(PERIOD_FRAMES)
                    if not data:
                        break
                    
                    if len(data) < period_bytes:        # pad final partial period
                        data = data + b"\x00" * (period_bytes - len(data))
                    
                    pcm.write(data)
                    done += PERIOD_FRAMES
                    if on_progress:
                        on_progress(min(done, total_frames), total_frames)
            
            if not loop:
                break
    finally:
        pcm.close()
