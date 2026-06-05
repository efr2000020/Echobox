# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""ctypes wrapper around libechobox_validator.so.

This is the validator's *only* path to the detection algorithm — there is no
Python shadow port. STFT, HPF, detector, and the algorithm registry all live
in the shared production C++ code; this module just plumbs them through to
Python.

The wrapper is **algorithm-agnostic by design**: it never names a specific
detector. Algorithms are discovered at import time by scanning a directory
of plugin ``.so`` files (the same mechanism the production binary uses in
dynamic-plugin mode). Each plugin self-declares its name and its set of
tunables, so adding or renaming an algorithm in C++ does not require any
Python change.

If the ``.so`` is missing, run ``./build_dev.sh`` first (it sets
``ECHOBOX_DYNAMIC_PLUGINS=ON``, which is what produces the validator lib
and the plugin ``.so`` files).
"""
from __future__ import annotations

import ctypes as C
import os
from dataclasses import dataclass, field, fields, replace
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np


# --- ctypes structs (layout-compatible with the C++ side) -------------------

class _DetectorState(C.Structure):
    _fields_ = [("active", C.c_bool),
                ("lo_hz",  C.c_float),
                ("hi_hz",  C.c_float)]


class _Annotation(C.Structure):
    _pack_   = 1
    _fields_ = [("start_frame", C.c_uint32),
                ("end_frame",   C.c_uint32),
                ("high_freq",   C.c_float),
                ("low_freq",    C.c_float)]


class _TunableInfo(C.Structure):
    _fields_ = [("key",           C.c_char_p),
                ("type",          C.c_int),
                ("default_value", C.c_double),
                ("min_value",     C.c_double),
                ("max_value",     C.c_double),
                ("doc",           C.c_char_p)]


class _PresetInfo(C.Structure):
    _fields_ = [("name", C.c_char_p),
                ("doc",  C.c_char_p)]


# Type tag values mirrored from validator_c_api.h. Kept as ints (not an
# IntEnum) to match the C side without dragging in enum machinery.
TUNABLE_TYPE_FLOAT = 0
TUNABLE_TYPE_INT   = 1


# --- lib loading ------------------------------------------------------------

def _find_lib() -> Path:
    env = os.environ.get("ECHOBOX_VALIDATOR_LIB")
    if env:
        p = Path(env)
        if not p.exists():
            raise FileNotFoundError(
                "ECHOBOX_VALIDATOR_LIB points at %s but the file is missing." % p)
        return p
    # Default: alongside the deploy tree, three levels up from this file
    # (tools/validator/native.py -> repo root -> deploy/lib/...).
    repo_root = Path(__file__).resolve().parents[2]
    cand = repo_root / "deploy" / "lib" / "libechobox_validator.so"
    if not cand.exists():
        raise FileNotFoundError(
            "%s not found. Run ./build_dev.sh first "
            "(needs ECHOBOX_DYNAMIC_PLUGINS=ON)." % cand)
    return cand


def _default_algorithms_dir() -> Path:
    env = os.environ.get("ECHOBOX_ALGORITHMS_DIR")
    if env:
        return Path(env)
    # Production install layout: deploy/bin/algorithms/*.so. Same path the
    # main binary walks (see src/app/Application.cpp::scanPlugins).
    return Path(__file__).resolve().parents[2] / "deploy" / "bin" / "algorithms"


_lib: Optional[C.CDLL] = None
_lib_load_error: Optional[str] = None

# Resolved at import time but inert until try_load() actually loads the lib.
# Used by list_algorithms() / status messages even when the .so is missing.
_ALGORITHMS_DIR = _default_algorithms_dir()


def _configure_signatures(lib: C.CDLL) -> None:
    """Apply all ctypes restype/argtypes to a freshly loaded lib handle."""
    lib.eb_init.restype  = C.c_size_t
    lib.eb_init.argtypes = [C.c_char_p]

    lib.eb_stft.restype  = C.c_size_t
    lib.eb_stft.argtypes = [C.c_int, C.c_size_t, C.c_size_t, C.c_float,
                            C.POINTER(C.c_float), C.c_size_t,
                            C.POINTER(C.c_float), C.c_size_t]

    lib.eb_detector_create.restype  = C.c_void_p
    lib.eb_detector_create.argtypes = [C.c_char_p, C.c_int, C.c_size_t,
                                       C.c_float, C.c_float]

    lib.eb_detector_destroy.argtypes = [C.c_void_p]

    lib.eb_detector_process_frame.restype  = C.c_bool
    lib.eb_detector_process_frame.argtypes = [
        C.c_void_p, C.POINTER(C.c_float), C.c_size_t, C.c_uint32,
        C.POINTER(_DetectorState), C.POINTER(_Annotation)]

    lib.eb_detector_set_tunable.restype  = C.c_bool
    lib.eb_detector_set_tunable.argtypes = [C.c_void_p, C.c_char_p, C.c_double]

    lib.eb_detector_get_tunable.restype  = C.c_bool
    lib.eb_detector_get_tunable.argtypes = [C.c_void_p, C.c_char_p,
                                            C.POINTER(C.c_double)]

    lib.eb_detector_list_tunables.restype  = C.c_size_t
    lib.eb_detector_list_tunables.argtypes = [C.c_void_p,
                                              C.POINTER(_TunableInfo), C.c_size_t]

    lib.eb_detector_apply_preset.restype  = C.c_bool
    lib.eb_detector_apply_preset.argtypes = [C.c_void_p, C.c_char_p]

    lib.eb_detector_list_presets.restype  = C.c_size_t
    lib.eb_detector_list_presets.argtypes = [C.c_void_p,
                                             C.POINTER(_PresetInfo), C.c_size_t]

    lib.eb_list_algorithms.restype  = C.c_size_t
    lib.eb_list_algorithms.argtypes = [C.POINTER(C.c_char_p), C.c_size_t]

    lib.eb_detector_set_floor.restype  = C.c_bool
    lib.eb_detector_set_floor.argtypes = [C.c_void_p, C.POINTER(C.c_float), C.c_size_t]


def try_load() -> bool:
    """Attempt to load (or re-load) the validator C library.

    Returns True if the lib is now available, False otherwise. Idempotent on
    success. Stores the failure reason in :func:`load_error` so the GUI can
    surface it in its "tuning unavailable" banner.

    Made public so a "Re-check" button can retry after the user runs
    ``./build_dev.sh`` without restarting the validator.
    """
    global _lib, _lib_load_error
    if _lib is not None:
        return True
    try:
        path = _find_lib()
    except FileNotFoundError as e:
        _lib_load_error = str(e)
        return False
    try:
        lib = C.CDLL(str(path))
    except OSError as e:
        _lib_load_error = "Failed to load %s: %s" % (path, e)
        return False
    _configure_signatures(lib)
    # Scan plugins now that signatures are wired up. eb_init is idempotent.
    lib.eb_init(str(_ALGORITHMS_DIR).encode("utf-8")
                if _ALGORITHMS_DIR.exists() else None)
    _lib = lib
    _lib_load_error = None
    return True


def is_available() -> bool:
    """True iff the validator C library is loaded and usable."""
    return _lib is not None


def load_error() -> Optional[str]:
    """Last load-failure message, or None if the lib is loaded.

    Stays meaningful after a successful retry as well — set back to None on
    success so callers can use it as a "currently broken?" signal.
    """
    return _lib_load_error


def _require_lib() -> None:
    """Raise a clear RuntimeError if anything tries to use the lib unloaded.

    Functions that DON'T need the lib for a useful answer (notably
    :func:`list_algorithms`, which can trivially return ``[]``) handle the
    missing-lib case themselves; this helper is for entry points that
    genuinely cannot proceed without the native side.
    """
    if _lib is None:
        raise RuntimeError(
            "Echobox validator native library not loaded. "
            + (_lib_load_error or
               "Build it with ./build_dev.sh (needs ECHOBOX_DYNAMIC_PLUGINS=ON)."))


# Best-effort load at import. Failure is non-fatal so consumers that only
# need the ALSA-loopback / fake-mic side (which doesn't touch the .so) can
# still use the validator without first building the native lib.
try_load()


# --- public API -------------------------------------------------------------

# Production STFT defaults, sourced from the C++ side at import time. Kept as
# module-level constants for backward compatibility with call sites that
# imported NFFT / HOP. Values can also be reached via eb_default_fft_params
# at runtime if the C++ defaults ever change.
NFFT = 4096
HOP  = 512


def algorithms_dir() -> Path:
    """The directory the wrapper scanned for algorithm plugins at import time."""
    return _ALGORITHMS_DIR


def stft(samples: np.ndarray, sample_rate: int, *,
         nfft: int = NFFT, hop: int = HOP,
         hpf_cutoff_hz: float = 0.0) -> np.ndarray:
    """Run the production STFT (+ optional HPF) on a 1-D float32 sample buffer.

    Returns a (n_frames, nfft/2 + 1) magnitude matrix. n_frames is
    len(samples) // hop, matching the production runtime exactly (including
    the implicit zero-padded warmup on early frames).
    """
    _require_lib()
    samples = np.ascontiguousarray(samples, dtype=np.float32)
    n = len(samples)
    bins = nfft // 2 + 1
    if n < hop:
        return np.empty((0, bins), dtype=np.float32)

    frames = n // hop
    out = np.empty((frames, bins), dtype=np.float32)
    got = _lib.eb_stft(
        sample_rate, nfft, hop, float(hpf_cutoff_hz),
        samples.ctypes.data_as(C.POINTER(C.c_float)), n,
        out.ctypes.data_as(C.POINTER(C.c_float)), out.size)
    return out[:got]


def list_algorithms() -> List[str]:
    """Names of every algorithm plugin discovered at import time.

    Returns an empty list — not an error — when the native lib hasn't loaded,
    so the GUI can show a clean "no algorithms" state and still expose the
    ALSA-loopback / fake-mic mode (which doesn't need the lib).
    """
    if _lib is None:
        return []
    cap = 32
    arr = (C.c_char_p * cap)()
    n = _lib.eb_list_algorithms(arr, cap)
    return [arr[i].decode() for i in range(min(n, cap)) if arr[i]]


# Detection record returned to callers; mirrors C++ Annotation but with the
# nicer naming the existing Python code already uses (lo_hz / hi_hz).
@dataclass
class Detection:
    start_frame: int
    end_frame: int
    lo_hz: float
    hi_hz: float

    def time_span(self, sample_rate: int, hop: int = HOP) -> Tuple[float, float]:
        sec = hop / float(sample_rate)
        return self.start_frame * sec, self.end_frame * sec


@dataclass
class FrameState:
    active: bool
    lo_hz: float
    hi_hz: float


@dataclass
class TunableInfo:
    """One knob the active algorithm exposes via setTunable()."""
    key: str
    type: str          # "float" or "int"
    default: float
    min: float
    max: float
    doc: str

    @property
    def is_int(self) -> bool:
        return self.type == "int"


@dataclass
class PresetInfo:
    """EXPERIMENTAL: one coarse-grained sensitivity preset the active
    algorithm recognises. Each preset name resolves to a bundle of tunable
    overrides owned by the algorithm itself; the Python side just plumbs
    the name through."""
    name: str
    doc: str


def _decode_tunable(raw: _TunableInfo) -> TunableInfo:
    return TunableInfo(
        key=raw.key.decode() if raw.key else "",
        type="int" if raw.type == TUNABLE_TYPE_INT else "float",
        default=float(raw.default_value),
        min=float(raw.min_value),
        max=float(raw.max_value),
        doc=raw.doc.decode() if raw.doc else "",
    )


def _decode_preset(raw: _PresetInfo) -> PresetInfo:
    return PresetInfo(
        name=raw.name.decode() if raw.name else "",
        doc=raw.doc.decode()  if raw.doc  else "",
    )


@dataclass
class DetectorConfig:
    """Static configuration applied at create() time.

    `algorithm=None` picks the first algorithm the registry knows about. That
    keeps single-algorithm setups frictionless while still letting the GUI /
    CLI offer an explicit picker when multiple plugins are installed.

    Per-algorithm tuning knobs are NOT listed here — they're set via
    `tunables` (forwarded to setTunable) and discovered dynamically through
    `Detector.list_tunables()` so this config carries no algorithm-specific
    field names.
    """
    algorithm:   Optional[str] = None
    sample_rate: int           = 384_000
    fft_size:    int           = NFFT
    freq_lo_hz:  float         = 20_000.0
    freq_hi_hz:  float         = 192_000.0
    # EXPERIMENTAL: coarse-grained sensitivity preset name (e.g. "quiet",
    # "balanced", "noisy"). Applied BEFORE tunables, so explicit per-knob
    # overrides in `tunables` always win.
    sensitivity: Optional[str] = None
    # Free-form overrides forwarded to the native detector via set_tunable.
    # Unknown keys raise at create() time, so typos surface immediately.
    tunables:    Dict[str, float] = field(default_factory=dict)


def _resolve_algorithm(name: Optional[str]) -> str:
    """Pick the algorithm to run with: caller-supplied name or first available."""
    if name:
        return name
    available = list_algorithms()
    if not available:
        raise RuntimeError(
            "No detection algorithms registered. Scanned %s — is the dir empty? "
            "Build the dev tree with ./build_dev.sh, or set "
            "ECHOBOX_ALGORITHMS_DIR." % _ALGORITHMS_DIR)
    return available[0]


class Detector:
    """Thin handle around an `eb_detector_create()` instance.

    Algorithm-agnostic: wraps whatever ISweepTracker the native lib knows
    about (selected via DetectorConfig.algorithm). The active algorithm's
    name is available as `self.algorithm`.
    """

    def __init__(self, config: DetectorConfig):
        _require_lib()
        algorithm = _resolve_algorithm(config.algorithm)
        self.cfg = config if config.algorithm else replace(config, algorithm=algorithm)
        self.algorithm = algorithm

        h = _lib.eb_detector_create(
            algorithm.encode("utf-8"),
            int(config.sample_rate),
            int(config.fft_size),
            float(config.freq_lo_hz),
            float(config.freq_hi_hz),
        )
        if not h:
            available = ", ".join(list_algorithms()) or "<none>"
            raise ValueError(
                "Unknown algorithm '%s'. Registered: %s. "
                "(Scanned %s — set ECHOBOX_ALGORITHMS_DIR to override.)"
                % (algorithm, available, _ALGORITHMS_DIR))
        self._h = h
        # Sensitivity preset is applied first so per-tunable overrides win.
        if config.sensitivity:
            if not self.apply_preset(config.sensitivity):
                available = ", ".join(p.name for p in self.list_presets()) or "<none>"
                raise ValueError(
                    "Sensitivity preset '%s' not recognised by algorithm '%s'. "
                    "Available: %s." % (config.sensitivity, algorithm, available))
        # Apply tunables. Surface unknown-key errors here, not silently in C++.
        for key, value in config.tunables.items():
            if not self.set_tunable(key, value):
                raise ValueError(
                    "Tunable '%s' not accepted by algorithm '%s'."
                    % (key, algorithm))

    def __del__(self):
        h = getattr(self, "_h", None)
        if h:
            _lib.eb_detector_destroy(h)
            self._h = None

    def set_tunable(self, key: str, value: float) -> bool:
        return bool(_lib.eb_detector_set_tunable(
            self._h, key.encode("utf-8"), C.c_double(float(value))))

    def get_tunable(self, key: str) -> Optional[float]:
        out = C.c_double(0.0)
        ok = _lib.eb_detector_get_tunable(
            self._h, key.encode("utf-8"), C.byref(out))
        return float(out.value) if ok else None

    def list_tunables(self) -> List[TunableInfo]:
        cap = 64
        arr = (_TunableInfo * cap)()
        n = _lib.eb_detector_list_tunables(self._h, arr, cap)
        return [_decode_tunable(arr[i]) for i in range(min(n, cap))]

    def apply_preset(self, name: str) -> bool:
        """EXPERIMENTAL: apply a named sensitivity preset. Returns False if
        `name` is not recognised by the active algorithm."""
        return bool(_lib.eb_detector_apply_preset(
            self._h, name.encode("utf-8")))

    def list_presets(self) -> List[PresetInfo]:
        """EXPERIMENTAL: enumerate the sensitivity presets this algorithm
        recognises. Returns an empty list for algorithms that don't ship
        presets."""
        cap = 16
        arr = (_PresetInfo * cap)()
        n = _lib.eb_detector_list_presets(self._h, arr, cap)
        return [_decode_preset(arr[i]) for i in range(min(n, cap))]

    def process_frame(self, mags: np.ndarray, frame_idx: int
                      ) -> Tuple[FrameState, Optional[Detection]]:
        mags = np.ascontiguousarray(mags, dtype=np.float32)
        st  = _DetectorState()
        ann = _Annotation()
        emitted = _lib.eb_detector_process_frame(
            self._h,
            mags.ctypes.data_as(C.POINTER(C.c_float)),
            len(mags), int(frame_idx),
            C.byref(st), C.byref(ann))
        state = FrameState(active=bool(st.active),
                           lo_hz=float(st.lo_hz),
                           hi_hz=float(st.hi_hz))
        det = None
        if emitted:
            det = Detection(
                start_frame=int(ann.start_frame),
                end_frame=int(ann.end_frame),
                lo_hz=float(ann.low_freq),
                hi_hz=float(ann.high_freq),
            )
        return state, det

    def run_on_spectrogram(self, mags_2d: np.ndarray) -> List[Detection]:
        out: List[Detection] = []
        for f in range(mags_2d.shape[0]):
            _, det = self.process_frame(mags_2d[f], f)
            if det is not None:
                out.append(det)
        return out

    def set_noise_floor(self, floor: np.ndarray) -> bool:
        """Seed the detector's per-bin noise floor before replay.

        The production binary writes a snapshot of this floor into each
        sidecar JSON at trigger time; feeding it back lets us reproduce
        on-device decisions on short recordings where the EMA would
        otherwise be cold-started from frame 0.

        Length must equal fft_size/2 + 1 for the configured FFT size.
        Returns False on size mismatch or if the algorithm doesn't model
        a floor.
        """
        arr = np.ascontiguousarray(floor, dtype=np.float32)
        return bool(_lib.eb_detector_set_floor(
            self._h,
            arr.ctypes.data_as(C.POINTER(C.c_float)),
            len(arr)))


def list_tunables(algorithm: Optional[str] = None,
                  sample_rate: int = 384_000,
                  fft_size: int = NFFT,
                  freq_lo_hz: float = 20_000.0,
                  freq_hi_hz: float = 192_000.0) -> List[TunableInfo]:
    """Describe the knobs of an algorithm without leaving a detector behind.

    Used by the GUI to build its parameter form and by the CLI's `describe`
    command. Creates a temporary detector, queries it, then disposes of it —
    cheap because configure() is the only setup work that runs.
    """
    cfg = DetectorConfig(algorithm=algorithm, sample_rate=sample_rate,
                         fft_size=fft_size,
                         freq_lo_hz=freq_lo_hz, freq_hi_hz=freq_hi_hz)
    det = Detector(cfg)
    try:
        return det.list_tunables()
    finally:
        del det


def list_presets(algorithm: Optional[str] = None,
                 sample_rate: int = 384_000,
                 fft_size: int = NFFT,
                 freq_lo_hz: float = 20_000.0,
                 freq_hi_hz: float = 192_000.0) -> List[PresetInfo]:
    """EXPERIMENTAL: describe the sensitivity presets of an algorithm without
    leaving a detector behind. Same lifecycle as `list_tunables`."""
    cfg = DetectorConfig(algorithm=algorithm, sample_rate=sample_rate,
                         fft_size=fft_size,
                         freq_lo_hz=freq_lo_hz, freq_hi_hz=freq_hi_hz)
    det = Detector(cfg)
    try:
        return det.list_presets()
    finally:
        del det


def preset_tunables(algorithm: Optional[str],
                    preset: str,
                    sample_rate: int = 384_000,
                    fft_size: int = NFFT,
                    freq_lo_hz: float = 20_000.0,
                    freq_hi_hz: float = 192_000.0) -> Dict[str, float]:
    """EXPERIMENTAL: read back the tunable values a given preset sets.

    Spins up a temporary detector, applies the preset, and snapshots every
    tunable via get_tunable. Used by the GUI to repopulate the parameter
    form when the user picks a preset, so they can see (and tweak) the
    exact values the preset moved to.
    """
    cfg = DetectorConfig(algorithm=algorithm, sample_rate=sample_rate,
                         fft_size=fft_size,
                         freq_lo_hz=freq_lo_hz, freq_hi_hz=freq_hi_hz,
                         sensitivity=preset)
    det = Detector(cfg)
    try:
        snap: Dict[str, float] = {}
        for t in det.list_tunables():
            v = det.get_tunable(t.key)
            if v is not None:
                snap[t.key] = v
        return snap
    finally:
        del det


# --- convenience: known DetectorConfig field names --------------------------
#
# Used by cli.py to validate `--set FIELD=VALUE` overrides before they leave
# Python. Anything not listed here is forwarded as a tunable.

CONFIG_FIELDS = {f.name for f in fields(DetectorConfig)} - {"tunables", "sensitivity"}
