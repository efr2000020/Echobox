"""Batch CLI: detector tuning workflows + ESP32 streaming, no GUI required.

Examples:
    python3 -m validator.cli run    recording.wav
    python3 -m validator.cli score  clean.wav noisy.wav -v
    python3 -m validator.cli grid   clean.wav noisy.wav \\
        --band-snr-threshold 8 10 12 15 --max-flatness 0.65 0.75
    python3 -m validator.cli overlay recording.wav -o out.png
    python3 -m validator.cli diagnose recording.wav
    python3 -m validator.cli stream  --host 192.168.1.42 recording.wav

    # Override a detector field for run/score/overlay:
    python3 -m validator.cli run recording.wav --set band_snr_threshold=10
"""
from __future__ import annotations

import argparse
import dataclasses
import sys
from pathlib import Path
from typing import Dict, List, Sequence

import numpy as np

from . import detector as det_mod
from . import evaluation as ev
from . import esp32_stream
from .dsp import HOP, NFFT, load_wav_float, stft_mags


# --- shared helpers ---------------------------------------------------------

def _apply_set_overrides(cfg: det_mod.DetectorConfig,
                         sets: Sequence[str]) -> det_mod.DetectorConfig:
    """Apply --set key=value overrides to a DetectorConfig."""
    fields = {f.name: f for f in dataclasses.fields(cfg)}
    overrides: Dict[str, object] = {}
    for kv in sets:
        key, _, val = kv.partition("=")
        key = key.strip().lower()
        if key not in fields:
            sys.exit("Unknown detector field '%s'. Known: %s"
                     % (key, ", ".join(sorted(fields))))
        ftype = fields[key].type
        try:
            overrides[key] = _coerce(val, ftype)
        except ValueError as e:
            sys.exit("Invalid value for %s: %s" % (key, e))
    return dataclasses.replace(cfg, **overrides)


def _coerce(s: str, hint):
    if hint in (int, "int"):
        return int(float(s))
    if hint in (float, "float"):
        return float(s)
    return s


def _detector_from_args(args, sample_rate: int) -> det_mod.BandEnergyDetector:
    cfg = det_mod.DetectorConfig(sample_rate=sample_rate)
    if getattr(args, "freq_lo_hz", None) is not None:
        cfg = dataclasses.replace(cfg, freq_lo_hz=float(args.freq_lo_hz))
    if getattr(args, "freq_hi_hz", None) is not None:
        cfg = dataclasses.replace(cfg, freq_hi_hz=float(args.freq_hi_hz))
    if getattr(args, "set", None):
        cfg = _apply_set_overrides(cfg, args.set)
    return det_mod.BandEnergyDetector(cfg)


def _load_and_stft(path: str):
    sr, samples = load_wav_float(path)
    mags = stft_mags(samples)
    return sr, samples, mags


# --- subcommands ------------------------------------------------------------

def cmd_run(args) -> None:
    tps = HOP
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        d = _detector_from_args(args, sr)
        dets = d.run_on_spectrogram(mags)
        secs_per_frame = HOP / sr
        print("=== %s  (sr=%d, %d frames) ===" % (path, sr, mags.shape[0]))
        print("    detections: %d" % len(dets))
        for i, e in enumerate(dets, 1):
            print("    %3d:  t=%6.2f-%6.2fs   %5.1f-%5.1f kHz"
                  % (i, e.start_frame * secs_per_frame,
                     e.end_frame * secs_per_frame,
                     e.lo_hz / 1000.0, e.hi_hz / 1000.0))


def cmd_label(args) -> None:
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        gt = ev.label_ground_truth(mags, sr, NFFT)
        secs_per_frame = HOP / sr
        print("=== %s : %d call events ===" % (path, len(gt)))
        for g in gt:
            print("    t=%6.2f-%6.2fs   ~%2.0f kHz"
                  % (g.start_frame * secs_per_frame,
                     g.end_frame * secs_per_frame,
                     g.mid_hz / 1000.0))


def cmd_score(args) -> None:
    totals = [0, 0, 0]
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        d = _detector_from_args(args, sr)
        dets = d.run_on_spectrogram(mags)
        gt = ev.label_ground_truth(mags, sr, NFFT)
        rep = ev.score(dets, gt)
        totals[0] += rep.tp; totals[1] += rep.fp; totals[2] += rep.fn
        secs_per_frame = HOP / sr
        print("=== %s ===" % path)
        print("    calls=%d  detections=%d  TP=%d  FP=%d  FN=%d"
              % (len(gt), len(dets), rep.tp, rep.fp, rep.fn))
        print("    precision=%.2f  recall=%.2f  F1=%.2f"
              % (rep.precision, rep.recall, rep.f1))
        if rep.false_positives and args.verbose:
            print("    false-positive times: %s"
                  % ", ".join("%.2fs" % (d.start_frame * secs_per_frame)
                              for d in rep.false_positives))
    if len(args.wavs) > 1:
        tp, fp, fn = totals
        prec = tp / (tp + fp) if (tp + fp) else 0.0
        rec  = tp / (tp + fn) if (tp + fn) else 0.0
        f1   = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0
        print("--- COMBINED: TP=%d FP=%d FN=%d  P=%.2f R=%.2f F1=%.2f ---"
              % (tp, fp, fn, prec, rec, f1))


def cmd_overlay(args) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    n = len(args.wavs)
    fig, axes = plt.subplots(n, 1, figsize=(15, 4.5 * n), squeeze=False)
    for ax, path in zip(axes[:, 0], args.wavs):
        sr, _, mags = _load_and_stft(path)
        d = _detector_from_args(args, sr)
        dets = d.run_on_spectrogram(mags)
        Sdb = 20.0 * np.log10(mags.T + 1e-9)
        freqs = np.fft.rfftfreq(NFFT, 1.0 / sr) / 1000.0
        t = np.arange(mags.shape[0]) * HOP / sr
        ax.pcolormesh(t, freqs, Sdb, shading="auto", cmap="inferno",
                      vmin=-100, vmax=-30)
        ax.set_ylim(0, min(200, sr / 2000))
        ax.set_ylabel("kHz")
        ax.set_title("%s  (detections in green)" % path)
        secs_per_frame = HOP / sr
        for e in dets:
            ax.add_patch(plt.Rectangle(
                (e.start_frame * secs_per_frame, e.lo_hz / 1000 - 1.5),
                max((e.end_frame - e.start_frame) * secs_per_frame, 0.012),
                (e.hi_hz - e.lo_hz) / 1000 + 3,
                fill=False, edgecolor="lime", lw=1.4))
    axes[-1, 0].set_xlabel("Time (s)")
    plt.tight_layout()
    plt.savefig(args.out, dpi=85)
    print("Saved overlay to %s" % args.out)


def cmd_diagnose(args) -> None:
    """Report the causal peak SNR each labelled call actually achieves —
    useful for deciding where to set band_snr_threshold."""
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        cfg = det_mod.DetectorConfig(sample_rate=sr)
        n_frames, n_bins = mags.shape
        bin_res = sr / NFFT
        min_bin = int(cfg.freq_lo_hz / bin_res)
        max_bin = min(int(cfg.freq_hi_hz / bin_res), n_bins)

        floor = mags[0].copy().astype(np.float64)
        snr_per_frame = np.zeros(n_frames, dtype=np.float64)
        for fr in range(n_frames):
            band = mags[fr, min_bin:max_bin].astype(np.float64)
            fl = np.maximum(floor[min_bin:max_bin], cfg.min_abs_floor)
            snr_per_frame[fr] = (band / fl).max() if band.size else 0.0
            a = np.where(band > floor[min_bin:max_bin], cfg.alpha_rise, cfg.alpha_fall)
            floor[min_bin:max_bin] = a * floor[min_bin:max_bin] + (1 - a) * band

        gt = ev.label_ground_truth(mags, sr, NFFT)
        secs_per_frame = HOP / sr
        call_snrs = []
        print("=== %s : per-call causal peak-SNR ===" % path)
        for g in gt:
            w = slice(max(0, g.start_frame - 2), g.end_frame + 3)
            msnr = float(snr_per_frame[w].max() if w.stop > w.start else 0.0)
            call_snrs.append(msnr)
            print("    t=%6.2fs  ~%2.0f kHz   maxSNR=%.2f"
                  % (g.start_frame * secs_per_frame, g.mid_hz / 1000, msnr))
        if call_snrs:
            cs = np.array(call_snrs)
            print("    -> calls span SNR %.1f to %.1f (median %.1f)."
                  % (cs.min(), cs.max(), np.median(cs)))
            print("    -> band_snr_threshold below ~%.1f would catch all labelled calls."
                  % cs.min())


def cmd_grid(args) -> None:
    sweep_fields = {
        "band_snr_threshold": getattr(args, "band_snr_threshold", None),
        "min_flatness":       getattr(args, "min_flatness", None),
        "max_flatness":       getattr(args, "max_flatness", None),
        "min_active_frames":  getattr(args, "min_active_frames", None),
        "hangover_frames":    getattr(args, "hangover_frames", None),
        "top_k":              getattr(args, "top_k", None),
    }
    sweeps = {k: v for k, v in sweep_fields.items() if v}
    if not sweeps:
        sweeps = {
            "band_snr_threshold": [8.0, 10.0, 12.0, 15.0],
            "max_flatness":       [0.70, 0.75, 0.80],
        }

    file_cache: List = []
    gt_cache: Dict = {}
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        file_cache.append((path, mags, sr))
        gt_cache[path] = ev.label_ground_truth(mags, sr, NFFT)

    base = det_mod.DetectorConfig()
    rows = ev.grid_search(file_cache, base, sweeps, gt_cache)

    print("Ranked by F1 (then worst-file recall). Top %d of %d combos:\n"
          % (min(args.top, len(rows)), len(rows)))
    for r in rows[:args.top]:
        print("F1=%.2f  minRecall=%.2f  P=%.2f R=%.2f  (TP=%d FP=%d FN=%d)  %s"
              % (r.f1, r.min_recall, r.precision, r.recall,
                 r.tp, r.fp, r.fn, r.overrides))
        for path, tp, fp, fn, nd in r.per_file:
            print("      %-28s  TP=%d FP=%d FN=%d  (ndet=%d)"
                  % (Path(path).name, tp, fp, fn, nd))
        print()


def cmd_stream(args) -> None:
    """Stream a WAV to the ESP32 over TCP. Real-time paced."""
    import threading
    stop_event = threading.Event()

    def on_progress(p: float, elapsed: float) -> None:
        sys.stdout.write("\r  streaming: %5.1f%%  (%.1fs)" % (p * 100, elapsed))
        sys.stdout.flush()

    print("Streaming %s -> %s:%d ..." % (args.wav, args.host, args.port))
    try:
        esp32_stream.stream_wav_to_esp32(
            host=args.host, port=args.port, wav_path=args.wav,
            chunk_ms=args.chunk_ms, on_progress=on_progress,
            stop_event=stop_event,
        )
        sys.stdout.write("\n  done.\n")
    except KeyboardInterrupt:
        stop_event.set()
        sys.stdout.write("\n  cancelled.\n")
    except (ConnectionError, OSError) as e:
        sys.stdout.write("\n  error: %s\n" % e)
        sys.exit(2)


# --- entry point ------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="validator.cli",
        description="BandEnergyDetector tuning + ESP32 streaming.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def detector_args(p):
        p.add_argument("--freq-lo-hz", type=float)
        p.add_argument("--freq-hi-hz", type=float)
        p.add_argument("--set", action="append", default=[],
                       metavar="FIELD=VALUE",
                       help="Override a DetectorConfig field, "
                            "e.g. --set band_snr_threshold=10")

    p = sub.add_parser("run", help="Run detector and list detections")
    p.add_argument("wavs", nargs="+")
    detector_args(p)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("label", help="Build offline ground-truth labels")
    p.add_argument("wavs", nargs="+")
    p.set_defaults(func=cmd_label)

    p = sub.add_parser("score", help="Score detector against offline labels")
    p.add_argument("wavs", nargs="+")
    detector_args(p)
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(func=cmd_score)

    p = sub.add_parser("overlay", help="Render spectrogram + detection boxes (PNG)")
    p.add_argument("wavs", nargs="+")
    detector_args(p)
    p.add_argument("-o", "--out", default="overlay.png")
    p.set_defaults(func=cmd_overlay)

    p = sub.add_parser("diagnose", help="Per-call causal peak SNR")
    p.add_argument("wavs", nargs="+")
    p.set_defaults(func=cmd_diagnose)

    p = sub.add_parser("grid", help="Grid-search detector params")
    p.add_argument("wavs", nargs="+")
    p.add_argument("--top", type=int, default=8)
    p.add_argument("--band-snr-threshold", nargs="+", type=float)
    p.add_argument("--min-flatness",       nargs="+", type=float)
    p.add_argument("--max-flatness",       nargs="+", type=float)
    p.add_argument("--min-active-frames",  nargs="+", type=int)
    p.add_argument("--hangover-frames",    nargs="+", type=int)
    p.add_argument("--top-k",              nargs="+", type=int)
    p.set_defaults(func=cmd_grid)

    p = sub.add_parser("stream", help="Stream a WAV to an ESP32 over TCP")
    p.add_argument("wav")
    p.add_argument("--host", required=True, help="ESP32 IP or hostname")
    p.add_argument("--port", type=int, default=esp32_stream.DEFAULT_PORT)
    p.add_argument("--chunk-ms", type=int, default=20)
    p.set_defaults(func=cmd_stream)

    return ap


def main(argv=None) -> None:
    args = build_parser().parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        try:
            sys.stdout.close()
        except Exception:
            pass
