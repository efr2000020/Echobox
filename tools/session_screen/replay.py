# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Feed the raw recordings through the Echobox offline harness (native.py
+ recorder_model.py) so we get a would-save decision without touching
ALSA or the Pi.

Fast: STFT + detector run in the production C++ code via ctypes, and the
harness path has no ring buffer that can drop samples (drop-free by
construction, which we assert on).

Config: read from ``SESSION_HEADER.json`` if present next to the input
directory, so replay matches how the device was actually run. CLI flags
override.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from tools.validator import native, recorder_model
from tools.validator.dsp import HOP as DEFAULT_HOP

from .manifest import ReplayRow, encode_json_list, write_replay_manifest


@dataclass
class ReplayConfig:
    """The subset of the session header the harness actually consumes.

    Mirrors ``RecorderModelConfig`` + a couple of detector knobs. Any
    session-header field the harness doesn't know about is ignored (with
    a debug line) rather than silently altering behaviour.
    """
    sample_rate:    int   = 384_000
    fft_size:       int   = native.NFFT
    hop_size:       int   = DEFAULT_HOP
    freq_lo_hz:     float = 20_000.0
    freq_hi_hz:     float = 192_000.0
    snr_threshold:  float = 12.0
    cricket_filter: bool  = True
    # Recorder knobs
    preroll_ms:    int = 50
    silence_ms:    int = 50
    min_length_ms: int = 0
    max_length_ms: int = 200
    # HPF cutoff — the C++ pipeline pins it to max(20 kHz, freq_lo_hz).
    hpf_cutoff_hz: float = 20_000.0


def load_config_from_session_header(header_path: Path) -> ReplayConfig:
    """Parse a ``SESSION_HEADER.json`` written by the collection overlay.

    Falls back to defaults for any missing field so an older header still
    yields a runnable config. Raises ``FileNotFoundError`` if the path
    doesn't exist — callers should catch and choose to run with defaults.
    """
    data = json.loads(header_path.read_text())
    cfg = ReplayConfig()
    cfg.sample_rate = int(data.get("sample_rate", cfg.sample_rate))
    conf = data.get("config") or {}
    cfg.fft_size       = int(conf.get("fft_size",       cfg.fft_size))
    cfg.hop_size       = int(conf.get("hop_size",       cfg.hop_size))
    cfg.freq_lo_hz     = float(conf.get("freq_lo_hz",   cfg.freq_lo_hz))
    cfg.freq_hi_hz     = float(conf.get("freq_hi_hz",   cfg.freq_hi_hz))
    cfg.snr_threshold  = float(conf.get("snr_threshold", cfg.snr_threshold))
    cfg.cricket_filter = bool(conf.get("cricket_filter", cfg.cricket_filter))
    cfg.preroll_ms     = int(conf.get("preroll_ms",     cfg.preroll_ms))
    cfg.silence_ms     = int(conf.get("silence_ms",     cfg.silence_ms))
    cfg.min_length_ms  = int(conf.get("min_length_ms",  cfg.min_length_ms))
    cfg.max_length_ms  = int(conf.get("max_length_ms",  cfg.max_length_ms))
    cfg.hpf_cutoff_hz  = max(20_000.0, cfg.freq_lo_hz)
    return cfg


# --- per-file harness pass ---------------------------------------------------

def replay_one(wav_path: Path, cfg: ReplayConfig) -> ReplayRow:
    """Replay one WAV through the Echobox offline harness.

    Handles all failure modes (unreadable WAV, sample-rate mismatch,
    missing native lib) by writing an error string to the row rather
    than raising — the surrounding batch keeps going.
    """
    file_str = str(wav_path)
    try:
        from tools.validator.dsp import load_wav_float
        sr, samples = load_wav_float(file_str)
        if sr != cfg.sample_rate:
            return ReplayRow(
                file=file_str,
                duration_s=len(samples) / float(sr),
                n_would_save=0, n_would_discard=0, would_save=False,
                clip_starts_ms="[]", clip_ends_ms="[]", discard_reasons="[]",
                error=f"sample-rate mismatch: wav={sr}Hz cfg={cfg.sample_rate}Hz",
            )
        mags = native.stft(samples, sr,
                           nfft=cfg.fft_size, hop=cfg.hop_size,
                           hpf_cutoff_hz=cfg.hpf_cutoff_hz)
        det_cfg = native.DetectorConfig(
            sample_rate=cfg.sample_rate,
            fft_size=cfg.fft_size,
            freq_lo_hz=cfg.freq_lo_hz,
            freq_hi_hz=cfg.freq_hi_hz,
            tunables={
                "band_snr_threshold":  float(cfg.snr_threshold),
                "sweep_gate_enabled":  1.0 if cfg.cricket_filter else 0.0,
                "hop_size_samples":    float(cfg.hop_size),
            },
        )
        rec_cfg = recorder_model.RecorderModelConfig(
            preroll_ms=cfg.preroll_ms,
            silence_ms=cfg.silence_ms,
            min_length_ms=cfg.min_length_ms,
            max_length_ms=cfg.max_length_ms,
            cricket_discard=cfg.cricket_filter,
        )
        result = recorder_model.run_on_spectrogram(
            mags, cfg.sample_rate,
            detector_config=det_cfg,
            recorder_config=rec_cfg,
            hop_size=cfg.hop_size,
        )

        frame_ms = cfg.hop_size * 1000.0 / cfg.sample_rate
        starts_ms = [r.begin_frame * frame_ms for r in result.saved]
        ends_ms   = [r.end_frame   * frame_ms for r in result.saved]
        reasons   = [r.discard_reason for r in result.discarded]
        return ReplayRow(
            file=file_str,
            duration_s=result.duration_s,
            n_would_save=len(result.saved),
            n_would_discard=len(result.discarded),
            would_save=len(result.saved) > 0,
            clip_starts_ms=encode_json_list(starts_ms),
            clip_ends_ms=encode_json_list(ends_ms),
            discard_reasons=encode_json_list(reasons),
        )
    except Exception as e:  # pragma: no cover - surfaced per-file
        return ReplayRow(
            file=file_str,
            duration_s=0.0,
            n_would_save=0, n_would_discard=0, would_save=False,
            clip_starts_ms="[]", clip_ends_ms="[]", discard_reasons="[]",
            error=f"{type(e).__name__}: {e}",
        )


def iter_input_wavs(input_dir: Path) -> List[Path]:
    """Walk the input directory for .wav files, sorted deterministically."""
    return sorted(p for p in input_dir.rglob("*.wav") if p.is_file())


CHECKPOINT_EVERY = 25
"""Rewrite the manifest every N rows so a kill or crash mid-stage doesn't
lose the whole run. Matches truth.CHECKPOINT_EVERY."""


def run_replay(input_dir: Path, output_path: Path, *,
               config: Optional[ReplayConfig] = None,
               progress: Optional[callable] = None,
               checkpoint_every: int = CHECKPOINT_EVERY,
               limit: Optional[int] = None) -> Path:
    """Replay every WAV in ``input_dir`` and write the manifest.

    ``progress`` is called with (index, total, path) after each file so a
    CLI can render a status line without this module owning a UI.

    ``checkpoint_every`` flushes the in-flight manifest to
    ``<output>.partial`` every N files so a kill mid-stage leaves data
    behind; the final write atomically renames the partial to the
    target path.

    ``limit`` runs only the first N files (fast smoke).
    """
    import sys as _sys                                                # noqa: WPS433
    if not native.is_available():
        raise RuntimeError(
            "Echobox validator native library not available. "
            + (native.load_error() or "Build it with ./build_dev.sh."))
    cfg = config or ReplayConfig()

    wavs = iter_input_wavs(input_dir)
    if limit is not None:
        wavs = wavs[:max(0, limit)]
    print(f"replay: {len(wavs)} WAVs; "
          f"sr={cfg.sample_rate} hop={cfg.hop_size} "
          f"snr={cfg.snr_threshold} cricket={cfg.cricket_filter}; "
          f"checkpoint every {checkpoint_every} files → "
          f"{output_path.name}.partial",
          file=_sys.stderr)

    partial = output_path.with_suffix(output_path.suffix + ".partial")
    rows: List[ReplayRow] = []
    for i, wav in enumerate(wavs):
        row = replay_one(wav, cfg)
        rows.append(row)
        if progress is not None:
            extra = _replay_row_summary(row)
            progress(i + 1, len(wavs), wav, extra)
        if checkpoint_every > 0 and (i + 1) % checkpoint_every == 0:
            write_replay_manifest(partial, rows)
            print(f"\nreplay: checkpoint → {partial} ({i + 1}/{len(wavs)})",
                  file=_sys.stderr)

    write_replay_manifest(partial, rows)
    partial.replace(output_path)
    return output_path


def _replay_row_summary(row: ReplayRow) -> str:
    """One-glance per-file result string for the progress line."""
    if row.error:
        return f"ERROR: {row.error[:40]}"
    if row.n_would_save == 0 and row.n_would_discard == 0:
        return "no trigger"
    return f"{row.n_would_save} save / {row.n_would_discard} discard"


# --- drop-safety assertion --------------------------------------------------

def assert_drop_free(rows: Iterable[ReplayRow]) -> None:
    """The plan requires refusing to score any file that dropped samples.

    The offline harness path has no ring buffer that CAN drop — samples
    are read straight from the WAV — so this is a defensive assertion
    against a future harness rewrite that introduces streaming. If a row
    ever carries a "dropped" error, this raises so the caller can decide
    whether to score anyway.
    """
    for r in rows:
        if "dropped" in (r.error or "").lower():
            raise RuntimeError(
                f"{r.file}: replay reported sample drops "
                f"({r.error!r}); refusing to score.")
