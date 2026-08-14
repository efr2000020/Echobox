# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Frame-domain model of ``src/recorder/Recorder.cpp``.

The Python validator's ``run_on_spectrogram`` counts *annotations* — the
detector's closed-event Annotation return value. The production recorder
does not read annotations; it polls ``DetectorState.active`` and commits
on the leading edge. Counting annotations therefore doesn't predict
what the recorder actually writes to disk.

This module replays a spectrogram through the detector, captures the
per-frame ``active`` flag, and models the recorder's begin / append /
close cycle plus the cricket-filter "no bat-like event" post-hoc
discard. The output is the field-relevant metric: does the recorder
produce a saved WAV for this file?

Design notes:
- Everything is in *frame* units. Saved length =
  preroll + (end - start) * frame_ms.
- We mirror ``Recorder::loop``:
    * begin on the leading edge of ``active``;
    * extend while ``active`` fires (frame indices tracked);
    * close after ``silenceMs`` of continuous idle;
    * discard if saved length < ``minLengthMs``;
    * cap at ``maxLengthMs``.
- The "no bat-like event" discard is modelled by treating each emitted
  Annotation as a bat-like event: BandEnergyDetector suppresses the
  Annotation whenever ``gate_rejected`` is set, so the emit count is a
  perfect Python-side proxy for the atomic bat-like-events counter the
  on-device recorder actually reads.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

import numpy as np

from . import native
from .dsp import HOP, load_wav_float


@dataclass
class RecorderModelConfig:
    """Frame-domain mirror of the production ``RecorderConfig``.

    .. warning::
       These defaults are the **R2v3-era** geometry (50/50/200, cricket
       filter on). They no longer match ``src/app/Config.hpp``, which
       ships 10/20/40 since the 0.4.0 short-clip release and
       ``cricketFilter = false`` since the per-call recall audit. Pass an
       explicit ``RecorderModelConfig(...)`` if you want to predict what
       the current shipping unit does; a bare ``RecorderModelConfig()``
       predicts the historical one. The long-pass profile remains an
       explicit override (``preroll_ms=1000, silence_ms=2000,
       max_length_ms=5000``).
    """
    preroll_ms:   int  = 50
    silence_ms:   int  = 50
    min_length_ms: int = 0
    max_length_ms: int = 200     # 0 = uncapped
    # Cricket-filter deferred discard: drop clips whose window saw no
    # bat-like event. False models `--cricket-filter off`, which is what
    # the unit now ships; True models the historical on-by-default gate
    # and is what this frozen R2v3 config keeps.
    cricket_discard: bool = True


@dataclass
class SavedRecording:
    """One WAV the modelled recorder would have produced.

    Frame indices are inclusive-start / exclusive-end to match the C++
    ``m_eventStart`` / ``endFrame + 1`` convention.
    """
    begin_frame:    int
    end_frame:      int    # last frame written (exclusive of trailing silence)
    duration_ms:    float
    saved_length_ms: float
    kept:           bool   # False means "would have been discarded"
    discard_reason: str    # "" when kept; else "min-length" / "cricket-gate"


@dataclass
class RecorderModelResult:
    """Everything the recorder-model produced for one WAV."""
    saved:      List[SavedRecording] = field(default_factory=list)
    discarded:  List[SavedRecording] = field(default_factory=list)
    duration_s: float                = 0.0
    n_frames:   int                  = 0

    @property
    def files_saved(self) -> int:
        return sum(1 for r in self.saved if r.kept)


def run_on_spectrogram(mags: np.ndarray,
                       sample_rate: int,
                       *,
                       detector_config: Optional[native.DetectorConfig] = None,
                       recorder_config: Optional[RecorderModelConfig] = None,
                       hop_size: int = HOP) -> RecorderModelResult:
    """Replay a spectrogram through the detector and the modelled recorder."""
    dcfg = detector_config or native.DetectorConfig(sample_rate=sample_rate)
    rcfg = recorder_config or RecorderModelConfig()

    det = native.Detector(dcfg)

    frame_ms      = hop_size * 1000.0 / sample_rate
    preroll_frames = int(round(rcfg.preroll_ms / frame_ms))
    silence_frames = int(round(rcfg.silence_ms / frame_ms))
    max_len_frames = (int(round(rcfg.max_length_ms / frame_ms))
                      if rcfg.max_length_ms > 0 else None)
    min_len_frames = int(round(rcfg.min_length_ms / frame_ms))

    n_frames = mags.shape[0]
    result = RecorderModelResult(duration_s=n_frames * hop_size / sample_rate,
                                 n_frames=n_frames)

    # Recorder state machine.
    state           = "idle"
    begin_frame     = 0
    last_active     = 0
    bat_like_in_clip = 0

    for f in range(n_frames):
        st, det_ann = det.process_frame(mags[f], f)

        # Emitted annotation == bat-like event: BandEnergyDetector
        # suppresses the annotation whenever gate_rejected is true, so
        # counting non-None annotations is a faithful mirror of the C++
        # bat-like-events atomic counter. Only frames while a clip is
        # open are attributed to it.
        if state == "active" and det_ann is not None:
            bat_like_in_clip += 1

        if state == "idle":
            if st.active:
                state = "active"
                begin_frame = f
                last_active = f
                bat_like_in_clip = 0
                # If the leading edge coincides with an event-close annotation
                # (a rare same-frame open-then-close), count it too. The C++
                # recorder wouldn't see the counter change before beginRecording
                # snapshots it, so this event does count toward "clip has a
                # bat-like event".
                if det_ann is not None:
                    bat_like_in_clip += 1
            continue

        # state == "active": either extend, close naturally, or hit the cap.
        if st.active:
            last_active = f

        # Close naturally: silence_frames of continuous idle since the last
        # active frame. The C++ recorder uses a wall-clock timeout; the frame
        # domain equivalent is (f - last_active) frames of idle.
        if (f - last_active) >= silence_frames:
            _finish(result, begin_frame, last_active, preroll_frames,
                    frame_ms, min_len_frames,
                    bat_like_in_clip, rcfg.cricket_discard,
                    max_len_active=False)
            state = "idle"
            continue

        # Hit the max-length cap. Recorder saves what it has and returns to
        # idle. The C++ recorder's cap is on TOTAL frames written
        # (preroll + live), matching what ends up in the WAV; mirror that
        # here so the model's saved_length_ms actually respects the cap.
        # §2.3 guard: if the event is still active at the cap, the detector
        # hasn't stamped the kept-events counter yet so the cricket-discard
        # must be skipped for this clip.
        if (max_len_frames is not None
                and (preroll_frames + (f - begin_frame)) >= max_len_frames):
            end_frame = max(begin_frame,
                            begin_frame + (max_len_frames - preroll_frames))
            _finish(result, begin_frame, end_frame, preroll_frames,
                    frame_ms, min_len_frames,
                    bat_like_in_clip, rcfg.cricket_discard,
                    max_len_active=bool(st.active))
            state = "idle"

    # Flush an in-progress recording at end of stream so the metric matches
    # the on-device behaviour where Recorder::stop() calls endRecording().
    if state == "active":
        _finish(result, begin_frame, last_active, preroll_frames,
                frame_ms, min_len_frames,
                bat_like_in_clip, rcfg.cricket_discard,
                max_len_active=False)

    return result


def run_on_wav(wav_path: str,
               *,
               detector_config: Optional[native.DetectorConfig] = None,
               recorder_config: Optional[RecorderModelConfig] = None,
               hpf_cutoff_hz: float = 20000.0,
               hop_size: int = HOP) -> RecorderModelResult:
    """Convenience wrapper: STFT then run_on_spectrogram."""
    sr, samples = load_wav_float(wav_path)
    mags = native.stft(samples, sr, hpf_cutoff_hz=hpf_cutoff_hz, hop=hop_size)
    return run_on_spectrogram(mags, sr,
                              detector_config=detector_config,
                              recorder_config=recorder_config,
                              hop_size=hop_size)


# --- helpers ---------------------------------------------------------------

def _finish(result: RecorderModelResult,
            begin_frame: int, end_frame: int,
            preroll_frames: int, frame_ms: float,
            min_len_frames: int,
            bat_like_in_clip: int,
            cricket_discard: bool,
            max_len_active: bool = False) -> None:
    saved_len_frames = preroll_frames + (end_frame - begin_frame)
    saved_len_ms     = saved_len_frames * frame_ms
    dur_ms           = (end_frame - begin_frame) * frame_ms

    discard_reason = ""
    if saved_len_frames < min_len_frames:
        discard_reason = "min-length"
    elif cricket_discard and bat_like_in_clip == 0 and not max_len_active:
        # Mirror Recorder::endRecording §2.3: a max-length forced close on
        # a still-open event cannot fairly cricket-discard because the
        # detector has not yet stamped the kept-events counter.
        discard_reason = "cricket-gate"

    rec = SavedRecording(
        begin_frame=begin_frame,
        end_frame=end_frame,
        duration_ms=dur_ms,
        saved_length_ms=saved_len_ms,
        kept=(discard_reason == ""),
        discard_reason=discard_reason,
    )
    if rec.kept:
        result.saved.append(rec)
    else:
        result.discarded.append(rec)
