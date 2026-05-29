"""PyQt6 GUI for the validator.

Two operating modes selected at the top of the window:

  * Offline detection  — runs BandEnergyDetector locally on a chosen WAV and
                         draws annotation rectangles over the spectrogram.
                         No C++ binary involved.

  * ESP32 streaming    — sends a chosen WAV to a networked ESP32 (which acts
                         as the microphone for the production C++ binary
                         running on the Pi). The GUI is just the streamer;
                         detection / recording happens downstream on the Pi.
"""
from __future__ import annotations

import os
import sys
import threading
import traceback
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pyqtgraph as pg
from PyQt6.QtCore import (
    QObject, QRectF, Qt, QThread, pyqtSignal, pyqtSlot,
)
from PyQt6.QtGui import QAction
from PyQt6.QtWidgets import (
    QApplication, QButtonGroup, QCheckBox, QComboBox, QDoubleSpinBox,
    QFileDialog, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
    QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton,
    QRadioButton, QSpinBox, QStackedWidget, QStatusBar, QVBoxLayout, QWidget,
)

from . import detector as det_mod
from . import esp32_stream
from .dsp import HOP, NFFT, load_wav_float, stft_mags


# ---------------------------------------------------------------------------
# Offline mode
# ---------------------------------------------------------------------------

class _OfflineWorker(QObject):
    """Runs STFT + detector off the GUI thread (still synchronous within itself)."""
    finished = pyqtSignal(object)   # (sample_rate, mags, detections) or Exception
    progress = pyqtSignal(str)

    def __init__(self, wav_path: str, cfg: det_mod.DetectorConfig):
        super().__init__()
        self.wav_path = wav_path
        self.cfg = cfg

    @pyqtSlot()
    def run(self) -> None:
        try:
            self.progress.emit("Loading WAV…")
            sr, samples = load_wav_float(self.wav_path)
            self.progress.emit("Computing STFT…")
            mags = stft_mags(samples)
            # Re-instantiate the detector with the actual sample rate.
            cfg = self.cfg.__class__(**{**asdict(self.cfg), "sample_rate": sr})
            detector = det_mod.BandEnergyDetector(cfg)
            self.progress.emit("Running detector…")
            dets = detector.run_on_spectrogram(mags)
            self.finished.emit((sr, mags, dets))
        except Exception as e:   # noqa: BLE001 - surface anything to the UI
            traceback.print_exc()
            self.finished.emit(e)


class OfflineModeWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._wav_path: Optional[str] = None
        self._sample_rate: int = 0
        self._mags: Optional[np.ndarray] = None
        self._detections: list = []
        self._thread: Optional[QThread] = None
        self._worker: Optional[_OfflineWorker] = None

        self._build_ui()
        self._refresh_run_button()

    # -- UI ----------------------------------------------------------------

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)

        # Row 1: file picker + run + status
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

        # Row 2: detector params (compact, single form)
        params = QGroupBox("Detector parameters")
        form = QFormLayout(params)
        self.sp_lo  = QSpinBox(); self.sp_lo.setRange(0, 384000); self.sp_lo.setValue(20000)
        self.sp_hi  = QSpinBox(); self.sp_hi.setRange(0, 384000); self.sp_hi.setValue(190000)
        self.sp_snr = QDoubleSpinBox(); self.sp_snr.setRange(0.0, 200.0); self.sp_snr.setSingleStep(0.5); self.sp_snr.setValue(12.0)
        self.sp_minf = QDoubleSpinBox(); self.sp_minf.setRange(0.0, 1.0); self.sp_minf.setSingleStep(0.05); self.sp_minf.setDecimals(2); self.sp_minf.setValue(0.10)
        self.sp_maxf = QDoubleSpinBox(); self.sp_maxf.setRange(0.0, 1.0); self.sp_maxf.setSingleStep(0.05); self.sp_maxf.setDecimals(2); self.sp_maxf.setValue(0.75)
        self.sp_topk = QSpinBox(); self.sp_topk.setRange(1, 64); self.sp_topk.setValue(8)
        self.sp_min_act = QSpinBox(); self.sp_min_act.setRange(1, 100); self.sp_min_act.setValue(2)
        self.sp_hang = QSpinBox(); self.sp_hang.setRange(1, 200); self.sp_hang.setValue(8)
        form.addRow("freq_lo_hz",         self.sp_lo)
        form.addRow("freq_hi_hz",         self.sp_hi)
        form.addRow("band_snr_threshold", self.sp_snr)
        form.addRow("min_flatness",       self.sp_minf)
        form.addRow("max_flatness",       self.sp_maxf)
        form.addRow("top_k",              self.sp_topk)
        form.addRow("min_active_frames",  self.sp_min_act)
        form.addRow("hangover_frames",    self.sp_hang)
        root.addWidget(params)

        # Row 3: spectrogram view
        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Time", units="s")
        self.plot.setLabel("left",   "Frequency", units="kHz")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.getViewBox().setDefaultPadding(0.0)
        self.img = pg.ImageItem()
        self.img.setLookupTable(pg.colormap.get("inferno").getLookupTable())
        self.plot.addItem(self.img)
        self._boxes: list = []
        root.addWidget(self.plot, stretch=1)

        # Row 4: detections list + status
        self.lbl_status = QLabel("Idle.")
        root.addWidget(self.lbl_status)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True)
        self.log.setPlaceholderText("Detections will be listed here.")
        self.log.setMaximumHeight(160)
        root.addWidget(self.log)

    # -- handlers ----------------------------------------------------------

    def _refresh_run_button(self) -> None:
        self.btn_run.setEnabled(bool(self._wav_path) and self._thread is None)

    def _on_open(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Open WAV", "", "WAV files (*.wav)")
        if not path:
            return
        self._wav_path = path
        self.lbl_file.setText(Path(path).name)
        self.lbl_file.setStyleSheet("color: white")
        self._refresh_run_button()

    def _build_config(self) -> det_mod.DetectorConfig:
        return det_mod.DetectorConfig(
            freq_lo_hz=float(self.sp_lo.value()),
            freq_hi_hz=float(self.sp_hi.value()),
            band_snr_threshold=float(self.sp_snr.value()),
            min_flatness=float(self.sp_minf.value()),
            max_flatness=float(self.sp_maxf.value()),
            top_k=int(self.sp_topk.value()),
            min_active_frames=int(self.sp_min_act.value()),
            hangover_frames=int(self.sp_hang.value()),
        )

    def _on_run(self) -> None:
        if not self._wav_path or self._thread is not None:
            return
        if self.sp_lo.value() >= self.sp_hi.value():
            QMessageBox.warning(self, "Invalid range",
                                "freq_lo_hz must be less than freq_hi_hz.")
            return

        self.btn_run.setEnabled(False)
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
# ESP32 streaming mode
# ---------------------------------------------------------------------------

class _StreamWorker(QObject):
    progress = pyqtSignal(float, float)   # (progress_0_to_1, elapsed_s)
    finished = pyqtSignal(bool, str)      # (success, message)

    def __init__(self, host: str, port: int, wav_path: str, chunk_ms: int):
        super().__init__()
        self.host = host
        self.port = port
        self.wav_path = wav_path
        self.chunk_ms = chunk_ms
        self.stop_event = threading.Event()

    @pyqtSlot()
    def run(self) -> None:
        try:
            esp32_stream.stream_wav_to_esp32(
                host=self.host, port=self.port, wav_path=self.wav_path,
                chunk_ms=self.chunk_ms,
                on_progress=lambda p, e: self.progress.emit(p, e),
                stop_event=self.stop_event,
            )
            if self.stop_event.is_set():
                self.finished.emit(False, "Stopped by user.")
            else:
                self.finished.emit(True, "Streaming complete.")
        except Exception as e:   # noqa: BLE001
            self.finished.emit(False, str(e))

    def stop(self) -> None:
        self.stop_event.set()


class Esp32ModeWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._wav_path: Optional[str] = None
        self._thread: Optional[QThread] = None
        self._worker: Optional[_StreamWorker] = None
        self._build_ui()
        self._refresh_buttons()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)

        cfg = QGroupBox("ESP32")
        form = QFormLayout(cfg)
        self.ed_host = QLineEdit(); self.ed_host.setPlaceholderText("192.168.1.42")
        self.sp_port = QSpinBox(); self.sp_port.setRange(1, 65535)
        self.sp_port.setValue(esp32_stream.DEFAULT_PORT)
        self.sp_chunk = QSpinBox(); self.sp_chunk.setRange(1, 500)
        self.sp_chunk.setValue(20); self.sp_chunk.setSuffix(" ms")
        form.addRow("Host / IP",          self.ed_host)
        form.addRow("Port",               self.sp_port)
        form.addRow("Chunk duration",     self.sp_chunk)
        root.addWidget(cfg)

        file_row = QHBoxLayout()
        self.btn_open = QPushButton("Open WAV…")
        self.btn_open.clicked.connect(self._on_open)
        self.lbl_file = QLabel("<no file>")
        self.lbl_file.setStyleSheet("color: gray")
        file_row.addWidget(self.btn_open)
        file_row.addWidget(self.lbl_file, stretch=1)
        root.addLayout(file_row)

        ctl = QHBoxLayout()
        self.btn_stream = QPushButton("Stream")
        self.btn_stream.clicked.connect(self._on_stream)
        self.btn_stop = QPushButton("Stop")
        self.btn_stop.clicked.connect(self._on_stop)
        ctl.addWidget(self.btn_stream)
        ctl.addWidget(self.btn_stop)
        ctl.addStretch(1)
        root.addLayout(ctl)

        self.progress = QProgressBar()
        self.progress.setRange(0, 1000)
        self.progress.setFormat("%p%")
        root.addWidget(self.progress)

        self.log = QPlainTextEdit(); self.log.setReadOnly(True)
        self.log.setPlaceholderText("Status log will appear here.")
        root.addWidget(self.log, stretch=1)
        for line in (
            "Mode 2 streams a WAV file to a networked ESP32 over TCP.",
            "The ESP32 firmware is expected to act as the microphone for",
            "the production LiteSpectrum binary running on the Pi (USB-audio",
            "or I2S, depending on how the ESP32 is wired). See validator/README.md",
            "for the wire format the firmware needs to implement.",
        ):
            self.log.appendPlainText(line)

    def _refresh_buttons(self) -> None:
        streaming = self._thread is not None
        self.btn_stream.setEnabled(bool(self._wav_path)
                                   and bool(self.ed_host.text().strip())
                                   and not streaming)
        self.btn_stop.setEnabled(streaming)
        self.btn_open.setEnabled(not streaming)
        self.ed_host.setEnabled(not streaming)
        self.sp_port.setEnabled(not streaming)
        self.sp_chunk.setEnabled(not streaming)

    def _on_open(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Open WAV", "", "WAV files (*.wav)")
        if not path:
            return
        self._wav_path = path
        self.lbl_file.setText(Path(path).name)
        self.lbl_file.setStyleSheet("color: white")
        self._refresh_buttons()

    def _on_stream(self) -> None:
        if self._thread is not None or not self._wav_path:
            return
        host = self.ed_host.text().strip()
        if not host:
            QMessageBox.warning(self, "Missing host", "Enter an ESP32 host / IP.")
            return

        self.progress.setValue(0)
        self.log.appendPlainText("→ Connecting to %s:%d …" % (host, self.sp_port.value()))

        self._thread = QThread(self)
        self._worker = _StreamWorker(host, self.sp_port.value(),
                                     self._wav_path, self.sp_chunk.value())
        self._worker.moveToThread(self._thread)
        self._worker.progress.connect(self._on_progress)
        self._worker.finished.connect(self._on_finished)
        self._thread.started.connect(self._worker.run)
        self._thread.start()
        self._refresh_buttons()

    def _on_stop(self) -> None:
        if self._worker is not None:
            self._worker.stop()
            self.log.appendPlainText("→ Stop requested…")

    @pyqtSlot(float, float)
    def _on_progress(self, p: float, elapsed: float) -> None:
        self.progress.setValue(int(p * 1000))
        if int(p * 100) % 10 == 0:
            self.log.appendPlainText("  %5.1f%%  %.1fs" % (p * 100, elapsed))

    @pyqtSlot(bool, str)
    def _on_finished(self, ok: bool, msg: str) -> None:
        if self._thread is not None:
            self._thread.quit()
            self._thread.wait()
        self._thread = None
        self._worker = None
        self.log.appendPlainText("← " + msg)
        self._refresh_buttons()


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("LiteSpectrum Validator")
        self.resize(1100, 750)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # Mode selector
        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("Mode:"))
        self.rb_offline = QRadioButton("Offline detection")
        self.rb_stream  = QRadioButton("ESP32 stream")
        self.rb_offline.setChecked(True)
        grp = QButtonGroup(self)
        grp.addButton(self.rb_offline, 0)
        grp.addButton(self.rb_stream,  1)
        grp.idClicked.connect(self._switch_mode)
        mode_row.addWidget(self.rb_offline)
        mode_row.addWidget(self.rb_stream)
        mode_row.addStretch(1)
        root.addLayout(mode_row)

        # Stacked mode pages
        self.stack = QStackedWidget()
        self.page_offline = OfflineModeWidget()
        self.page_stream  = Esp32ModeWidget()
        self.stack.addWidget(self.page_offline)
        self.stack.addWidget(self.page_stream)
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
