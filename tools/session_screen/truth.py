# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""BatDetect2 wrapper for the ``truth`` stage.

Runs BatDetect2 over every WAV in an input directory and caches the
per-file result as a parquet manifest. See ``score.py`` and the report
caveats — BatDetect2 is NOT ground truth, it's a screen.

Resampling: BatDetect2 resamples input to ``api.TARGET_SAMPLERATE_HZ``
(256 kHz). Our capture is 384 kHz, so every file goes through a
down-resample inside the library. The exact resample rate lands in the
manifest so the report doesn't hide it.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional

from .manifest import TruthRow, encode_json_list, write_truth_manifest


DEFAULT_DETECTION_THRESHOLD = 0.5
"""BatDetect2's own default is used unless the caller overrides. Bumping
this trades recall for precision; the report is a screen, so we err on
the side of BatDetect2's ship-default and let a human eyeball the
disagreements CSV."""


def _import_batdetect2():
    """Import BatDetect2 lazily so ``score``/``replay`` can run in a venv
    that never installed the ML dep. Raises a plain-English error the
    CLI surfaces without a traceback."""
    try:
        from batdetect2 import api          # noqa: WPS433
        return api
    except ImportError as e:  # pragma: no cover - covered at CLI level
        raise ImportError(
            "BatDetect2 is not installed. Install with "
            "'pip install -r tools/session_screen/requirements.txt' "
            "(pulls torch — ~2 GB). This is the only dep for the 'truth' "
            "stage; 'replay' and 'score' work without it."
        ) from e


def _model_hash(model_path: Path) -> str:
    """SHA-256 of the model checkpoint, truncated to 16 hex chars.

    Written into every truth row so a report can flag mixed caches (two
    runs against different model files) — non-invertible, cheap.
    """
    h = hashlib.sha256()
    with open(model_path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


@dataclass
class TruthConfig:
    """Knobs for the truth stage. Defaults reproduce BatDetect2's own
    ship defaults; ``detection_threshold`` is the only one most tuning
    sessions will touch."""
    detection_threshold: float = DEFAULT_DETECTION_THRESHOLD
    device:             str   = "auto"    # "auto" | "cpu" | "cuda"


def _resolve_device(device: str):
    api = _import_batdetect2()
    import torch  # noqa: WPS433 - only reached after batdetect2 loaded
    if device == "auto":
        if torch.cuda.is_available():
            return torch.device("cuda")
        return torch.device("cpu")
    return torch.device(device)


def _run_one(wav_path: Path, api, model, config, device,
             model_hash: str, error_log: Optional[Path] = None) -> TruthRow:
    """Call BatDetect2 on one file and shape the result into a TruthRow.

    Any exception (unreadable WAV, torch OOM, etc.) is caught and turned
    into an error string on the row so the batch keeps going. That
    tradeoff — logging vs raising — is what makes 'run once at night,
    inspect in the morning' viable on a 585-file corpus.

    ``error_log``, if provided, gets the full traceback appended per
    failure so a post-mortem doesn't have to reproduce the crash — the
    per-row error string alone (``IndexError: list index out of range``)
    tells you nothing about where in BatDetect2 it fired.

    The row's ``file`` field stores the WAV **basename** — score.py
    joins truth ↔ replay by basename, so the parquet content matches the
    join key and stays valid if the dataset is later moved on disk.
    """
    file_key = wav_path.name
    display  = str(wav_path)
    try:
        result = api.process_file(
            display, model=model, config=config, device=device)
    except Exception as e:
        if error_log is not None:
            import traceback as _tb                                     # noqa: WPS433
            with open(error_log, "a") as f:
                f.write(f"\n===== {display} =====\n")
                _tb.print_exc(file=f)
        return TruthRow(
            file=file_key, duration_s=0.0, bat_present=False,
            n_detections=0, top_species="", top_confidence=0.0,
            detections_json="[]",
            resample_hz=int(api.TARGET_SAMPLERATE_HZ),
            model_hash=model_hash,
            error=f"{type(e).__name__}: {e}",
        )
    return _shape_result(file_key, result, api, model_hash)


def _shape_result(file_str: str, result: dict, api, model_hash: str) -> TruthRow:
    """Extract the fields we care about from BatDetect2's RunResults."""
    pred = result.get("pred_dict") or {}
    annotations = pred.get("annotation") or []
    duration_s  = float(pred.get("duration") or 0.0)

    detections = []
    top_species    = ""
    top_confidence = 0.0
    for a in annotations:
        # BatDetect2's ``det_prob`` is the presence-probability we score
        # against; ``class_prob`` is the species-conditional probability.
        conf = float(a.get("det_prob", 0.0))
        detections.append({
            "start_time_s": float(a.get("start_time", 0.0)),
            "end_time_s":   float(a.get("end_time",   0.0)),
            "low_freq_hz":  int(a.get("low_freq",  0)),
            "high_freq_hz": int(a.get("high_freq", 0)),
            "confidence":   conf,
            "species":      str(a.get("class", "")),
        })
        if conf > top_confidence:
            top_confidence = conf
            top_species    = str(a.get("class", ""))

    return TruthRow(
        file=file_str,
        duration_s=duration_s,
        bat_present=len(detections) > 0,
        n_detections=len(detections),
        top_species=top_species,
        top_confidence=top_confidence,
        detections_json=encode_json_list(detections),
        resample_hz=int(api.TARGET_SAMPLERATE_HZ),
        model_hash=model_hash,
    )


def iter_input_wavs(input_dir: Path) -> List[Path]:
    return sorted(p for p in input_dir.rglob("*.wav") if p.is_file())


CHECKPOINT_EVERY = 25
"""Rewrite the manifest every N rows so a kill or crash mid-stage doesn't
lose the whole run. Cheap (a 585-row parquet is a few hundred KB); if
your run has extreme per-file variance and 25 feels off, tune via the
``checkpoint_every`` parameter."""


def run_truth(input_dir: Path, output_path: Path, *,
              config: Optional[TruthConfig] = None,
              progress: Optional[callable] = None,
              checkpoint_every: int = CHECKPOINT_EVERY,
              limit: Optional[int] = None) -> Path:
    """Run BatDetect2 over every WAV in ``input_dir`` and write the
    truth manifest to ``output_path``. Returns ``output_path``.

    Model + config are loaded exactly once and reused across files —
    the fixed cost is the checkpoint load (~1 s on CPU), and the
    per-file cost is small enough that batching gives no additional
    speedup here. GPU is picked up automatically if visible.

    ``checkpoint_every`` controls how often the in-flight manifest is
    flushed to disk (as ``<output>.partial``) so a mid-run kill leaves
    something behind. On successful completion the partial is atomically
    renamed to ``output_path``.

    ``limit``, if set, processes only the first N files — useful for a
    fast smoke test before committing hours of CPU.
    """
    import sys as _sys                                                # noqa: WPS433
    api = _import_batdetect2()
    cfg = config or TruthConfig()

    print(f"truth: loading BatDetect2 model on {cfg.device} ...", file=_sys.stderr)
    device = _resolve_device(cfg.device)
    model, params = api.load_model(device=device)

    # BatDetect2 gotcha: DEFAULT_PROCESSING_CONFIGURATIONS['class_names']
    # is an empty list on disk. Spreading it into get_config() overrides
    # the real 17-species list stored in the model checkpoint, and every
    # file that fires ANY detection then hits IndexError in
    # utils/detector_utils.format_single_result — masked ~74% of the
    # corpus on our first run, including every bat-positive file.
    proc_cfg = api.get_config(
        detection_threshold=cfg.detection_threshold,
        class_names=list(params["class_names"]),
    )
    model_hash = _model_hash(Path(api.DEFAULT_MODEL_PATH))

    wavs = iter_input_wavs(input_dir)
    if limit is not None:
        wavs = wavs[:max(0, limit)]
    print(f"truth: model loaded (hash={model_hash}); {len(wavs)} WAVs; "
          f"threshold={cfg.detection_threshold}; "
          f"checkpoint every {checkpoint_every} files → "
          f"{output_path.name}.partial",
          file=_sys.stderr)

    partial   = output_path.with_suffix(output_path.suffix + ".partial")
    error_log = output_path.with_suffix(output_path.suffix + ".errors.txt")
    if error_log.exists():
        error_log.unlink()
    rows: List[TruthRow] = []
    for i, wav in enumerate(wavs):
        row = _run_one(wav, api, model, proc_cfg, device, model_hash,
                       error_log=error_log)
        rows.append(row)
        if progress is not None:
            extra = _truth_row_summary(row)
            progress(i + 1, len(wavs), wav, extra)
        if checkpoint_every > 0 and (i + 1) % checkpoint_every == 0:
            write_truth_manifest(partial, rows)
            print(f"\ntruth: checkpoint → {partial} ({i + 1}/{len(wavs)})",
                  file=_sys.stderr)

    write_truth_manifest(partial, rows)
    partial.replace(output_path)
    return output_path


def _truth_row_summary(row: TruthRow) -> str:
    """One-glance per-file result string for the progress line."""
    if row.error:
        return f"ERROR: {row.error[:40]}"
    if row.n_detections == 0:
        return "no bat"
    species = row.top_species[:24] if row.top_species else "?"
    return f"{row.n_detections} det, top={species} p={row.top_confidence:.2f}"
