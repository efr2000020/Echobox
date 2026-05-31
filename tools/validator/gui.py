"""PyQt6 GUI for the validator.

Two operating modes selected at the top of the window:

  * Offline detection  — runs the active detector locally on a chosen WAV
                         and draws annotation rectangles over the spectrogram.
                         The detector and its tunable form are discovered
                         dynamically from the native validator library, so
                         the GUI is algorithm-agnostic.
  * ALSA Loopback      — streams a WAV into snd-aloop so the production C++
                         binary captures it as if from a real microphone.
"""
from __future__ import annotations

import math
import os
import sys
import dataclasses
import traceback
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np
import pyqtgraph as pg
from PyQt6.QtCore import (
    QObject, QRectF, Qt, QThread, pyqtSignal, pyqtSlot,
)
from PyQt6.QtWidgets import (
    QApplication, QButtonGroup, QCheckBox, QComboBox, QDoubleSpinBox,
    QFileDialog, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
    QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton,
    QRadioButton, QScrollArea, QSizePolicy, QSpinBox, QSplitter, QStackedWidget,
    QStatusBar, QVBoxLayout, QWidget,
)

from . import alsa_stream
from . import native
from .dsp import HOP, NFFT, load_wav_float


# ---------------------------------------------------------------------------
# ALSA Loopback mode (formerly fake-mic)
# ---------------------------------------------------------------------------

class _AlsaWorker(QObject):
    """Feeds the WAV into the loopback device on a worker thread."""
    progress = pyqtSignal(int, int)      # (frames_done, total_frames)
    message  = pyqtSignal(str)
    failed   = pyqtSignal(str)
    finished = pyqtSignal()

    def __init__(self, wav_path: str, device: str, loop: bool):
        super().__init__()
        self.wav_path = wav_path
        self.device   = device
        self.loop     = loop
        self._stop    = False

    def stop(self) -> None:
        self._stop = True

    @pyqtSlot()
    def run(self) -> None:
        try:
            alsa_stream.stream_wav_to_alsa(
                self.wav_path, self.device, self.loop,
                on_progress=lambda d, t: self.progress.emit(d, t),
                is_stopping=lambda: self._stop
            )
        except Exception as e:
            self.failed.emit(str(e))
            return
        self.finished.emit()


class LoopbackModeWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._card: Optional[str] = None
        self._wav_path: str = ""
        self._thread: Optional[QThread] = None
        self._worker: Optional[_AlsaWorker] = None

        self._build_ui()
        self.refresh_device()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # Device status
        self.status_lbl = QLabel()
        self.status_lbl.setWordWrap(True)
        self.status_lbl.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.status_lbl)

        recheck = QPushButton("Re-check device")
        recheck.clicked.connect(self.refresh_device)
        layout.addWidget(recheck)

        # File picker
        file_row = QHBoxLayout()
        self.path_edit = QLineEdit()
        self.path_edit.setPlaceholderText("Choose a 384 kHz / S16 / mono WAV…")
        self.path_edit.setReadOnly(True)
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._on_browse)
        file_row.addWidget(self.path_edit)
        file_row.addWidget(browse)
        layout.addLayout(file_row)

        self.wav_lbl = QLabel("No file selected.")
        self.wav_lbl.setWordWrap(True)
        layout.addWidget(self.wav_lbl)

        # Controls
        ctrl = QHBoxLayout()
        self.play_btn = QPushButton("Play")
        self.play_btn.clicked.connect(self._on_play)
        self.stop_btn = QPushButton("Stop")
        self.stop_btn.clicked.connect(self._on_stop)
        self.stop_btn.setEnabled(False)
        self.loop_chk = QCheckBox("Loop")
        self.loop_chk.setChecked(True)
        ctrl.addWidget(self.play_btn)
        ctrl.addWidget(self.stop_btn)
        ctrl.addWidget(self.loop_chk)
        ctrl.addStretch(1)
        layout.addLayout(ctrl)

        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        layout.addWidget(self.progress)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        layout.addWidget(self.log, 1)

    def refresh_device(self) -> None:
        self._card = alsa_stream.detect_loopback_card()
        if self._card is None:
            self.status_lbl.setText(
                "<b style='color:#c0392b'>snd-aloop not loaded.</b><br>"
                "Run once (in this session, prefix with <code>!</code>):<br>"
                "<code>sudo modprobe snd-aloop id=UltraMic384K</code>")
            self.play_btn.setEnabled(False)
            return

        cap = alsa_stream.CAPTURE_DEV.format(card=self._card)
        note = "" if self._card == alsa_stream.CARD_ID else (
            f"  (loaded as '{self._card}', not '{alsa_stream.CARD_ID}' — "
            f"reload with id={alsa_stream.CARD_ID} for name parity with the Pi)")
        
        self.status_lbl.setText(
            f"<b style='color:#27ae60'>Loopback ready:</b> card '{self._card}'.{note}<br>"
            f"Point the app at:<br><code>./deploy/bin/Echobox --device {cap}</code>")
        self.play_btn.setEnabled(bool(self._wav_path))

    def _on_browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select WAV", "", "WAV files (*.wav)")
        if not path:
            return
        self._wav_path = path
        self.path_edit.setText(path)
        
        try:
            import wave
            with wave.open(path, "rb") as w:
                info = alsa_stream.AlsaWavInfo(
                    rate=w.getframerate(), channels=w.getnchannels(),
                    width=w.getsampwidth(), frames=w.getnframes())
        except Exception as e:
            self.wav_lbl.setText(f"<span style='color:#c0392b'>Cannot read WAV: {e}</span>")
            self.play_btn.setEnabled(False)
            return

        fmt = (f"{info.rate} Hz, {info.width * 8}-bit, "
               f"{'mono' if info.channels == 1 else f'{info.channels}ch'}, "
               f"{info.duration_s:.2f}s")
        if info.is_production_format:
            self.wav_lbl.setText(f"<b>{fmt}</b> — matches the Ultramic format.")
        else:
            self.wav_lbl.setText(
                f"<b>{fmt}</b><br><span style='color:#d35400'>"
                "Warning: not 384 kHz/S16/mono. It will play at its own rate, "
                "but won't exercise the production 384 kHz capture path.</span>")
        self.play_btn.setEnabled(self._card is not None)

    def _on_play(self) -> None:
        if not self._wav_path or self._card is None:
            return
        
        device = alsa_stream.PLAYBACK_DEV.format(card=self._card)
        self._thread = QThread(self)
        self._worker = _AlsaWorker(self._wav_path, device, self.loop_chk.isChecked())
        self._worker.moveToThread(self._thread)
        self._worker.progress.connect(self._on_progress)
        self._worker.failed.connect(self._on_failed)
        self._worker.finished.connect(self._on_finished)
        self._thread.started.connect(self._worker.run)
        self._thread.start()

        self.play_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self._log(f"Playing {Path(self._wav_path).name}"
                  f"{' (looping)' if self.loop_chk.isChecked() else ''}")

    def _on_stop(self) -> None:
        if self._worker:
            self._worker.stop()
        self.stop_btn.setEnabled(False)
        self._log("Stopping…")

    def _on_progress(self, done: int, total: int) -> None:
        self.progress.setValue(int(done * 100 / total) if total else 0)

    def _on_failed(self, msg: str) -> None:
        self._log("ERROR: " + msg)
        self._teardown()

    def _on_finished(self) -> None:
        self._log("Done.")
        self._teardown()

    def _teardown(self) -> None:
        if self._thread:
            self._thread.quit()
            self._thread.wait(2000)
            self._thread = None
        self._worker = None
        self.progress.setValue(0)
        self.play_btn.setEnabled(bool(self._wav_path) and self._card is not None)
        self.stop_btn.setEnabled(False)

    def _log(self, msg: str) -> None:
        self.log.appendPlainText(msg)


# ---------------------------------------------------------------------------
# Offline mode
# ---------------------------------------------------------------------------

class _OfflineWorker(QObject):
    """Runs STFT + detector off the GUI thread (still synchronous within itself)."""
    finished = pyqtSignal(object)   # (sample_rate, mags, detections) or Exception
    progress = pyqtSignal(str)

    def __init__(self, wav_path: str, cfg: native.DetectorConfig):
        super().__init__()
        self.wav_path = wav_path
        self.cfg = cfg

    @pyqtSlot()
    def run(self) -> None:
        try:
            self.progress.emit("Loading WAV…")
            sr, samples = load_wav_float(self.wav_path)
            self.progress.emit("Computing STFT…")
            mags = native.stft(samples, sr, hpf_cutoff_hz=20000.0)
            # Re-instantiate the detector with the actual sample rate.
            cfg = dataclasses.replace(self.cfg, sample_rate=sr)
            detector = native.Detector(cfg)
            self.progress.emit("Running detector…")
            dets = detector.run_on_spectrogram(mags)
            self.finished.emit((sr, mags, dets))
        except Exception as e:   # noqa: BLE001 - surface anything to the UI
            traceback.print_exc()
            self.finished.emit(e)


def _float_step_and_decimals(default: float, max_value: float) -> tuple[float, int]:
    """Pick a sensible singleStep/decimals for a float tunable.

    Drives QDoubleSpinBox UX without baking algorithm-specific values into the
    GUI: small-magnitude defaults (e.g. 1e-6) get fine steps and many decimals;
    large-range knobs get coarse steps and few. Users can always type any value.
    """
    magnitude = max(abs(default), 1e-12)
    order = math.floor(math.log10(magnitude))
    if order <= -3:
        return 10 ** (order - 1), max(2, -order + 2)
    if magnitude < 0.1:
        return 0.01, 4
    if max_value <= 10.0:
        return 0.05, 3
    return 0.5, 2


class OfflineModeWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._wav_path: Optional[str] = None
        self._sample_rate: int = 0
        self._mags: Optional[np.ndarray] = None
        self._detections: list = []
        self._thread: Optional[QThread] = None
        self._worker: Optional[_OfflineWorker] = None
        # algorithm name -> {tunable key -> widget}. Populated when the
        # algorithm picker changes; widgets are owned by the form layout.
        self._tunable_widgets: Dict[str, QWidget] = {}
        self._algorithm_count: int = 0   # 0 = none registered, drives both pickers

        self._build_ui()
        self._populate_algorithms()
        self._refresh_run_button()

    # -- UI ----------------------------------------------------------------

    def _build_ui(self) -> None:
        # Layout: a thin global action row on top, then a horizontal splitter
        # whose left pane is a scrollable controls column and right pane is a
        # vertical splitter that gives the spectrogram the bulk of the space
        # with the detection log tucked below it. Both splitters are user-
        # draggable so the layout adapts to whatever the user values most.
        root = QVBoxLayout(self)

        # Global action row: file picker + run button, always at the top.
        file_row = QHBoxLayout()
        self.btn_open = QPushButton("Open WAV…")
        self.btn_open.clicked.connect(self._on_open)
        self.lbl_file = QLabel("<no file>")
        self.lbl_file.setStyleSheet("color: gray")
        self.btn_run = QPushButton("Run Detection")
        self.btn_run.clicked.connect(self._on_run)
        file_row.addWidget(self.btn_open)
        file_row.addWidget(self.lbl_file, stretch=1)
        file_row.addWidget(self.btn_run)
        root.addLayout(file_row)

        main_split = QSplitter(Qt.Orientation.Horizontal)

        # --- Left pane: scrollable controls column ------------------------
        # Wrapping the controls in a QScrollArea means a tall tunable manifest
        # never dictates the window's minimum height, and the spectrogram
        # never gets squeezed to make room for an extra knob.
        controls = QWidget()
        controls_layout = QVBoxLayout(controls)
        controls_layout.setContentsMargins(0, 0, 0, 0)

        alg_row = QHBoxLayout()
        alg_row.addWidget(QLabel("Algorithm:"))
        self.cmb_algorithm = QComboBox()
        self.cmb_algorithm.currentTextChanged.connect(self._on_algorithm_changed)
        alg_row.addWidget(self.cmb_algorithm, stretch=1)
        controls_layout.addLayout(alg_row)

        # EXPERIMENTAL: sensitivity preset dropdown. Picking a preset loads
        # the algorithm's bundle of tunable values into the form below so
        # the user can still tweak individual knobs after a preset.
        preset_row = QHBoxLayout()
        preset_row.addWidget(QLabel("Sensitivity (experimental):"))
        self.cmb_preset = QComboBox()
        self.cmb_preset.currentTextChanged.connect(self._on_preset_changed)
        preset_row.addWidget(self.cmb_preset, stretch=1)
        controls_layout.addLayout(preset_row)

        # Detection window (algorithm-independent — configure() args, not tunables).
        window = QGroupBox("Detection window")
        win_form = QFormLayout(window)
        self.sp_lo = QSpinBox(); self.sp_lo.setRange(0, 384000); self.sp_lo.setValue(20000)
        self.sp_hi = QSpinBox(); self.sp_hi.setRange(0, 384000); self.sp_hi.setValue(190000)
        win_form.addRow("freq_lo_hz", self.sp_lo)
        win_form.addRow("freq_hi_hz", self.sp_hi)
        controls_layout.addWidget(window)

        # Dynamic tunable form, rebuilt whenever the algorithm changes.
        self.params_box  = QGroupBox("Algorithm tunables")
        self.params_form = QFormLayout(self.params_box)
        controls_layout.addWidget(self.params_box)
        controls_layout.addStretch(1)   # keep groups top-aligned in tall windows

        controls_scroll = QScrollArea()
        controls_scroll.setWidget(controls)
        controls_scroll.setWidgetResizable(True)
        controls_scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        controls_scroll.setMinimumWidth(240)
        main_split.addWidget(controls_scroll)

        # --- Right pane: spectrogram (big) + log (small) ------------------
        right_split = QSplitter(Qt.Orientation.Vertical)

        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Time", units="s")
        self.plot.setLabel("left",   "Frequency", units="kHz")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.getViewBox().setDefaultPadding(0.0)
        self.plot.setSizePolicy(QSizePolicy.Policy.Expanding,
                                QSizePolicy.Policy.Expanding)
        self.img = pg.ImageItem()
        self.img.setLookupTable(pg.colormap.get("inferno").getLookupTable())
        self.plot.addItem(self.img)
        self._boxes: list = []
        right_split.addWidget(self.plot)

        log_panel = QWidget()
        log_layout = QVBoxLayout(log_panel)
        log_layout.setContentsMargins(0, 0, 0, 0)
        self.lbl_status = QLabel("Idle.")
        log_layout.addWidget(self.lbl_status)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True)
        self.log.setPlaceholderText("Detections will be listed here.")
        log_layout.addWidget(self.log)
        right_split.addWidget(log_panel)
        right_split.setStretchFactor(0, 5)   # plot dominates the right column
        right_split.setStretchFactor(1, 1)
        right_split.setSizes([520, 140])

        main_split.addWidget(right_split)
        main_split.setStretchFactor(0, 0)    # controls don't grow on resize
        main_split.setStretchFactor(1, 1)    # spectrogram column gets the room
        main_split.setSizes([280, 820])
        main_split.setChildrenCollapsible(False)

        root.addWidget(main_split, stretch=1)

    # -- algorithm + form wiring ------------------------------------------

    def _populate_algorithms(self) -> None:
        try:
            algorithms = native.list_algorithms()
        except Exception as e:
            algorithms = []
            self.lbl_status.setText("Native lib error: %s" % e)
        self._algorithm_count = len(algorithms)
        self.cmb_algorithm.blockSignals(True)
        self.cmb_algorithm.clear()
        if algorithms:
            self.cmb_algorithm.addItems(algorithms)
            self.cmb_algorithm.setEnabled(len(algorithms) > 1)
        else:
            self.cmb_algorithm.addItem("<none — run ./build_dev.sh "
                                       "or set ECHOBOX_ALGORITHMS_DIR>")
            self.cmb_algorithm.setEnabled(False)
        self.cmb_algorithm.blockSignals(False)
        if algorithms:
            self._rebuild_tunable_form(algorithms[0])

    def _rebuild_tunable_form(self, algorithm: str) -> None:
        # Tear down any existing rows. takeRow removes both label + field.
        while self.params_form.rowCount() > 0:
            self.params_form.removeRow(0)
        self._tunable_widgets.clear()

        try:
            tunables = native.list_tunables(algorithm=algorithm)
        except Exception as e:
            self.lbl_status.setText("Cannot describe '%s': %s" % (algorithm, e))
            return

        for t in tunables:
            if t.is_int:
                w = QSpinBox()
                w.setRange(int(t.min), int(t.max))
                w.setValue(int(t.default))
            else:
                w = QDoubleSpinBox()
                w.setRange(float(t.min), float(t.max))
                step, decimals = _float_step_and_decimals(t.default, t.max)
                w.setSingleStep(step)
                w.setDecimals(decimals)
                w.setValue(float(t.default))
            if t.doc:
                w.setToolTip(t.doc)
            self.params_form.addRow(t.key, w)
            self._tunable_widgets[t.key] = w

        self._repopulate_preset_dropdown(algorithm)

    def _repopulate_preset_dropdown(self, algorithm: str) -> None:
        # The first entry is always "<none>" so the user can opt out of any
        # preset and edit tunables freely. After "<none>", entries come from
        # the algorithm's own listPresets().
        try:
            presets = native.list_presets(algorithm=algorithm)
        except Exception:
            presets = []
        self.cmb_preset.blockSignals(True)
        self.cmb_preset.clear()
        self.cmb_preset.addItem("<none>")
        for p in presets:
            self.cmb_preset.addItem(p.name)
            if p.doc:
                self.cmb_preset.setItemData(
                    self.cmb_preset.count() - 1, p.doc, Qt.ItemDataRole.ToolTipRole)
        self.cmb_preset.setEnabled(len(presets) > 0)
        self.cmb_preset.blockSignals(False)

    def _on_algorithm_changed(self, name: str) -> None:
        if name and self._algorithm_count > 0:
            self._rebuild_tunable_form(name)

    def _on_preset_changed(self, name: str) -> None:
        # "<none>" means "don't apply a preset" — leave the spinboxes alone.
        if not name or name.startswith("<") or self._algorithm_count == 0:
            return
        algorithm = self.cmb_algorithm.currentText()
        try:
            values = native.preset_tunables(algorithm=algorithm, preset=name)
        except Exception as e:
            self.lbl_status.setText("Cannot apply preset '%s': %s" % (name, e))
            return
        # Push the preset's tunable values into the existing spinboxes so the
        # user sees (and can tweak) exactly what changed.
        for key, value in values.items():
            w = self._tunable_widgets.get(key)
            if w is None:
                continue
            if isinstance(w, QSpinBox):
                w.setValue(int(round(value)))
            else:
                w.setValue(float(value))
        self.lbl_status.setText("Applied preset '%s'." % name)

    # -- handlers ----------------------------------------------------------

    def _refresh_run_button(self) -> None:
        self.btn_run.setEnabled(
            bool(self._wav_path)
            and self._thread is None
            and self._algorithm_count > 0
        )

    def _on_open(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Open WAV", "", "WAV files (*.wav)")
        if not path:
            return
        self._wav_path = path
        self.lbl_file.setText(Path(path).name)
        self.lbl_file.setStyleSheet("color: white")
        self._refresh_run_button()

    def _build_config(self) -> native.DetectorConfig:
        tunables: Dict[str, float] = {}
        for key, w in self._tunable_widgets.items():
            tunables[key] = float(w.value())
        algorithm = self.cmb_algorithm.currentText()
        # Preset values are already baked into the spinboxes via
        # _on_preset_changed, so we don't pass sensitivity here — that would
        # apply the preset a second time and could fight a tweak the user
        # made after picking the preset. The tunables dict is the truth.
        return native.DetectorConfig(
            algorithm=algorithm,
            freq_lo_hz=float(self.sp_lo.value()),
            freq_hi_hz=float(self.sp_hi.value()),
            tunables=tunables,
        )

    def _on_run(self) -> None:
        if not self._wav_path or self._thread is not None:
            return
        if self.sp_lo.value() >= self.sp_hi.value():
            QMessageBox.warning(self, "Invalid range",
                                "freq_lo_hz must be less than freq_hi_hz.")
            return

        self.btn_run.setEnabled(False)
        self.cmb_algorithm.setEnabled(False)
        self.log.clear()
        self.lbl_status.setText("Running…")

        self._thread = QThread(self)
        self._worker = _OfflineWorker(self._wav_path, self._build_config())
        self._worker.moveToThread(self._thread)
        self._worker.progress.connect(self.lbl_status.setText)
        self._worker.finished.connect(self._on_finished)
        self._thread.started.connect(self._worker.run)
        self._thread.start()

    @pyqtSlot(object)
    def _on_finished(self, result) -> None:
        # Tear down worker thread.
        if self._thread is not None:
            self._thread.quit()
            self._thread.wait()
        self._thread = None
        self._worker = None
        # Re-enable the algorithm picker iff there's actually a choice to make.
        self.cmb_algorithm.setEnabled(self._algorithm_count > 1)
        self._refresh_run_button()

        if isinstance(result, Exception):
            self.lbl_status.setText("Error: %s" % result)
            QMessageBox.critical(self, "Detection failed", str(result))
            return

        sr, mags, dets = result
        self._sample_rate = sr
        self._mags = mags
        self._detections = dets
        self._render(sr, mags, dets)
        self.lbl_status.setText(
            "Done. %d frames, %d detections." % (mags.shape[0], len(dets)))

    # -- rendering ---------------------------------------------------------

    def _render(self, sr: int, mags: np.ndarray, dets: list) -> None:
        # Spectrogram (frames on x, bins on y → swap to time on x, freq on y).
        S_db = 20.0 * np.log10(mags + 1e-9)
        self.img.setImage(S_db, autoLevels=False, levels=(-100, -30))
        time_axis = mags.shape[0] * HOP / sr
        nyquist_khz = sr / 2.0 / 1000.0
        self.img.setRect(QRectF(0.0, 0.0, time_axis, nyquist_khz))
        self.plot.setXRange(0, time_axis, padding=0)
        self.plot.setYRange(0, nyquist_khz, padding=0)

        for box in self._boxes:
            self.plot.removeItem(box)
        self._boxes.clear()

        secs_per_frame = HOP / sr
        for e in dets:
            x0 = e.start_frame * secs_per_frame
            x1 = max(e.end_frame * secs_per_frame, x0 + secs_per_frame)
            y0 = e.lo_hz / 1000.0
            y1 = e.hi_hz / 1000.0
            box = pg.RectROI([x0, y0], [x1 - x0, y1 - y0],
                             pen=pg.mkPen("g", width=2),
                             movable=False, rotatable=False, resizable=False)
            for h in box.getHandles():
                box.removeHandle(h)
            self.plot.addItem(box)
            self._boxes.append(box)

        # Update text log.
        lines = ["%3d:  t=%6.2f-%6.2fs   %5.1f-%5.1f kHz"
                 % (i, e.start_frame * secs_per_frame, e.end_frame * secs_per_frame,
                    e.lo_hz / 1000.0, e.hi_hz / 1000.0)
                 for i, e in enumerate(dets, 1)]
        self.log.setPlainText("\n".join(lines))

# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Echobox Validator")
        self.resize(1100, 750)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # Mode selector
        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("Mode:"))
        self.rb_offline = QRadioButton("Offline detection")
        self.rb_alsa    = QRadioButton("ALSA Loopback (fake-mic)")
        
        self.rb_offline.setChecked(True)
        grp = QButtonGroup(self)
        grp.addButton(self.rb_offline, 0)
        grp.addButton(self.rb_alsa,    1)
        grp.idClicked.connect(self._switch_mode)
        
        mode_row.addWidget(self.rb_offline)
        mode_row.addWidget(self.rb_alsa)
        mode_row.addStretch(1)
        root.addLayout(mode_row)

        # Stacked mode pages
        self.stack = QStackedWidget()
        self.page_offline = OfflineModeWidget()
        self.page_alsa    = LoopbackModeWidget()
        
        self.stack.addWidget(self.page_offline)
        self.stack.addWidget(self.page_alsa)
        root.addWidget(self.stack, stretch=1)

        # Status bar
        sb = QStatusBar()
        self.setStatusBar(sb)
        sb.showMessage("Ready.")

    def _switch_mode(self, index: int) -> None:
        self.stack.setCurrentIndex(index)


def main(argv=None) -> int:
    # Allow running from anywhere; pyqtgraph picks a sensible Qt API config.
    os.environ.setdefault("QT_QPA_PLATFORM_PLUGIN_PATH",
                          os.environ.get("QT_QPA_PLATFORM_PLUGIN_PATH", ""))
    app = QApplication(argv if argv is not None else sys.argv)
    pg.setConfigOptions(antialias=True, useOpenGL=False)
    win = MainWindow()
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
