# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Single entry point for the offline validation quick-win.

    python -m tools.session_screen.validate all --input <recordings-dir>

runs BatDetect2 + the Echobox offline harness over every WAV in
``<recordings-dir>`` and drops a per-file report into
``<output-dir>``. Each stage caches its output as parquet, so re-running
after tweaking a knob is fast.

The four subcommands can also be run individually:

    validate.py truth   --input <dir>   --output truth_manifest.parquet
    validate.py replay  --input <dir>   --output replay_manifest.parquet
    validate.py score   --truth <path>  --replay <path> --output-dir <dir>
    validate.py all     --input <dir>   --output-dir <dir>

See ``README.md`` for the two honesty caveats — this tool is a rough
tuning proxy against another model, NOT device-exact truth.
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
from typing import Optional


DEFAULT_OUTPUT_DIR = Path("validation_out")


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
        rate    = i / elapsed                             # files / second
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
            # No \r; one line per file so PyCharm/CI logs preserve history.
            sys.stderr.write(line + "\n")
        sys.stderr.flush()
        self._reported = True


# --- truth ------------------------------------------------------------------

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

def cmd_replay(args: argparse.Namespace) -> int:
    from . import replay
    output = Path(args.output) if args.output else _default_replay_path(
        Path(args.output_dir))
    if output.exists() and not args.force:
        print(f"replay: cache exists at {output}. Use --force to recompute.")
        return 0
    input_dir = Path(args.input)
    if not input_dir.is_dir():
        _report_missing_input_dir(input_dir)
        return 2

    cfg: Optional[replay.ReplayConfig] = None
    if args.session_header:
        header = Path(args.session_header)
        if not header.exists():
            print(f"error: --session-header {header} not found.", file=sys.stderr)
            return 2
        cfg = replay.load_config_from_session_header(header)
        print(f"replay: loaded config from {header}", file=sys.stderr, flush=True)
    else:
        # Convention: the collection overlay writes SESSION_HEADER.json next
        # to the reference WAV dir, so <input>/../SESSION_HEADER.json is the
        # right guess when the user doesn't pass one explicitly.
        auto = input_dir.parent / "SESSION_HEADER.json"
        if auto.exists():
            cfg = replay.load_config_from_session_header(auto)
            print(f"replay: auto-loaded config from {auto}",
                  file=sys.stderr, flush=True)

    reporter = _ProgressReporter("replay")
    started = time.monotonic()
    try:
        replay.run_replay(
            input_dir, output, config=cfg,
            progress=reporter.update,
            limit=args.limit,
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

    rejected_dir = Path(args.rejected_dir) if args.rejected_dir else None
    if rejected_dir is not None and not rejected_dir.is_dir():
        print(f"error: --rejected-dir {rejected_dir} is not a directory. "
              f"Pass the same path the device wrote its --save-rejected clips "
              f"into (typically <output>/rejected/).",
              file=sys.stderr)
        return 2
    rejected_truth = (Path(args.rejected_truth) if args.rejected_truth
                      else None)
    if rejected_truth is not None and not rejected_truth.exists():
        print(f"error: --rejected-truth {rejected_truth} not found. "
              f"Run 'truth --input <rejected-dir>' first, or omit the flag "
              f"to skip the BatDetect2 cross-reference.",
              file=sys.stderr)
        return 2

    summary = score.score_and_write(truth_path, replay_path, output_dir,
                                    rejected_dir=rejected_dir,
                                    rejected_truth_path=rejected_truth)
    print()
    print(f"score: wrote report.md, results.csv, disagreements.csv → {output_dir}")
    if rejected_dir is not None:
        print(f"score: rejected/ ingest scanned {rejected_dir}")
    print()
    print("Headline (rough tuning proxy, not device-exact):")
    print(f"  files scored: {summary.n_files} "
          f"(skipped for errors: {summary.n_errors})")
    _print_pct("  recall", summary.recall, summary.n_tp,
               summary.n_tp + summary.n_fn)
    _print_pct("  cricket-FP", summary.fp_rate, summary.n_fp,
               summary.n_fp + summary.n_tn)
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

    # Reuse the truth/replay commands via shim namespaces so the CLI has
    # ONE source of truth for each stage's argument surface.
    truth_args = argparse.Namespace(
        input=args.input,
        output=None,
        output_dir=str(output_dir),
        force=args.force,
        detection_threshold=args.detection_threshold,
        device=args.device,
        limit=args.limit,
    )
    rc = cmd_truth(truth_args)
    if rc != 0:
        return rc

    replay_args = argparse.Namespace(
        input=args.input,
        output=None,
        output_dir=str(output_dir),
        session_header=args.session_header,
        force=args.force,
        limit=args.limit,
    )
    rc = cmd_replay(replay_args)
    if rc != 0:
        return rc

    score_args = argparse.Namespace(
        truth=str(_default_truth_path(output_dir)),
        replay=str(_default_replay_path(output_dir)),
        output_dir=str(output_dir),
        rejected_dir=args.rejected_dir,
        rejected_truth=args.rejected_truth,
    )
    return cmd_score(score_args)


# --- argparse plumbing ------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="tools.session_screen.validate",
        description=(
            "Offline validation quick-win: compare Echobox's would-save "
            "decisions against BatDetect2 on the same raw recordings. "
            "This is a ROUGH TUNING PROXY, not a device-exact benchmark — "
            "see the report caveats."),
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
        "replay", help="Replay every WAV through the Echobox offline harness.")
    p_replay.add_argument("--input", required=True)
    p_replay.add_argument("--output", default=None)
    p_replay.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    p_replay.add_argument("--force", action="store_true")
    p_replay.add_argument("--session-header", default=None,
                          help="Path to a SESSION_HEADER.json to source the "
                               "recorder config from (auto-discovered as "
                               "<input>/../SESSION_HEADER.json if omitted).")
    p_replay.add_argument("--limit", type=int, default=None,
                          help="Process only the first N WAVs (fast smoke).")
    p_replay.set_defaults(func=cmd_replay)

    p_score = sub.add_parser(
        "score", help="Compare truth vs replay; emit report + CSVs.")
    p_score.add_argument("--truth",  required=True,
                         help="Path to truth_manifest.parquet.")
    p_score.add_argument("--replay", required=True,
                         help="Path to replay_manifest.parquet.")
    p_score.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR),
                         help="Where report.md and CSVs land "
                              "(default: %(default)s).")
    p_score.add_argument("--rejected-dir", default=None,
                         help="Directory of sidecars written by the shipping "
                              "app's --save-rejected feature. When set, the "
                              "report gains a 'Rejected-set — device's own "
                              "near-miss population' section.")
    p_score.add_argument("--rejected-truth", default=None,
                         help="Optional truth manifest built by running "
                              "'truth --input <rejected-dir>' first. When "
                              "provided, the rejected/ section reports how "
                              "many discarded clips BatDetect2 flagged as "
                              "real bats — the direct 'recall we lost' "
                              "tuning signal.")
    p_score.set_defaults(func=cmd_score)

    p_all = sub.add_parser(
        "all", help="Run truth → replay → score end to end.")
    p_all.add_argument("--input", required=True)
    p_all.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    p_all.add_argument("--force", action="store_true",
                       help="Recompute cached stages.")
    p_all.add_argument("--detection-threshold", type=float, default=0.5)
    p_all.add_argument("--device", default="auto",
                       choices=["auto", "cpu", "cuda"])
    p_all.add_argument("--session-header", default=None)
    p_all.add_argument("--rejected-dir", default=None,
                       help="Optional sidecar dir from --save-rejected. See "
                            "'score --help'.")
    p_all.add_argument("--rejected-truth", default=None,
                       help="Optional truth manifest built over --rejected-dir. "
                            "See 'score --help'.")
    p_all.add_argument("--limit", type=int, default=None,
                       help="Process only the first N WAVs (fast smoke).")
    p_all.set_defaults(func=cmd_all)

    return ap


def main(argv: Optional[list] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":       # pragma: no cover - CLI entry
    sys.exit(main())
