"""Offline ground-truth labeler + greedy scoring + grid search.

The ground-truth labeler is intentionally NOT a production algorithm: it
cheats by using a non-causal (whole-file) per-bin median floor that a real
real-time detector cannot use. Its job is only to produce approximate call
labels we can score the active causal detector against. Always eyeball the
overlay before trusting a score.
"""
from __future__ import annotations

import dataclasses
import itertools
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Sequence, Tuple

import numpy as np

from .native import Detector, DetectorConfig, Detection


# --- ground truth -----------------------------------------------------------

@dataclass
class GroundTruthEvent:
    start_frame: int
    end_frame: int
    mid_hz: float


def label_ground_truth(mags_2d: np.ndarray,
                       sample_rate: int,
                       fft_size: int,
                       band_lo_hz: float = 20000.0,
                       band_hi_hz: float = 120000.0,
                       snr_percentile: float = 95.0,
                       snr_floor: float = 5.0,
                       gap_tol_frames: int = 3,
                       call_lo_hz: float = 22000.0,
                       call_hi_hz: float = 90000.0,
                       ) -> List[GroundTruthEvent]:
    n_frames, n_bins = mags_2d.shape
    bin_res = sample_rate / fft_size
    min_bin = int(band_lo_hz / bin_res)
    max_bin = min(int(band_hi_hz / bin_res), n_bins)
    band = mags_2d[:, min_bin:max_bin]

    floor = np.median(band, axis=0) + 1e-9   # non-causal floor
    snr = band / floor
    frame_peak_snr = snr.max(axis=1)
    peak_bin = snr.argmax(axis=1) + min_bin

    bar = max(np.percentile(frame_peak_snr, snr_percentile), snr_floor)
    call_frame = frame_peak_snr > bar

    raw_events: List[Tuple[int, int]] = []
    cur: List[int] | None = None
    gap = 0
    for i in range(n_frames):
        if call_frame[i]:
            cur = [i, i] if cur is None else [cur[0], i]
            gap = 0
        elif cur is not None:
            gap += 1
            if gap > gap_tol_frames:
                raw_events.append((cur[0], cur[1]))
                cur = None
    if cur is not None:
        raw_events.append((cur[0], cur[1]))

    kept: List[GroundTruthEvent] = []
    for s, e in raw_events:
        mid = float(np.median(peak_bin[s:e + 1]) * bin_res)
        if call_lo_hz < mid < call_hi_hz and (e - s) >= 1:
            kept.append(GroundTruthEvent(s, e, mid))
    return kept


# --- scoring ----------------------------------------------------------------

@dataclass
class ScoreReport:
    tp: int
    fp: int
    fn: int
    false_positives: List[Detection] = field(default_factory=list)

    @property
    def precision(self) -> float:
        d = self.tp + self.fp
        return self.tp / d if d else 0.0

    @property
    def recall(self) -> float:
        d = self.tp + self.fn
        return self.tp / d if d else 0.0

    @property
    def f1(self) -> float:
        p, r = self.precision, self.recall
        return 2 * p * r / (p + r) if (p + r) else 0.0


def score(detections: Sequence[Detection],
          gt: Sequence[GroundTruthEvent],
          tol_frames: int = 8) -> ScoreReport:
    """Greedy match of detections to GT by frame-range proximity."""
    matched = set()
    tp = 0
    fp_list: List[Detection] = []
    for d in detections:
        hit = -1
        for i, g in enumerate(gt):
            if i in matched:
                continue
            if d.start_frame <= g.end_frame + tol_frames \
               and d.end_frame >= g.start_frame - tol_frames:
                hit = i
                break
        if hit >= 0:
            matched.add(hit)
            tp += 1
        else:
            fp_list.append(d)
    fp = len(detections) - tp
    fn = len(gt) - len(matched)
    return ScoreReport(tp=tp, fp=fp, fn=fn, false_positives=fp_list)


# --- grid search ------------------------------------------------------------

@dataclass
class GridRow:
    overrides: Dict[str, float]
    f1: float
    min_recall: float
    precision: float
    recall: float
    tp: int
    fp: int
    fn: int
    per_file: List[Tuple[str, int, int, int, int]]   # (path, tp, fp, fn, n_det)


def grid_search(file_cache: Iterable[Tuple[str, np.ndarray, int]],
                base_cfg: DetectorConfig,
                sweeps: Dict[str, Sequence],
                gt_cache: Dict[str, List[GroundTruthEvent]],
                ) -> List[GridRow]:
    """Run every combination of `sweeps` over the cached (path, mags, sr) files.

    Ranks results by F1 (then worst-file recall, so a config can't 'win' by
    acing one file and tanking another).
    """
    keys = list(sweeps.keys())
    rows: List[GridRow] = []

    for combo in itertools.product(*[sweeps[k] for k in keys]):
        overrides = dict(zip(keys, combo))
        # Sweep values land in `tunables` (forwarded to the native detector's
        # setTunable); base_cfg's static fields (algorithm, fft_size, freq
        # window) stay fixed across the sweep.
        merged_tunables = {**base_cfg.tunables, **overrides}

        tp = fp = fn = 0
        per_file: List[Tuple[str, int, int, int, int]] = []
        min_rec = 1.0
        for path, mags, sr in file_cache:
            cfg = dataclasses.replace(
                base_cfg, sample_rate=sr, tunables=merged_tunables)
            det = Detector(cfg)
            dets = det.run_on_spectrogram(mags)
            rep = score(dets, gt_cache[path])
            tp += rep.tp
            fp += rep.fp
            fn += rep.fn
            min_rec = min(min_rec, rep.recall)
            per_file.append((path, rep.tp, rep.fp, rep.fn, len(dets)))

        prec = tp / (tp + fp) if (tp + fp) else 0.0
        rec  = tp / (tp + fn) if (tp + fn) else 0.0
        f1   = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0
        rows.append(GridRow(
            overrides=overrides, f1=f1, min_recall=min_rec,
            precision=prec, recall=rec, tp=tp, fp=fp, fn=fn,
            per_file=per_file,
        ))

    rows.sort(key=lambda r: (-r.f1, -r.min_recall))
    return rows
