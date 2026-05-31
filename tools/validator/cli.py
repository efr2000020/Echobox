"""Batch CLI: detector tuning workflows + ALSA loopback streaming, no GUI required.

Algorithm-agnostic: every subcommand drives the active algorithm through the
native validator library. Pick a specific plugin with ``--algorithm NAME``;
omitted, the CLI uses the first registered algorithm.

Examples::

    python3 -m validator.cli describe                       # list tunables
    python3 -m validator.cli run     recording.wav
    python3 -m validator.cli score   clean.wav noisy.wav -v
    python3 -m validator.cli grid    clean.wav noisy.wav \\
        --sweep band_snr_threshold 8 10 12 15 \\
        --sweep max_flatness 0.65 0.75
    python3 -m validator.cli overlay  recording.wav -o out.png
    python3 -m validator.cli diagnose recording.wav
    python3 -m validator.cli loopback recording.wav --loop

    # Override any static field or algorithm tunable for run / score / overlay:
    python3 -m validator.cli run recording.wav --set band_snr_threshold=10
"""
from __future__ import annotations

import argparse
import dataclasses
import sys
from pathlib import Path
from typing import Dict, List, Sequence

import numpy as np

from . import alsa_stream
from . import evaluation as ev
from . import native
from .dsp import HOP, NFFT, load_wav_float


# --- shared helpers ---------------------------------------------------------

def _apply_set_overrides(cfg: native.DetectorConfig,
                         sets: Sequence[str]) -> native.DetectorConfig:
    """Apply --set key=value overrides.

    Static config fields (sample_rate, freq_lo_hz, ...) are replaced directly;
    everything else is treated as an algorithm tunable and validated by the
    native side at create() time.
    """
    static_fields = native.CONFIG_FIELDS
    static_overrides: Dict[str, object] = {}
    tunable_overrides: Dict[str, float] = dict(cfg.tunables)
    for kv in sets:
        key, _, val = kv.partition("=")
        key = key.strip().lower()
        if key in static_fields:
            try:
                static_overrides[key] = _coerce_static(key, val)
            except ValueError as e:
                sys.exit("Invalid value for %s: %s" % (key, e))
        else:
            try:
                tunable_overrides[key] = float(val)
            except ValueError:
                sys.exit("Invalid value for tunable %s: %r" % (key, val))
    return dataclasses.replace(cfg, tunables=tunable_overrides, **static_overrides)


def _coerce_static(key: str, val: str):
    if key == "algorithm":
        return val
    if key in ("sample_rate", "fft_size"):
        return int(float(val))
    if key in ("freq_lo_hz", "freq_hi_hz"):
        return float(val)
    return val


def _detector_from_args(args, sample_rate: int) -> native.Detector:
    cfg = native.DetectorConfig(
        algorithm=getattr(args, "algorithm", None),
        sample_rate=sample_rate,
        sensitivity=getattr(args, "sensitivity", None))
    if getattr(args, "freq_lo_hz", None) is not None:
        cfg = dataclasses.replace(cfg, freq_lo_hz=float(args.freq_lo_hz))
    if getattr(args, "freq_hi_hz", None) is not None:
        cfg = dataclasses.replace(cfg, freq_hi_hz=float(args.freq_hi_hz))
    if getattr(args, "set", None):
        cfg = _apply_set_overrides(cfg, args.set)
    return native.Detector(cfg)


def _load_and_stft(path: str):
    sr, samples = load_wav_float(path)
    # HPF + STFT both happen native-side now, matching the production runtime.
    mags = native.stft(samples, sr, hpf_cutoff_hz=20000.0)
    return sr, samples, mags


def _parse_sweeps(sweep_args: Sequence[Sequence[str]]) -> Dict[str, List[float]]:
    """Turn `--sweep key v1 v2 ...` (one or more) into {key: [values]}.

    Values are coerced to float; the algorithm will narrow to int where its
    tunable manifest says so.
    """
    sweeps: Dict[str, List[float]] = {}
    for entry in sweep_args:
        if len(entry) < 2:
            sys.exit("--sweep needs a key and at least one value: --sweep KEY VAL [VAL ...]")
        key = entry[0]
        try:
            values = [float(v) for v in entry[1:]]
        except ValueError as e:
            sys.exit("Invalid sweep value for %s: %s" % (key, e))
        sweeps[key] = values
    return sweeps


# --- subcommands ------------------------------------------------------------

def cmd_describe(args) -> None:
    """List the active algorithm, its tunable manifest, and its presets."""
    available = native.list_algorithms()
    if not available:
        sys.exit("No algorithms registered. Scanned %s." % native.algorithms_dir())
    name = args.algorithm or available[0]
    print("Algorithms available: %s" % ", ".join(available))
    print("Active: %s" % name)
    try:
        tunables = native.list_tunables(algorithm=name)
        presets  = native.list_presets(algorithm=name)
    except (ValueError, RuntimeError) as e:
        sys.exit("error: %s" % e)

    if presets:
        print("\nSensitivity presets (EXPERIMENTAL):")
        for p in presets:
            print("  %-10s  %s" % (p.name, p.doc))

    if not tunables:
        print("\n(no tunables)")
        return
    print("\nTunables:")
    print("  %-22s %-6s %-10s %-22s  %s"
          % ("key", "type", "default", "range", "doc"))
    for t in tunables:
        rng = "[%g, %g]" % (t.min, t.max)
        print("  %-22s %-6s %-10g %-22s  %s"
              % (t.key, t.type, t.default, rng, t.doc))


def cmd_run(args) -> None:
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        d = _detector_from_args(args, sr)
        dets = d.run_on_spectrogram(mags)
        secs_per_frame = HOP / sr
        print("=== %s  (sr=%d, %d frames, algorithm=%s) ==="
              % (path, sr, mags.shape[0], d.algorithm))
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
        ax.set_title("%s  (%s, detections in green)" % (path, d.algorithm))
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
    useful for deciding where to set the algorithm's SNR threshold.

    Models the per-bin EMA floor used by floor-based detectors, so the active
    algorithm must expose ``alpha_rise``, ``alpha_fall``, and ``min_abs_floor``
    as tunables. Algorithms with a different internal model (e.g. learned
    detectors) won't expose these — diagnose will refuse rather than print a
    number that doesn't match the algorithm it claims to describe.
    """
    REQUIRED = ("alpha_rise", "alpha_fall", "min_abs_floor")
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        cfg = native.DetectorConfig(
            algorithm=getattr(args, "algorithm", None),
            sample_rate=sr,
            sensitivity=getattr(args, "sensitivity", None))
        det = native.Detector(cfg)
        floors = {k: det.get_tunable(k) for k in REQUIRED}
        if any(v is None for v in floors.values()):
            missing = [k for k, v in floors.items() if v is None]
            sys.exit("diagnose: algorithm '%s' does not expose %s — "
                     "diagnose is specific to floor-based detectors."
                     % (det.algorithm, ", ".join(missing)))
        alpha_rise, alpha_fall, min_abs_floor = (
            floors["alpha_rise"], floors["alpha_fall"], floors["min_abs_floor"])

        n_frames, n_bins = mags.shape
        bin_res = sr / NFFT
        min_bin = int(cfg.freq_lo_hz / bin_res)
        max_bin = min(int(cfg.freq_hi_hz / bin_res), n_bins)

        floor = mags[0].copy().astype(np.float64)
        snr_per_frame = np.zeros(n_frames, dtype=np.float64)
        for fr in range(n_frames):
            band = mags[fr, min_bin:max_bin].astype(np.float64)
            fl = np.maximum(floor[min_bin:max_bin], min_abs_floor)
            snr_per_frame[fr] = (band / fl).max() if band.size else 0.0
            a = np.where(band > floor[min_bin:max_bin], alpha_rise, alpha_fall)
            floor[min_bin:max_bin] = a * floor[min_bin:max_bin] + (1 - a) * band

        gt = ev.label_ground_truth(mags, sr, NFFT)
        secs_per_frame = HOP / sr
        call_snrs = []
        print("=== %s : per-call causal peak-SNR (%s) ===" % (path, det.algorithm))
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
    sweeps = _parse_sweeps(args.sweep)
    if not sweeps:
        sys.exit("grid: pass at least one --sweep KEY V1 V2 ... "
                 "(see `describe` for tunable keys).")

    file_cache: List = []
    gt_cache: Dict = {}
    for path in args.wavs:
        sr, _, mags = _load_and_stft(path)
        file_cache.append((path, mags, sr))
        gt_cache[path] = ev.label_ground_truth(mags, sr, NFFT)

    base = native.DetectorConfig(
        algorithm=getattr(args, "algorithm", None),
        sensitivity=getattr(args, "sensitivity", None))
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


def cmd_loopback(args) -> None:
    """Stream a WAV to an ALSA loopback device. Real-time paced."""
    card = alsa_stream.detect_loopback_card()
    if card is None:
        sys.exit("Error: snd-aloop not loaded. Run 'sudo modprobe snd-aloop id=UltraMic384K' first.")

    device = args.device or alsa_stream.PLAYBACK_DEV.format(card=card)
    print("Streaming %s -> %s %s..." % (args.wav, device, "(looping)" if args.loop else ""))

    try:
        alsa_stream.stream_wav_to_alsa(
            args.wav, device, loop=args.loop,
            on_progress=lambda d, t: sys.stdout.write("\r  %d/%d frames" % (d, t))
        )
        sys.stdout.write("\n  done.\n")
    except KeyboardInterrupt:
        sys.stdout.write("\n  cancelled.\n")
    except Exception as e:
        sys.stdout.write("\n  error: %s\n" % e)
        sys.exit(2)


# --- entry point ------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="validator.cli",
        description="Offline detector tuning + ALSA loopback streaming.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def algorithm_arg(p):
        p.add_argument("--algorithm",
                       help="Plugin to run with (default: first registered). "
                            "See `describe` for the list.")

    def detector_args(p):
        algorithm_arg(p)
        p.add_argument("--sensitivity",
                       help="EXPERIMENTAL: coarse sensitivity preset "
                            "(e.g. quiet/balanced/noisy). Applied before --set, "
                            "so individual tunable overrides still win. "
                            "See `describe` for the list.")
        p.add_argument("--freq-lo-hz", type=float)
        p.add_argument("--freq-hi-hz", type=float)
        p.add_argument("--set", action="append", default=[],
                       metavar="FIELD=VALUE",
                       help="Override a DetectorConfig field or algorithm tunable, "
                            "e.g. --set band_snr_threshold=10")

    p = sub.add_parser("describe", help="List registered algorithms and their tunables")
    algorithm_arg(p)
    p.set_defaults(func=cmd_describe)

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

    p = sub.add_parser("diagnose", help="Per-call causal peak SNR (floor-based detectors only)")
    p.add_argument("wavs", nargs="+")
    algorithm_arg(p)
    p.add_argument("--sensitivity",
                   help="EXPERIMENTAL: read floor parameters from this preset")
    p.set_defaults(func=cmd_diagnose)

    p = sub.add_parser("grid", help="Grid-search detector tunables")
    p.add_argument("wavs", nargs="+")
    algorithm_arg(p)
    p.add_argument("--sensitivity",
                   help="EXPERIMENTAL: starting preset for all sweep combinations")
    p.add_argument("--top", type=int, default=8)
    p.add_argument("--sweep", action="append", nargs="+", default=[],
                   metavar=("KEY", "VAL"),
                   help="Sweep an algorithm tunable: --sweep band_snr_threshold 8 10 12. "
                        "Repeat for multi-axis sweeps.")
    p.set_defaults(func=cmd_grid)

    p = sub.add_parser("loopback", help="Stream a WAV to an ALSA loopback (fake-mic)")
    p.add_argument("wav")
    p.add_argument("--device", help="ALSA device name (default: auto-detect)")
    p.add_argument("--loop", action="store_true", help="Loop the file continuously")
    p.set_defaults(func=cmd_loopback)

    return ap


def main(argv=None) -> None:
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except (ValueError, RuntimeError) as e:
        sys.exit("error: %s" % e)


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        try:
            sys.stdout.close()
        except Exception:
            pass
