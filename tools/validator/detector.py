"""Python port of the C++ BandEnergyDetector.

MUST stay in sync with src/dsp/algorithms/BandEnergyDetector/BandEnergyDetector.cpp.
If you change the algorithm in C++, mirror the change here (and vice versa);
otherwise tuning results obtained from this script will diverge from what the
production binary produces.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import numpy as np


# Internal sub-band edges, identical to BAND_EDGES_HZ in the header. At
# configure time these are clamped to the user's [freq_lo_hz, freq_hi_hz]
# window and bands that fall entirely outside the window are dropped.
DEFAULT_BAND_EDGES_HZ: Tuple[float, ...] = (
    20000.0, 45000.0, 80000.0, 130000.0, 190000.0,
)


@dataclass
class DetectorConfig:
    # Pipeline
    sample_rate: int = 384000
    fft_size: int = 4096
    freq_lo_hz: float = 20000.0
    freq_hi_hz: float = 190000.0

    # Tuning (mirror the constexprs in BandEnergyDetector.hpp)
    alpha_rise: float = 0.995
    alpha_fall: float = 0.90
    min_abs_floor: float = 1e-6
    top_k: int = 8
    band_snr_threshold: float = 12.0
    min_flatness: float = 0.10
    max_flatness: float = 0.75
    warmup_frames: int = 40
    min_active_frames: int = 2
    hangover_frames: int = 8

    band_edges_hz: Tuple[float, ...] = field(
        default_factory=lambda: DEFAULT_BAND_EDGES_HZ)


@dataclass
class Detection:
    """One closed event. Hz bounds are sub-band granularity, like the C++ side."""
    start_frame: int
    end_frame: int
    lo_hz: float
    hi_hz: float

    def time_span(self, sample_rate: int, hop: int) -> Tuple[float, float]:
        sec = hop / float(sample_rate)
        return self.start_frame * sec, self.end_frame * sec


@dataclass
class FrameState:
    active: bool
    lo_hz: float
    hi_hz: float


class BandEnergyDetector:
    """Stateful detector. Feed one FFT magnitude frame at a time, or call
    run_on_spectrogram(mags_2d) to process an entire pre-computed STFT."""

    def __init__(self, config: DetectorConfig):
        self.cfg = config
        self.bin_resolution = config.sample_rate / config.fft_size
        self.n_bins = config.fft_size // 2 + 1

        nyquist = config.sample_rate / 2.0
        user_lo = max(0.0, config.freq_lo_hz)
        user_hi = min(nyquist, config.freq_hi_hz)

        bands: List[Tuple[int, int]] = []
        edges = config.band_edges_hz
        for i in range(len(edges) - 1):
            band_lo_hz = max(edges[i],     user_lo)
            band_hi_hz = min(edges[i + 1], user_hi)
            if band_hi_hz <= band_lo_hz:
                continue
            lo = int(band_lo_hz / self.bin_resolution)
            hi = int(band_hi_hz / self.bin_resolution)
            if hi > self.n_bins:
                hi = self.n_bins
            if lo >= self.n_bins:
                break
            if hi > lo + 1:
                bands.append((lo, hi))
        self.bands: List[Tuple[int, int]] = bands

        if bands:
            self.in_band_lo = bands[0][0]
            self.in_band_hi = bands[-1][1]
        else:
            self.in_band_lo = 0
            self.in_band_hi = 0

        self._reset_state()

    # -------- public API -------------------------------------------------

    def process_frame(self,
                      mags: np.ndarray,
                      frame_idx: int
                      ) -> Tuple[FrameState, Optional[Detection]]:
        """Mirror of BandEnergyDetector::processFrame.

        Returns (per_frame_state, annotation_or_none). The annotation is only
        populated on the frame at which an event closes.
        """
        cfg = self.cfg
        self._warmup += 1

        if self._noise_floor is None or self._noise_floor.shape[0] != mags.shape[0]:
            self._noise_floor = mags.astype(np.float64).copy()

        floor = self._noise_floor

        # 1. Per-band top-K SNR over the PRE-update floor.
        best_band_snr = 0.0
        best_band = -1
        for bi, (lo, hi) in enumerate(self.bands):
            band_floor = np.maximum(floor[lo:hi], cfg.min_abs_floor)
            band_snr = mags[lo:hi] / band_floor
            count = hi - lo
            k = min(cfg.top_k, count)
            if k <= 0:
                continue
            # np.partition gives an O(N) top-K boundary, matching std::nth_element.
            top_k_mean = float(np.partition(band_snr, count - k)[count - k:].mean())
            if top_k_mean > best_band_snr:
                best_band_snr = top_k_mean
                best_band = bi

        # 2. Asymmetric EMA floor update (after the SNR measurement).
        a = np.where(mags > floor, cfg.alpha_rise, cfg.alpha_fall)
        self._noise_floor = a * floor + (1.0 - a) * mags

        warmed_up = self._warmup >= cfg.warmup_frames

        # 3. Spectral-flatness gate over the in-band region.
        flatness = 1.0
        if self.in_band_hi > self.in_band_lo:
            in_band = mags[self.in_band_lo:self.in_band_hi].astype(np.float64) + 1e-12
            geo_mean = float(np.exp(np.mean(np.log(in_band))))
            ari_mean = float(in_band.mean())
            flatness = geo_mean / (ari_mean + 1e-12)
        flatness_ok = cfg.min_flatness < flatness < cfg.max_flatness

        hot = (warmed_up and best_band >= 0
               and best_band_snr > cfg.band_snr_threshold
               and flatness_ok)

        # 4. Temporal state machine.
        annotation: Optional[Detection] = None
        if hot:
            band_lo_hz = self.bands[best_band][0] * self.bin_resolution
            band_hi_hz = self.bands[best_band][1] * self.bin_resolution
            self._active_run += 1
            self._silence_frames = 0

            if (not self._in_event) and self._active_run >= cfg.min_active_frames:
                self._in_event = True
                self._event_start = frame_idx - (self._active_run - 1)
                self._event_lo_hz = band_lo_hz
                self._event_hi_hz = band_hi_hz
            if self._in_event:
                self._event_lo_hz = min(self._event_lo_hz, band_lo_hz)
                self._event_hi_hz = max(self._event_hi_hz, band_hi_hz)
        else:
            if self._in_event:
                self._silence_frames += 1
                if self._silence_frames > cfg.hangover_frames:
                    annotation = Detection(
                        start_frame=self._event_start,
                        end_frame=frame_idx - self._silence_frames,
                        lo_hz=self._event_lo_hz,
                        hi_hz=self._event_hi_hz,
                    )
                    self._in_event = False
                    self._active_run = 0
                    self._silence_frames = 0
            else:
                self._active_run = 0

        state = FrameState(
            active=self._in_event,
            lo_hz=self._event_lo_hz if self._in_event else 0.0,
            hi_hz=self._event_hi_hz if self._in_event else 0.0,
        )
        return state, annotation

    def run_on_spectrogram(self, mags_2d: np.ndarray) -> List[Detection]:
        """Process a whole spectrogram and return all closed detections."""
        out: List[Detection] = []
        for f in range(mags_2d.shape[0]):
            _, ann = self.process_frame(mags_2d[f], f)
            if ann is not None:
                out.append(ann)
        return out

    # -------- internals --------------------------------------------------

    def _reset_state(self) -> None:
        self._noise_floor: Optional[np.ndarray] = None
        self._warmup: int = 0
        self._in_event: bool = False
        self._active_run: int = 0
        self._silence_frames: int = 0
        self._event_start: int = 0
        self._event_lo_hz: float = 0.0
        self._event_hi_hz: float = 0.0
