# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for load_config_from_session_header.

The replay stage sources recorder knobs from a real ``SESSION_HEADER.json``
so it matches how the device was actually run. This exercise pins:

  1. Every field in the header maps to the right ReplayConfig attribute.
  2. Missing fields fall back to defaults (older headers stay runnable).
  3. HPF cutoff is pinned to max(20 kHz, freq_lo_hz), mirroring the C++
     pipeline (see ``src/dsp/DspPipeline.cpp:hpfCutoff``).
"""
from __future__ import annotations

import json
from pathlib import Path


# Skip the whole file if the native lib (imported transitively via
# tools.session_screen.replay) isn't available in this environment. The
# config-parsing logic doesn't need it, but the import chain does.
import pytest
try:
    from tools.session_screen import replay as R
except Exception as e:  # pragma: no cover - env-dependent
    pytest.skip(f"replay module unavailable: {e}", allow_module_level=True)


def _write_header(path: Path, config: dict, sample_rate: int = 384000) -> None:
    path.write_text(json.dumps({
        "firmware_sha": "deadbeef",
        "algorithm":    "BandEnergyDetector",
        "sample_rate":  sample_rate,
        "channels":     1,
        "streams":      {"a": True, "b": True, "c": True, "d": True},
        "start_sample": 0,
        "start_wall_iso8601": "2026-08-03T00:00:00.000Z",
        "config":       config,
    }))


def test_load_config_maps_every_field(tmp_path: Path) -> None:
    header = tmp_path / "SESSION_HEADER.json"
    _write_header(header, {
        "preroll_ms":    10,
        "silence_ms":    40,
        "min_length_ms": 5,
        "max_length_ms": 200,
        "snr_threshold": 18.0,
        "fft_size":      4096,
        "hop_size":      512,
        "freq_lo_hz":    16000,
        "freq_hi_hz":    192000,
        "cricket_filter": True,
    })
    cfg = R.load_config_from_session_header(header)

    assert cfg.sample_rate    == 384000
    assert cfg.fft_size       == 4096
    assert cfg.hop_size       == 512
    assert cfg.freq_lo_hz     == 16000.0
    assert cfg.freq_hi_hz     == 192000.0
    assert cfg.snr_threshold  == 18.0
    assert cfg.cricket_filter is True
    assert cfg.preroll_ms     == 10
    assert cfg.silence_ms     == 40
    assert cfg.min_length_ms  == 5
    assert cfg.max_length_ms  == 200
    # Cutoff is pinned to max(20 kHz, freq_lo_hz) to mirror the pipeline.
    # Here freq_lo_hz < 20 kHz so HPF sits at 20 kHz.
    assert cfg.hpf_cutoff_hz  == 20000.0


def test_load_config_hpf_tracks_freq_lo(tmp_path: Path) -> None:
    header = tmp_path / "SESSION_HEADER.json"
    _write_header(header, {
        "freq_lo_hz": 30000,   # above the 20 kHz floor
    })
    cfg = R.load_config_from_session_header(header)
    assert cfg.hpf_cutoff_hz == 30000.0


def test_load_config_falls_back_on_missing_fields(tmp_path: Path) -> None:
    """An older header without every field should still yield a runnable
    config, not raise KeyError."""
    header = tmp_path / "SESSION_HEADER.json"
    _write_header(header, {})   # totally empty config block
    cfg = R.load_config_from_session_header(header)
    assert cfg.sample_rate    == 384000     # top-level fallback
    assert cfg.preroll_ms     == 50         # default from ReplayConfig
    assert cfg.max_length_ms  == 200


def test_load_config_missing_file(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        R.load_config_from_session_header(tmp_path / "does_not_exist.json")
