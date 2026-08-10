# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Single entry point for the rough algorithm assessment.

    python -m tools.session_screen.validate all --input <recordings-dir>

runs BatDetect2 truth + ``echobox-replay`` for one recorder config over
every WAV in ``<recordings-dir>`` and drops a per-clip report into
``<output-dir>``. Each stage caches its output as parquet, so re-running
after tweaking a knob is fast.

The five subcommands:

    validate.py truth   --input <dir>   --output truth_manifest.parquet
    validate.py replay  --input <dir>   --output replay_manifest.parquet
    validate.py score   --truth <path>  --replay <path> --output-dir <dir>
    validate.py all     --input <dir>   --output-dir <dir>
    validate.py sweep   --input <dir>   --output-dir <dir>
        # runs the plan's baseline + shorter configs into two subdirs and
        # emits report.md with a config-comparison table.

See ``README.md`` for the four-point honesty ceiling — this tool is a
rough tuning proxy, NOT a device-exact recall claim.
"""
from __future__ import annotations

# Run either as a module (``python -m tools.session_screen.validate``) OR as
# a plain script (``python validate.py …`` — how PyCharm's default green-arrow
# runs it). Direct script invocation leaves ``__package__`` unset, which
# breaks the ``from . import …`` lines below. Fix that up in one place instead
# of asking every user to configure PyCharm as "run as module".
if __package__ in (None, ""):  # pragma: no cover - exercised by CLI, not tests
    import pathlib
    import sys as _sys
    _here = pathlib.Path(__file__).resolve()
    _repo = _here.parents[2]   # tools/session_screen/validate.py → repo root
    if str(_repo) not in _sys.path:
        _sys.path.insert(0, str(_repo))
    __package__ = "tools.session_screen"

import argparse
import sys
import time
from pathlib import Path
from typing import List, Optional


DEFAULT_OUTPUT_DIR = Path("validation_out")


# --- CLI-configurable replay knobs -------------------------------------------
#
# The plan pins baseline + shorter to these exact CLI flags; keeping them in
# one place means the two entry points ('replay' and 'sweep') can't drift.

def _plan_baseline():
    from . import replay as _replay
    return _replay.ReplayConfig(
        preroll_ms=50, silence_ms=50,
        max_length_ms=200, min_length_ms=0, cricket_filter=True)


def _plan_shorter():
    from . import replay as _replay
    return _replay.ReplayConfig(
        preroll_ms=20, silence_ms=40,
        max_length_ms=200, min_length_ms=0, cricket_filter=True)


class _ProgressReporter:
    """stderr progress line with ETA + rate + optional per-file result.

    In-place update via ``\\r`` when the stream is a TTY; one-line-per-file
    otherwise (PyCharm's Run window, CI logs, ``tee`` redirection) so no
    output is silently overwritten in environments that don't honour the
    carriage return.
    """

    def __init__(self, prefix: str):
        self._prefix    = prefix
        self._started   = time.monotonic()
        self._tty       = sys.stderr.isatty()
        self._last_len  = 0
        self._reported  = False

    def _fmt_eta(self, remaining_s: float) -> str:
        if remaining_s < 60:
            return f"{int(remaining_s)}s"
        if remaining_s < 3600:
            return f"{int(remaining_s // 60)}m{int(remaining_s % 60):02d}s"
        return f"{int(remaining_s // 3600)}h{int((remaining_s % 3600) // 60):02d}m"

    def update(self, i: int, total: int, path: Path,
               extra: Optional[str] = None) -> None:
        elapsed = max(1e-3, time.monotonic() - self._started)
        rate    = i / elapsed
        remaining = (total - i) / rate if rate > 0 else 0
        per_file  = elapsed / i if i > 0 else 0
        width  = 30
        filled = int(width * i / max(total, 1))
        bar = "#" * filled + "-" * (width - filled)
        name = path.name[:40]
        line = (f"{self._prefix} [{bar}] {i:>3}/{total} "
                f"{per_file:5.1f}s/f  ETA {self._fmt_eta(remaining):>6}  "
                f"{name}")
        if extra:
            line += f"  | {extra}"

        if self._tty:
            pad = " " * max(0, self._last_len - len(line))
            sys.stderr.write("\r" + line + pad)
            self._last_len = len(line)
            if i == total:
                sys.stderr.write("\n")
        else:
            sys.stderr.write(line + "\n")
        sys.stderr.flush()
        self._reported = True


# --- default paths -----------------------------------------------------------

def _default_truth_path(output_dir: Path) -> Path:
    return output_dir / "truth_manifest.parquet"


def _default_replay_path(output_dir: Path) -> Path:
    return output_dir / "replay_manifest.parquet"


def _report_missing_input_dir(input_dir: Path) -> None:
    """Same 'not a directory' error, but include the absolute path and CWD
    so a user running under PyCharm's default (script-dir) working directory
    can see immediately why a relative path didn't resolve."""
    import os
    print(f"error: --input {input_dir} is not a directory.\n"
          f"  resolved to: {input_dir.resolve()}\n"
          f"  cwd:         {os.getcwd()}\n"
          f"  If you passed a relative path, set the working directory to "
          f"the repo root or pass an absolute path.",
          file=sys.stderr)


# --- truth ------------------------------------------------------------------

def cmd_truth(args: argparse.Namespace) -> int:
    from . import truth
    output = Path(args.output) if args.output else _default_truth_path(
        Path(args.output_dir))
    if output.exists() and not args.force:
        print(f"truth: cache exists at {output}. Use --force to recompute.")
        return 0
    input_dir = Path(args.input)
    if not input_dir.is_dir():
        _report_missing_input_dir(input_dir)
        return 2
    cfg = truth.TruthConfig(
        detection_threshold=args.detection_threshold,
        device=args.device,
    )
    reporter = _ProgressReporter("truth ")
    started = time.monotonic()
    try:
        truth.run_truth(
            input_dir, output, config=cfg,
            progress=reporter.update,
            limit=args.limit,
        )
    except ImportError as e:
        print(f"\nerror: {e}", file=sys.stderr)
        return 3
    elapsed = time.monotonic() - started
    print(f"truth: wrote {output} in {elapsed:.1f}s")
    return 0


# --- replay -----------------------------------------------------------------

def _build_replay_config(args: argparse.Namespace):
    """Turn ``replay`` / ``sweep`` args into a ReplayConfig.

    Precedence: explicit CLI flags override the ``--config`` preset, which
    itself overrides the ReplayConfig defaults.
    """
    from . import replay as R
    if getattr(args, "config", None) == "baseline":
        cfg = _plan_baseline()
    elif getattr(args, "config", None) == "shorter":
        cfg = _plan_shorter()
    else:
        cfg = R.ReplayConfig()

    # Optional per-flag overrides for one-off tuning without editing code.
    for attr in ("preroll_ms", "silence_ms", "min_length_ms", "max_length_ms",
                 "snr_threshold"):
        v = getattr(args, attr, None)
        if v is not None:
            setattr(cfg, attr, v)
    ck = getattr(args, "cricket_filter", None)
    if ck is not None:
        cfg.cricket_filter = (ck == "on")
    tunables = getattr(args, "tunable", None)
    if tunables:
        cfg.tunables = list(tunables)
    return cfg


def cmd_replay(args: argparse.Namespace) -> int:
    from . import replay as R
    output = Path(args.output) if args.output else _default_replay_path(
        Path(args.output_dir))
    if output.exists() and not args.force:
        print(f"replay: cache exists at {output}. Use --force to recompute.")
        return 0
    input_dir = Path(args.input)
    if not input_dir.is_dir():
        _report_missing_input_dir(input_dir)
        return 2

    cfg = _build_replay_config(args)
    print(f"replay: config={cfg.label()}", file=sys.stderr, flush=True)

    reporter = _ProgressReporter("replay")
    started = time.monotonic()
    try:
        R.run_replay(
            input_dir, output, config=cfg,
            progress=reporter.update,
            limit=args.limit,
            jobs=getattr(args, "jobs", 1),
        )
    except RuntimeError as e:
        print(f"\nerror: {e}", file=sys.stderr)
        return 3
    elapsed = time.monotonic() - started
    print(f"replay: wrote {output} in {elapsed:.1f}s")
    return 0


# --- score ------------------------------------------------------------------

def cmd_score(args: argparse.Namespace) -> int:
    from . import score
    truth_path  = Path(args.truth)
    replay_path = Path(args.replay)
    output_dir  = Path(args.output_dir)
    for p in (truth_path, replay_path):
        if not p.exists():
            print(f"error: {p} not found. Run 'truth' and 'replay' first.",
                  file=sys.stderr)
            return 2

    summary = score.score_and_write(truth_path, replay_path, output_dir,
                                    config_label=getattr(args, "label", "") or "")
    print()
    print(f"score: wrote report.md, results.csv, disagreements.csv → {output_dir}")
    print()
    print("Headline (rough tuning proxy, not device-exact):")
    print(f"  clips scored: {summary.n_clips} "
          f"(no-clip sources: {summary.n_no_clip_sources}, "
          f"error rows: {summary.n_error_rows})")
    c = summary.confusion
    pc = summary.per_call
    _print_pct("  per-call recall (KPI 1)", summary.per_call_recall,
               pc.n_calls_captured, pc.n_calls_total)
    _print_pct("  cricket-rejection (KPI 2)", summary.cricket_rejection_rate,
               c.n_tn, c.n_tn + c.n_fp)
    _print_pct("  per-clip recall (legacy)", summary.recall,
               c.n_tp, c.n_tp + c.n_fn)
    _print_pct("  per-clip cricket-FP (legacy)", summary.fp_rate,
               c.n_fp, c.n_fp + c.n_tn)
    return 0


def _print_pct(label: str, ratio: Optional[float], num: int, den: int) -> None:
    if ratio is None:
        print(f"{label}: n/a (0/0)")
    else:
        print(f"{label}: {100.0 * ratio:5.1f}%  ({num}/{den})")


# --- all --------------------------------------------------------------------

def cmd_all(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    truth_args = argparse.Namespace(
        input=args.input, output=None, output_dir=str(output_dir),
        force=args.force, detection_threshold=args.detection_threshold,
        device=args.device, limit=args.limit,
    )
    rc = cmd_truth(truth_args)
    if rc != 0:
        return rc

    replay_args = argparse.Namespace(
        input=args.input, output=None, output_dir=str(output_dir),
        force=args.force, limit=args.limit,
        config=args.config,
        preroll_ms=args.preroll_ms, silence_ms=args.silence_ms,
        min_length_ms=args.min_length_ms, max_length_ms=args.max_length_ms,
        snr_threshold=args.snr_threshold, cricket_filter=args.cricket_filter,
        tunable=getattr(args, "tunable", None),
        jobs=getattr(args, "jobs", 1),
    )
    rc = cmd_replay(replay_args)
    if rc != 0:
        return rc

    score_args = argparse.Namespace(
        truth=str(_default_truth_path(output_dir)),
        replay=str(_default_replay_path(output_dir)),
        output_dir=str(output_dir),
        label=args.config or "",
    )
    return cmd_score(score_args)


# --- sweep (baseline + shorter) ---------------------------------------------

def cmd_sweep(args: argparse.Namespace) -> int:
    """Run the plan's two configs (baseline + shorter) into two subdirs
    and emit a top-level report.md that combines both.

    The truth manifest is computed once at the top level and reused for
    both configs — BatDetect2 truth doesn't depend on the recorder
    config, so re-running it twice would only waste ~an hour of CPU.
    """
    from . import score
    output_root = Path(args.output_dir)
    output_root.mkdir(parents=True, exist_ok=True)

    # 1. Shared truth manifest (cheap re-check via --force gating).
    truth_args = argparse.Namespace(
        input=args.input, output=None, output_dir=str(output_root),
        force=args.force, detection_threshold=args.detection_threshold,
        device=args.device, limit=args.limit,
    )
    rc = cmd_truth(truth_args)
    if rc != 0:
        return rc
    shared_truth = _default_truth_path(output_root)

    # 2. Per-config replay + score into its own subdir.
    labels: List[str]           = []
    summaries: list             = []
    per_subdir: List[Path]      = []
    for label, cfg_ctor in (("baseline", _plan_baseline),
                            ("shorter",  _plan_shorter)):
        sub = output_root / label
        sub.mkdir(parents=True, exist_ok=True)
        cfg = cfg_ctor()
        print(f"\n=== {label}: {cfg.label()} ===", file=sys.stderr)

        replay_out = _default_replay_path(sub)
        if replay_out.exists() and not args.force:
            print(f"replay: cache exists at {replay_out}. Skipping "
                  f"(re-run with --force to redo).")
        else:
            reporter = _ProgressReporter(f"replay-{label}")
            from . import replay as R
            input_dir = Path(args.input)
            if not input_dir.is_dir():
                _report_missing_input_dir(input_dir)
                return 2
            try:
                R.run_replay(input_dir, replay_out, config=cfg,
                             progress=reporter.update, limit=args.limit,
                             jobs=getattr(args, "jobs", 1))
            except RuntimeError as e:
                print(f"\nerror: {e}", file=sys.stderr)
                return 3

        summary = score.score_and_write(shared_truth, replay_out, sub,
                                        config_label=f"{label} ({cfg.label()})")
        labels.append(label)
        summaries.append(summary)
        per_subdir.append(sub)

    # 3. Combined top-level report.md — honesty header + per-config
    # headline rows + baseline-vs-shorter diff table + pointers into
    # the subdirs for per-species matrices and disagreements CSVs.
    lines: List[str] = []
    lines.append("# Echobox rough algorithm assessment — sweep")
    lines.append("")
    lines.append("> " + score.HEADLINE_CAVEATS.replace("\n", "\n> "))
    lines.append("")
    lines.append(f"- truth manifest: `{shared_truth}`")
    lines.append("")
    lines.append(score.render_config_diff(labels, summaries))
    lines.append("")
    lines.append("## Per-config drill-down")
    lines.append("")
    for label, sub in zip(labels, per_subdir):
        lines.append(f"- **{label}** → `{sub}/report.md`, "
                     f"`{sub}/results.csv`, `{sub}/disagreements.csv`")
    lines.append("")
    (output_root / "report.md").write_text("\n".join(lines))
    print(f"\nsweep: wrote {output_root / 'report.md'} "
          f"(+ per-config outputs under {output_root}/)")
    return 0


# --- followup (per-file recall + cricket-FP profile, no re-run) -------------

def cmd_followup(args: argparse.Namespace) -> int:
    """Auditor follow-up. Reads existing sweep artefacts, emits:

      - per config: recall_per_file.csv, presence_fn_files.csv,
        cricket_fp_features.csv, cricket_fp_profile.md
      - top-level:  recall_summary.md (per-file vs per-clip bracket)
    """
    from . import presence
    from . import cricket_fp as cfp
    from . import score
    from .manifest import read_truth_manifest, read_replay_manifest

    root = Path(args.output_dir)
    truth_path = _default_truth_path(root)
    if not truth_path.exists():
        print(f"error: truth manifest not found at {truth_path}. "
              f"Run 'sweep' first (or point --output-dir at the sweep root).",
              file=sys.stderr)
        return 2
    truth = read_truth_manifest(truth_path)
    score.assert_basenames_unique(truth, column="file", source="truth")

    label_summaries: list = []
    for label in args.configs:
        sub = root / label
        replay_path = _default_replay_path(sub)
        if not replay_path.exists():
            print(f"error: {replay_path} missing. Run 'sweep' first.",
                  file=sys.stderr)
            return 2
        replay = read_replay_manifest(replay_path)
        score.assert_basenames_unique(replay, column="source_file",
                                      source=f"replay:{label}")

        pres_summary, pass_summary = presence.write_deliverables(
            label, sub, truth, replay)
        clip_summary = score.score(truth, replay)
        label_summaries.append((label, pres_summary, pass_summary,
                                clip_summary))

        # Cricket-FP profile — needs the sidecar tree kept next to the
        # manifest.
        replay_root = sub / "replay_manifest.replay"
        disagreements_csv = sub / "disagreements.csv"
        if not replay_root.exists() or not disagreements_csv.exists():
            print(f"warning: skipping {label} cricket-FP profile — missing "
                  f"{replay_root} or {disagreements_csv}", file=sys.stderr)
            continue
        cfp.characterise_one_config(
            replay_root, disagreements_csv,
            replay, truth, sub,
            tn_sample_n=args.tn_sample_n,
            tp_sample_n=args.tp_sample_n,
            config_label=label)
        print(f"followup: {label} → recall_per_file.csv, "
              f"presence_fn_files.csv, cricket_fp_features.csv, "
              f"cricket_fp_profile.md")

    (root / "recall_summary.md").write_text(
        presence.render_summary_md(label_summaries))
    print(f"followup: wrote {root / 'recall_summary.md'}")
    print()
    print("Bracket (per-file → per-pass → per-clip):")
    for label, pres, pas, clip in label_summaries:
        c = clip.confusion
        pf_num, pf_den = pres.present_tp, pres.present_tp + pres.present_fn
        pp_num, pp_den = pas.caught, pas.n_passes
        pc_num, pc_den = c.n_tp, c.n_tp + c.n_fn
        pf_pct = 100.0 * pres.recall_per_file if pres.recall_per_file is not None else float("nan")
        pp_pct = 100.0 * pas.recall_per_pass  if pas.recall_per_pass  is not None else float("nan")
        pc_pct = 100.0 * clip.recall          if clip.recall          is not None else float("nan")
        print(f"  {label:8s}: file {pf_pct:5.1f}% ({pf_num}/{pf_den})  "
              f"pass {pp_pct:5.1f}% ({pp_num}/{pp_den})  "
              f"clip {pc_pct:5.1f}% ({pc_num}/{pc_den})")
    return 0


# --- followup2 (veto/provisional recovery, v2 sidecars only) ---------------

def cmd_followup2(args: argparse.Namespace) -> int:
    """Run the veto+provisional recovery analysis against a v2-sidecar
    replay run. Emits ``veto_provisional_recovery.{csv,md}`` alongside
    the manifest.
    """
    from . import veto_provisional as vp
    from .manifest import read_truth_manifest, read_replay_manifest

    root = Path(args.output_dir)
    truth_path = _default_truth_path(root)
    replay_path = _default_replay_path(root)
    for p in (truth_path, replay_path):
        if not p.exists():
            print(f"error: {p} not found. followup2 needs a completed "
                  f"replay+truth in --output-dir.", file=sys.stderr)
            return 2

    truth  = read_truth_manifest(truth_path)
    replay = read_replay_manifest(replay_path)
    replay_root = replay_path.with_suffix(".replay")
    if not replay_root.exists():
        print(f"error: {replay_root} missing — the raw sidecar tree from "
              "echobox-replay is needed for this analysis.", file=sys.stderr)
        return 2

    result = vp.characterise(replay_root, truth, replay, root,
                             config_label=args.config_label)
    pool = result["pool"]
    picks = result["picks"]
    print(f"followup2: wrote {root / 'veto_provisional_recovery.csv'}")
    print(f"followup2: wrote {root / 'veto_provisional_recovery.md'}")
    print()
    total_recoverable_clips = (pool.n_clips_veto_only
                               + pool.n_clips_provisional_only
                               + pool.n_clips_both)
    total_recoverable_calls = (pool.n_calls_veto_only
                               + pool.n_calls_provisional_only
                               + pool.n_calls_both)
    print(f"pool split (rejected clips):")
    print(f"  veto_only:        clips={pool.n_clips_veto_only:>5}  "
          f"calls={pool.n_calls_veto_only}")
    print(f"  provisional_only: clips={pool.n_clips_provisional_only:>5}  "
          f"calls={pool.n_calls_provisional_only}")
    print(f"  both:             clips={pool.n_clips_both:>5}  "
          f"calls={pool.n_calls_both}")
    print(f"  or_fail:          clips={pool.n_clips_or_fail:>5}  "
          f"calls={pool.n_calls_or_fail}")
    print(f"  recoverable pool: {total_recoverable_clips} clips, "
          f"{total_recoverable_calls} calls")
    print()
    for label, key in (("efficient", "max_efficiency"),
                       ("max recov", "max_recovery")):
        p = picks.get(key)
        if p is None:
            print(f"  {label}: NONE")
            continue
        print(f"  {label}: {p['knob']}={p['value']:.2f} → "
              f"{int(p['n_calls_recovered_net'])} calls at "
              f"{p['kb_added_net']:.0f} KB "
              f"({p['calls_per_kb_net']:.4f} calls/KB)")
    return 0


# --- argparse plumbing ------------------------------------------------------

def _add_replay_overrides(p: argparse.ArgumentParser) -> None:
    """Optional per-knob overrides shared by 'replay' and 'all'."""
    p.add_argument("--config", default=None, choices=["baseline", "shorter"],
                   help="Named preset from the plan. Overridden by any of "
                        "the explicit knob flags below.")
    p.add_argument("--preroll-ms",    dest="preroll_ms",    type=int, default=None)
    p.add_argument("--silence-ms",    dest="silence_ms",    type=int, default=None)
    p.add_argument("--min-length-ms", dest="min_length_ms", type=int, default=None)
    p.add_argument("--max-length-ms", dest="max_length_ms", type=int, default=None)
    p.add_argument("--snr-threshold", dest="snr_threshold", type=float, default=None)
    p.add_argument("--cricket-filter", dest="cricket_filter",
                   default=None, choices=["on", "off"])
    # Repeatable: --tunable KEY=VALUE. Forwarded to echobox-replay
    # verbatim; the C++ tool applies each via ISweepTracker::setTunable
    # after dsp.start(). Useful for one-off A/B runs (e.g.
    # max_flatness=0.80) without editing code.
    p.add_argument("--tunable", dest="tunable", action="append",
                   default=None, metavar="KEY=VALUE",
                   help="Detector tunable override (repeatable). "
                        "Forwarded to echobox-replay as --tunable KEY=VALUE.")


def _add_jobs(p: argparse.ArgumentParser) -> None:
    """Parallel-replay shard count. Default 1 = single subprocess,
    byte-identical to pre-flag behaviour. See run_replay's docstring for
    the state-scoping caveat when jobs>1."""
    p.add_argument("-j", "--jobs", type=int, default=1,
                   help="Parallel echobox-replay shards (default: 1). "
                        "jobs>1 splits the WAV list into contiguous "
                        "shards and runs one subprocess per shard; any "
                        "recorder state that crosses file boundaries "
                        "(silence-window rollover, running stats, "
                        "rep-guard if re-enabled) is scoped per-shard.")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="tools.session_screen.validate",
        description=(
            "Rough algorithm assessment: run echobox-replay + BatDetect2 "
            "over a WAV corpus and produce a per-clip confusion + rates "
            "report. ROUGH TUNING PROXY, not a customer-facing claim — "
            "see the report's honesty ceiling."),
    )
    sub = ap.add_subparsers(dest="command", required=True)

    p_truth = sub.add_parser(
        "truth", help="Run BatDetect2 over every WAV and cache detections.")
    p_truth.add_argument("--input", required=True,
                         help="Directory of input WAVs (walked recursively).")
    p_truth.add_argument("--output", default=None,
                         help="Manifest path (default: <output-dir>/truth_manifest.parquet).")
    p_truth.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR),
                         help="Where truth_manifest.parquet lands "
                              "(default: %(default)s).")
    p_truth.add_argument("--force", action="store_true",
                         help="Recompute even if the manifest already exists.")
    p_truth.add_argument("--detection-threshold", type=float,
                         default=0.5,
                         help="BatDetect2 detection threshold (default: %(default)s).")
    p_truth.add_argument("--device", default="auto",
                         choices=["auto", "cpu", "cuda"],
                         help="Torch device (default: auto).")
    p_truth.add_argument("--limit", type=int, default=None,
                         help="Process only the first N WAVs (fast smoke).")
    p_truth.set_defaults(func=cmd_truth)

    p_replay = sub.add_parser(
        "replay",
        help="Run echobox-replay over every WAV; emit per-clip manifest.")
    p_replay.add_argument("--input",  required=True)
    p_replay.add_argument("--output", default=None)
    p_replay.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    p_replay.add_argument("--force", action="store_true")
    p_replay.add_argument("--limit", type=int, default=None,
                          help="Process only the first N WAVs (fast smoke).")
    _add_replay_overrides(p_replay)
    _add_jobs(p_replay)
    p_replay.set_defaults(func=cmd_replay)

    p_score = sub.add_parser(
        "score", help="Compare truth vs replay; emit report + CSVs.")
    p_score.add_argument("--truth",  required=True,
                         help="Path to truth_manifest.parquet.")
    p_score.add_argument("--replay", required=True,
                         help="Path to replay_manifest.parquet.")
    p_score.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    p_score.add_argument("--label", default="",
                         help="Config label rendered into the report title.")
    p_score.set_defaults(func=cmd_score)

    p_all = sub.add_parser(
        "all", help="Run truth → replay → score end to end for one config.")
    p_all.add_argument("--input", required=True)
    p_all.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    p_all.add_argument("--force", action="store_true")
    p_all.add_argument("--detection-threshold", type=float, default=0.5)
    p_all.add_argument("--device", default="auto",
                       choices=["auto", "cpu", "cuda"])
    p_all.add_argument("--limit", type=int, default=None)
    _add_replay_overrides(p_all)
    _add_jobs(p_all)
    p_all.set_defaults(func=cmd_all)

    p_sweep = sub.add_parser(
        "sweep",
        help="Run the plan's baseline + shorter configs and diff them.")
    p_sweep.add_argument("--input", required=True)
    p_sweep.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    p_sweep.add_argument("--force", action="store_true")
    p_sweep.add_argument("--detection-threshold", type=float, default=0.5)
    p_sweep.add_argument("--device", default="auto",
                         choices=["auto", "cpu", "cuda"])
    p_sweep.add_argument("--limit", type=int, default=None)
    _add_jobs(p_sweep)
    p_sweep.set_defaults(func=cmd_sweep)

    p_followup2 = sub.add_parser(
        "followup2",
        help="Veto/provisional recovery analysis. Reads v2 sidecars (post "
             "decision-path instrumentation) and sweeps the temporal rep-guard "
             "knobs + provisional gate on/off, per-call.")
    p_followup2.add_argument("--output-dir", required=True,
                             help="Sweep run root — must contain "
                                  "truth_manifest.parquet + a replay_manifest.parquet "
                                  "written by an instrumented (v2) build.")
    p_followup2.add_argument("--config-label", default="baseline",
                             help="Label stamped into the report title.")
    p_followup2.set_defaults(func=cmd_followup2)

    p_followup = sub.add_parser(
        "followup",
        help="Auditor follow-up: per-file/per-pass recall + cricket-FP profile. "
             "No replay or truth re-run — reads existing manifests + sidecars.")
    p_followup.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR),
                            help="The sweep root that contains truth_manifest.parquet "
                                 "+ baseline/ + shorter/. Same value passed to 'sweep'.")
    p_followup.add_argument("--configs", nargs="+",
                            default=["baseline", "shorter"],
                            help="Config subdirs to process. Default: baseline shorter.")
    p_followup.add_argument("--tn-sample-n", type=int, default=200,
                            help="Sample this many TN clips for the feature "
                                 "distribution comparison (default: %(default)s).")
    p_followup.add_argument("--tp-sample-n", type=int, default=None,
                            help="Sample this many TP clips for the trade-off "
                                 "cost estimate. Default: all TP (usually cheap).")
    p_followup.set_defaults(func=cmd_followup)

    return ap


def main(argv: Optional[list] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":       # pragma: no cover - CLI entry
    sys.exit(main())
