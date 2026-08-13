# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Drive ``echobox-replay`` (the C++ offline tool) over a corpus of raw
recordings and flatten its ``accepted/`` + ``rejected/`` sidecar tree
into a per-clip parquet manifest.

Why the C++ tool instead of the Python offline harness?
    Only the shipping recorder writes real ``rejected/`` sidecars. Those
    sidecars are the whole point of the exercise: for the first time,
    false negatives come from the device's own discards, not a proxy
    denominator. ``tools/validator/native.py`` runs the STFT + detector
    but has no recorder, so it can't produce them.

Invocation model: **one subprocess for the whole input dir**, then
attribute each output clip back to its source WAV via a cumulative
sample-offset table. Per-file subprocesses would spend most of their
time on plugin scan + DSP init (measured 14 s/60 s-WAV overhead vs
~2 s of actual DSP work). Batching drops a 585-file run from ~2.4 h to
~15 min per config.

Per-clip source-time recovery: the sidecar's ``device.frames_processed``
is a monotonic hop-frame counter across the whole run. Convert to
absolute sample offset (× hop_size), bisect against the cumulative
start-sample table, and the source WAV + within-source offset fall
out. Precision is ±hop_size samples (~1.3 ms at 384 kHz / hop 512),
well tighter than BatDetect2's per-event granularity or our overlap
slop (20 ms).

Attribution safety: the C++ tool sorts input files lexicographically
(``enumerateWavFiles`` in tools/replay/WavFileAudioSource.cpp), and we
build the offset table from the same ``sorted()`` output. If either
sort ever diverges, the attribution silently misaligns — so any change
to enumeration on either side must be paired.
"""
from __future__ import annotations

import bisect
import json
import concurrent.futures
import os
import shutil
import subprocess
import sys
import threading
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from .manifest import ReplayRow, write_replay_manifest


# --- config ------------------------------------------------------------------

@dataclass
class ReplayConfig:
    """Recorder + detector knobs passed to ``echobox-replay`` as CLI flags.

    Defaults are the **legacy long-clip** geometry (50/50/200) from the
    pre-0.4.0 baseline, NOT the current shipping config. The shipping
    device runs 10/20/40 as of 0.4.0 (see ``src/app/Config.hpp``); use
    ``validate --config shipping`` to reproduce that. Fields that don't
    affect the join (algorithm plugin name, ring capacity, etc.) live
    inside the C++ tool's own defaults.
    """
    sample_rate:    int   = 384_000
    fft_size:       int   = 4096
    hop_size:       int   = 512
    freq_lo_hz:     float = 20_000.0
    freq_hi_hz:     float = 192_000.0
    snr_threshold:  float = 12.0
    cricket_filter: bool  = True
    preroll_ms:     int   = 50
    silence_ms:     int   = 50
    min_length_ms:  int   = 0
    max_length_ms:  int   = 200
    hpf_cutoff_hz:  float = 20_000.0
    # Repeatable detector tunables (KEY=VALUE strings), forwarded to
    # echobox-replay as --tunable flags. Empty list = no overrides;
    # detector uses its compiled defaults.
    tunables:       List[str] = field(default_factory=list)

    def label(self) -> str:
        base = (f"pr{self.preroll_ms}_sl{self.silence_ms}_"
                f"mx{self.max_length_ms}_mn{self.min_length_ms}_"
                f"{'ck' if self.cricket_filter else 'nc'}")
        if self.tunables:
            base += "_" + ",".join(self.tunables)
        return base

    def as_cli_flags(self) -> List[str]:
        flags = [
            "--sample-rate",   str(self.sample_rate),
            "--fft-size",      str(self.fft_size),
            "--hop-size",      str(self.hop_size),
            "--freq-lo-hz",    str(int(self.freq_lo_hz)),
            "--freq-hi-hz",    str(int(self.freq_hi_hz)),
            "--snr-threshold", str(self.snr_threshold),
            "--preroll-ms",    str(self.preroll_ms),
            "--silence-ms",    str(self.silence_ms),
            "--min-length-ms", str(self.min_length_ms),
            "--max-length-ms", str(self.max_length_ms),
            "--cricket-filter", "on" if self.cricket_filter else "off",
        ]
        for kv in self.tunables:
            flags += ["--tunable", kv]
        return flags


def load_config_from_session_header(header_path: Path) -> ReplayConfig:
    """Parse a ``SESSION_HEADER.json`` written by the collection overlay."""
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


# --- echobox-replay binary discovery ----------------------------------------

_DEFAULT_BINARY_CANDIDATES = (
    "deploy/bin/echobox-replay",
    "build/tools/replay/echobox-replay",
)

_REPO_ROOT_MARKER = "src"


def _repo_root() -> Path:
    here = Path(__file__).resolve()
    for parent in (here, *here.parents):
        if (parent / _REPO_ROOT_MARKER).is_dir():
            return parent
    return here.parents[2]


def locate_echobox_replay() -> Path:
    """Return an executable ``echobox-replay`` path or raise.

    Precedence: ``$ECHOBOX_REPLAY_BIN`` env var → ``deploy/bin/…`` →
    ``build/tools/replay/…``. The deploy dir is preferred because it
    sets up the ``algorithms/`` sibling dir the plugin loader needs.
    """
    env = os.environ.get("ECHOBOX_REPLAY_BIN")
    if env:
        p = Path(env)
        if not p.is_file() or not os.access(p, os.X_OK):
            raise RuntimeError(
                f"$ECHOBOX_REPLAY_BIN={env} is not an executable file.")
        return p
    root = _repo_root()
    for rel in _DEFAULT_BINARY_CANDIDATES:
        cand = root / rel
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    raise RuntimeError(
        "echobox-replay not found. Build with cmake "
        "(-DECHOBOX_BUILD_REPLAY=ON), then deploy/bin/echobox-replay or "
        "build/tools/replay/echobox-replay should exist. Or set "
        "$ECHOBOX_REPLAY_BIN to point at the binary.")


# --- attribution: cumulative sample-offset table ----------------------------

@dataclass
class _SourceEntry:
    """One row in the offset table: source path + [start, end) in samples
    across the concatenated stream ``echobox-replay`` sees."""
    path:         Path
    start_sample: int    # inclusive
    end_sample:   int    # exclusive


def iter_input_wavs(input_dir: Path) -> List[Path]:
    """Walk the input directory for .wav files, sorted deterministically.

    Sort must match the C++ enumeration order in
    ``tools/replay/WavFileAudioSource.cpp:enumerateWavFiles`` — both use
    ``std::sort`` / ``sorted()`` on filesystem paths, which are the same
    lexicographic byte-order in practice.
    """
    return sorted(p for p in input_dir.rglob("*.wav") if p.is_file())


def _wav_n_frames(wav_path: Path) -> int:
    """Return the WAV's frame count via a stdlib header parse. Cheap —
    ``wave.open`` reads ~44 bytes; no PCM samples are touched."""
    with wave.open(str(wav_path), "rb") as w:
        return w.getnframes()


def _build_offset_table(wavs: List[Path]) -> List[_SourceEntry]:
    """Precompute the [start_sample, end_sample) window for each source
    WAV in the concatenated stream.

    O(n) file-header reads. On a 585-file corpus this is < 1 s.
    """
    out: List[_SourceEntry] = []
    total = 0
    for w in wavs:
        try:
            n = _wav_n_frames(w)
        except Exception:
            # A file we can't open still occupies zero samples in the
            # stream so subsequent files' offsets don't shift. The C++
            # tool will fail loudly on the same file, and the resulting
            # error line will land in ``echobox-replay``'s stdout.
            n = 0
        out.append(_SourceEntry(path=w, start_sample=total,
                                end_sample=total + n))
        total += n
    return out


def _attribute(frames_processed: int, hop_size: int, sample_rate: int,
               table: List[_SourceEntry],
               table_starts: List[int]) -> Tuple[Path, float]:
    """Given a clip's ``frames_processed`` (hop-frame counter across the
    whole run), return ``(source_wav_path, clip_end_ms_in_source)``.

    ``table_starts`` is the cached ``[e.start_sample for e in table]`` —
    pass it in so the bisect is O(log n) instead of O(n log n) across
    thousands of clips.
    """
    abs_sample = frames_processed * hop_size
    i = bisect.bisect_right(table_starts, abs_sample) - 1
    i = max(0, min(i, len(table) - 1))
    entry = table[i]
    within = max(0, abs_sample - entry.start_sample)
    return entry.path, 1000.0 * within / float(sample_rate)


# --- sidecar → row ----------------------------------------------------------

def _wav_duration_ms(wav_path: Path, sample_rate: int) -> float:
    """Duration in ms via the stdlib WAV header."""
    try:
        with wave.open(str(wav_path), "rb") as w:
            n_frames = w.getnframes()
            sr = w.getframerate()
    except Exception:
        return 0.0
    if sr <= 0:
        sr = sample_rate
    return 1000.0 * n_frames / float(sr)


def _to_float(v) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def _load_sidecar(json_path: Path, root: Path, cfg: ReplayConfig,
                  table: List[_SourceEntry],
                  table_starts: List[int]) -> ReplayRow:
    """Parse one sidecar into a ``ReplayRow``, attributing it to its
    source WAV via the cumulative offset table.

    ``kept`` comes from the filesystem path (``rejected/`` prefix under
    ``root`` = discarded), not the JSON — a rejected sidecar carries a
    ``recording.rejected`` block but the layout is the authoritative
    signal.
    """
    try:
        doc = json.loads(json_path.read_text())
    except Exception as e:
        return ReplayRow(
            source_file="", clip_wav=str(json_path.relative_to(root)),
            kept=False, rejected_reason="", rejected_mode="",
            clip_start_ms=0.0, clip_end_ms=0.0, clip_duration_ms=0.0,
            n_events=0, n_gate_rejected_events=0,
            min_bandwidth_khz=0.0, max_drift_khz=0.0,
            tunable_min_bw_khz=0.0, frames_processed=0,
            error=f"sidecar parse: {type(e).__name__}: {e}",
        )

    rel = json_path.relative_to(root)
    # "rejected/" is the recorder's authoritative discard marker. In
    # single-shard mode it lives at rel.parts[0]; in sharded mode
    # (--jobs > 1) it sits under a "shard_NN/" prefix, so scan the whole
    # rel path. We control the shard-dir names, so no false positive.
    kept = "rejected" not in rel.parts

    recording = doc.get("recording") or {}
    device    = doc.get("device")    or {}
    detector  = doc.get("detector")  or {}
    tunables  = detector.get("tunables") or {}
    events    = doc.get("events")    or []
    rejected_meta = recording.get("rejected") or {}

    gate_rejected = [e for e in events if bool(e.get("gate_rejected", False))]
    min_bw = min((_to_float(e.get("bandwidth_khz")) for e in gate_rejected),
                 default=0.0)
    max_dr = max((_to_float(e.get("drift_khz"))     for e in gate_rejected),
                 default=0.0)

    frames_proc = int(device.get("frames_processed", 0))
    hop = cfg.hop_size
    sr  = cfg.sample_rate

    source_path, end_ms = _attribute(frames_proc, hop, sr,
                                     table, table_starts)

    wav_path = json_path.with_suffix(".wav")
    dur_ms = _wav_duration_ms(wav_path, sr) if wav_path.exists() else 0.0
    start_ms = max(0.0, end_ms - dur_ms)

    return ReplayRow(
        source_file=source_path.name,
        clip_wav=str(json_path.relative_to(root).with_suffix(".wav")),
        kept=kept,
        rejected_reason=str(rejected_meta.get("reason", "")),
        rejected_mode=str(rejected_meta.get("mode", "")),
        clip_start_ms=start_ms,
        clip_end_ms=end_ms,
        clip_duration_ms=dur_ms,
        n_events=len(events),
        n_gate_rejected_events=len(gate_rejected),
        min_bandwidth_khz=min_bw,
        max_drift_khz=max_dr,
        tunable_min_bw_khz=_to_float(tunables.get("min_bandwidth_khz")),
        frames_processed=frames_proc,
    )


def _walk_output_dir(output_dir: Path, cfg: ReplayConfig,
                     table: List[_SourceEntry],
                     table_starts: List[int]) -> List[ReplayRow]:
    """Enumerate every clip sidecar under ``output_dir`` into
    ``ReplayRow`` rows, attributing each to a source WAV."""
    rows: List[ReplayRow] = []
    if not output_dir.exists():
        return rows
    for p in sorted(output_dir.rglob("*.json")):
        if p.name == "replay_manifest.json":
            continue
        if p.name.startswith("."):
            continue
        rows.append(_load_sidecar(p, output_dir, cfg, table, table_starts))
    return rows


def _no_clips_row(source_wav: Path) -> ReplayRow:
    return ReplayRow(
        source_file=source_wav.name, clip_wav="", kept=False,
        rejected_reason="", rejected_mode="",
        clip_start_ms=0.0, clip_end_ms=0.0, clip_duration_ms=0.0,
        n_events=0, n_gate_rejected_events=0,
        min_bandwidth_khz=0.0, max_drift_khz=0.0,
        tunable_min_bw_khz=0.0, frames_processed=0,
        no_clips=True,
    )


# --- driver: one subprocess for the whole corpus ----------------------------

def _prepare_input_for_replay(wavs: List[Path], input_dir: Path,
                              scratch_root: Path,
                              limit: Optional[int]) -> Path:
    """When ``--limit N`` is set, ``echobox-replay`` needs to see only
    those N files. Build a scratch dir of symlinks so the C++ tool's
    directory walk lands only on the intended subset.

    For a full run (``limit is None``), just return ``input_dir`` — no
    symlinking, so the C++ tool sees the real corpus paths in its
    stdout.
    """
    if limit is None or len(wavs) == 0:
        return input_dir
    link_dir = scratch_root / "_input_subset"
    if link_dir.exists():
        shutil.rmtree(link_dir)
    link_dir.mkdir(parents=True)
    for w in wavs:
        (link_dir / w.name).symlink_to(w.resolve())
    return link_dir


_STREAM_LOCK = threading.Lock()


def _stream_subprocess(argv: List[str], prefix: str = "  ") -> int:
    """Fire ``argv``, tee its combined stdout+stderr to sys.stderr line
    by line, return the exit code. Line-buffered so long-running runs
    don't sit silent for minutes.

    A module-level lock serialises the per-line writes so parallel shards
    can't interleave mid-line; the lock is held only for the duration of
    one line write, not the whole subprocess.
    """
    proc = subprocess.Popen(argv,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    assert proc.stdout is not None
    for line in iter(proc.stdout.readline, ''):
        with _STREAM_LOCK:
            sys.stderr.write(prefix + line)
            sys.stderr.flush()
    proc.stdout.close()
    return proc.wait()


# --- shard helpers ----------------------------------------------------------

def _partition_contiguous(wavs: List[Path], n: int) -> List[List[Path]]:
    """Split ``wavs`` into ``n`` contiguous chunks of near-equal size.

    Contiguous (not round-robin) so time-adjacent field WAVs stay in the
    same shard — keeps the recorder's short-timescale state (silence
    rollover, running noise floor) as close to single-run behaviour as
    possible. Empty shards are dropped so ``jobs > n_files`` degrades
    gracefully.
    """
    n = max(1, min(n, len(wavs)))
    base, extra = divmod(len(wavs), n)
    out: List[List[Path]] = []
    start = 0
    for i in range(n):
        size = base + (1 if i < extra else 0)
        if size > 0:
            out.append(wavs[start:start + size])
            start += size
    return out


def _prepare_shard_input(wavs: List[Path], link_dir: Path) -> Path:
    """Symlink one shard's WAVs into a fresh dir the C++ tool can enumerate.

    Uses ``w.name`` — matches ``_prepare_input_for_replay``'s existing
    convention. Two source WAVs with the same basename in different
    subdirs would collide; not a problem for the flat field-session
    layouts we ship against.
    """
    if link_dir.exists():
        shutil.rmtree(link_dir)
    link_dir.mkdir(parents=True)
    for w in wavs:
        (link_dir / w.name).symlink_to(w.resolve())
    return link_dir


@dataclass
class _ShardResult:
    idx:       int
    shard_dir: Path
    wavs:      List[Path]
    table:     List[_SourceEntry]
    rc:        int


def _run_one_shard(idx: int, shard_wavs: List[Path], cfg: ReplayConfig,
                   bin_path: Path, replay_root: Path,
                   n_shards: int) -> _ShardResult:
    """Launch one echobox-replay subprocess for one shard.

    Returns even on non-zero rc; the caller inspects ``rc`` after all
    shards complete so a single failure doesn't leave zombies.
    """
    shard_dir  = replay_root / f"shard_{idx:02d}"
    input_link = replay_root / f"_input_shard_{idx:02d}"
    _prepare_shard_input(shard_wavs, input_link)
    shard_dir.mkdir(parents=True, exist_ok=True)

    # --save-rejected requires --cricket-filter on (nothing gets rejected
    # otherwise, so echobox-replay refuses the combination). For ceiling
    # runs where the filter is off, drop the rejected-sink flags.
    argv = [
        str(bin_path),
        "--input",  str(input_link),
        "--output", str(shard_dir),
    ]
    if cfg.cricket_filter:
        argv += ["--save-rejected", "all",
                 "--save-rejected-max-per-hour", "0"]
    argv += list(cfg.as_cli_flags())
    prefix = f"  [replay-{idx:02d}/{n_shards - 1:02d}] "
    rc = _stream_subprocess(argv, prefix=prefix)
    table = _build_offset_table(shard_wavs)
    return _ShardResult(idx=idx, shard_dir=shard_dir, wavs=shard_wavs,
                        table=table, rc=rc)


def _collect_shard_rows(shard: _ShardResult, replay_root: Path,
                        cfg: ReplayConfig) -> List[ReplayRow]:
    """Walk one shard's sidecar tree and emit its rows (including
    no_clips rows for its WAVs that produced nothing)."""
    starts = [e.start_sample for e in shard.table]
    rows: List[ReplayRow] = []
    if shard.shard_dir.exists():
        for p in sorted(shard.shard_dir.rglob("*.json")):
            if p.name == "replay_manifest.json" or p.name.startswith("."):
                continue
            rows.append(_load_sidecar(p, replay_root, cfg,
                                      shard.table, starts))
    seen = {r.source_file for r in rows
            if r.source_file and not r.no_clips}
    for entry in shard.table:
        if str(entry.path) not in seen:
            rows.append(_no_clips_row(entry.path))
    return rows


def run_replay(input_dir: Path, output_path: Path, *,
               config: Optional[ReplayConfig] = None,
               replay_root: Optional[Path] = None,
               binary: Optional[Path] = None,
               progress: Optional[Callable] = None,
               checkpoint_every: int = 0,
               limit: Optional[int] = None,
               jobs: int = 1) -> Path:
    """Batch-replay ``input_dir`` through ``echobox-replay`` and write a
    per-clip manifest to ``output_path``.

    ``replay_root`` — where the C++ tool's ``accepted/`` + ``rejected/``
    sidecar tree lands. Kept after the run for debugging; defaults to
    ``<output_path>.replay/``.

    ``jobs`` — number of parallel echobox-replay subprocesses. Default 1
    preserves the single-run stream (byte-identical to pre-flag runs).
    With ``jobs > 1`` the WAV list is split into contiguous shards and
    each shard becomes its own subprocess under
    ``<replay_root>/shard_NN/``; any recorder state that crosses file
    boundaries (silence-window rollover, running stats, rep-guard if
    re-enabled) is therefore scoped to a shard, not the whole corpus.
    Rep-guard is OFF by default so this is inert today.

    ``progress`` and ``checkpoint_every`` are accepted for compat with
    the ``truth`` stage's driver signature but unused in subprocess mode
    — the batch either succeeds cleanly or fails; there's nothing
    meaningful to checkpoint mid-run.
    """
    cfg = config or ReplayConfig()
    bin_path = binary or locate_echobox_replay()
    jobs = max(1, int(jobs or 1))

    if replay_root is None:
        replay_root = output_path.with_suffix(".replay")
    if replay_root.exists():
        shutil.rmtree(replay_root)
    replay_root.mkdir(parents=True, exist_ok=True)

    wavs = iter_input_wavs(input_dir)
    if limit is not None:
        wavs = wavs[:max(0, limit)]

    if not wavs:
        print("replay: no input WAVs — writing empty manifest.",
              file=sys.stderr)
        write_replay_manifest(output_path, [])
        return output_path

    print(f"replay: binary={bin_path}", file=sys.stderr)
    print(f"replay: {len(wavs)} WAVs; config={cfg.label()}",
          file=sys.stderr)

    if jobs > 1:
        rows = _run_sharded(wavs, cfg, bin_path, replay_root, jobs)
    else:
        rows = _run_single(wavs, cfg, bin_path, replay_root,
                           input_dir, limit)

    rows.sort(key=lambda r: (r.source_file, r.clip_start_ms))

    partial = output_path.with_suffix(output_path.suffix + ".partial")
    write_replay_manifest(partial, rows)
    partial.replace(output_path)
    print(f"replay: wrote {output_path} "
          f"({sum(1 for r in rows if r.kept)} kept + "
          f"{sum(1 for r in rows if not r.kept and not r.no_clips)} "
          f"discarded + "
          f"{sum(1 for r in rows if r.no_clips)} sources without clips)",
          file=sys.stderr)
    return output_path


def _run_single(wavs: List[Path], cfg: ReplayConfig, bin_path: Path,
                replay_root: Path, input_dir: Path,
                limit: Optional[int]) -> List[ReplayRow]:
    """Original single-subprocess path — byte-identical to pre-jobs runs
    (same argv, same input arg, same output layout, same offset table)."""
    started_table = time.monotonic()
    table = _build_offset_table(wavs)
    table_starts = [e.start_sample for e in table]
    print(f"replay: offset table built in "
          f"{time.monotonic() - started_table:.1f}s "
          f"(total samples: {table[-1].end_sample:,})", file=sys.stderr)

    actual_input = _prepare_input_for_replay(wavs, input_dir, replay_root, limit)
    # --save-rejected requires --cricket-filter on (see shard variant).
    argv = [
        str(bin_path),
        "--input",  str(actual_input),
        "--output", str(replay_root),
    ]
    if cfg.cricket_filter:
        argv += ["--save-rejected", "all",
                 "--save-rejected-max-per-hour", "0"]
    argv += list(cfg.as_cli_flags())
    print(f"replay: launching echobox-replay (expect ~"
          f"{15 * len(wavs) / 585:.1f} min for {len(wavs)} WAVs)",
          file=sys.stderr)
    started = time.monotonic()
    rc = _stream_subprocess(argv, prefix="  [replay] ")
    if rc != 0:
        raise RuntimeError(f"echobox-replay failed with rc={rc}")
    print(f"replay: echobox-replay done in "
          f"{time.monotonic() - started:.1f}s",
          file=sys.stderr)

    rows = _walk_output_dir(replay_root, cfg, table, table_starts)
    seen_sources = {r.source_file for r in rows
                    if r.source_file and not r.no_clips}
    for entry in table:
        if str(entry.path) not in seen_sources:
            rows.append(_no_clips_row(entry.path))
    return rows


def _run_sharded(wavs: List[Path], cfg: ReplayConfig, bin_path: Path,
                 replay_root: Path, jobs: int) -> List[ReplayRow]:
    """Split WAVs into contiguous shards and run N subprocesses in
    parallel. Merges each shard's sidecar tree into one row list.

    Honesty caveat, printed up-front so it lands in any tee/log:
    recorder state that crosses file boundaries is scoped to a shard,
    not the whole corpus. Rep-guard is OFF by default; if you turn it
    back on, prefer jobs=1 for apples-to-apples with prior runs.
    """
    shards = _partition_contiguous(wavs, jobs)
    n = len(shards)
    print(f"replay: sharding into {n} parallel jobs "
          f"({[len(s) for s in shards]} WAVs each)", file=sys.stderr)
    print(f"replay: HONESTY NOTE — with jobs>1, recorder state that "
          f"crosses file boundaries (silence-window rollover, running "
          f"stats, rep-guard if re-enabled) is scoped per-shard, not "
          f"across the whole corpus. Rep-guard is OFF by default so "
          f"this is inert today. Use jobs=1 for byte-identical to "
          f"single-run.", file=sys.stderr)
    print(f"replay: launching {n} echobox-replay subprocesses (expect "
          f"~{15 * len(wavs) / (585 * n):.1f} min for {len(wavs)} WAVs)",
          file=sys.stderr)

    started = time.monotonic()
    results: List[Optional[_ShardResult]] = [None] * n
    with concurrent.futures.ThreadPoolExecutor(max_workers=n) as ex:
        futures = {ex.submit(_run_one_shard, i, shards[i], cfg,
                             bin_path, replay_root, n): i
                   for i in range(n)}
        for f in concurrent.futures.as_completed(futures):
            i = futures[f]
            results[i] = f.result()

    for r in results:
        assert r is not None
        if r.rc != 0:
            raise RuntimeError(
                f"echobox-replay shard {r.idx:02d} failed with rc={r.rc}")
    print(f"replay: all {n} shards done in "
          f"{time.monotonic() - started:.1f}s", file=sys.stderr)

    rows: List[ReplayRow] = []
    for r in results:
        assert r is not None
        rows.extend(_collect_shard_rows(r, replay_root, cfg))
    return rows


# --- drop-safety assertion --------------------------------------------------

def assert_drop_free(rows) -> None:
    """Refuse to score any row whose subprocess reported sample drops.

    ``echobox-replay`` has drop-free backpressure by construction (see
    its capture loop docstring), so a "dropped" error would signal a
    regression in the C++ code — safest to fail loudly.
    """
    for r in rows:
        if "dropped" in (r.error or "").lower():
            raise RuntimeError(
                f"{r.source_file}: replay reported sample drops "
                f"({r.error!r}); refusing to score.")
