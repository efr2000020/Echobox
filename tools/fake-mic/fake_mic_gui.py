#!/usr/bin/env python3
"""Fake Ultramic — feed a WAV into an ALSA loopback so LiteSpectrum captures it
as if a real Dodotronic Ultramic 384K were plugged in.

The production binary opens its mic through ALSA (`snd_pcm_open` on a `--device`
name). ALSA hides USB entirely, so a virtual `snd-aloop` capture device at
384 kHz / S16_LE / mono is indistinguishable from the real mic to the app.

This GUI is just the *producer* side: it writes a chosen WAV into the loopback
playback endpoint, real-time paced (ALSA blocking writes are clocked by the
device), with optional looping so the "mic" runs continuously. Point the app at
the matching capture endpoint and you exercise the real capture -> DSP ->
recorder path with no hardware.

One-time setup (needs root once per boot; no passwordless sudo here):

    sudo modprobe snd-aloop id=UltraMic384K

Then run this GUI, Browse to a 384 kHz WAV, hit Play, and in another terminal:

    ./deploy/bin/LiteSpectrum --device plughw:UltraMic384K,1,0
"""
from __future__ import annotations

import sys
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import alsaaudio
from PyQt6.QtCore import QObject, QThread, Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QApplication, QCheckBox, QFileDialog, QHBoxLayout, QLabel, QLineEdit,
    QMainWindow, QPlainTextEdit, QProgressBar, QPushButton, QVBoxLayout, QWidget,
)

# --- contract with the production app --------------------------------------
# snd-aloop pairs its two PCM devices: whatever is *played* to device 0 is
# *captured* on device 1 (same subdevice), and vice-versa. So we write to
# device 0 and the app reads device 1.
CARD_ID         = "UltraMic384K"          # set via `modprobe snd-aloop id=...`
FALLBACK_CARD   = "Loopback"              # default id if loaded without id=
PLAYBACK_DEV    = "hw:{card},0,0"         # we feed here
CAPTURE_DEV     = "plughw:{card},1,0"     # app's --device

TARGET_RATE     = 384_000
TARGET_CH       = 1
TARGET_WIDTH    = 2                       # bytes -> S16_LE
PERIOD_FRAMES   = 1920                    # 5 ms @ 384 kHz


@dataclass
class WavInfo:
    rate: int
    channels: int
    width: int          # bytes per sample
    frames: int

    @property
    def duration_s(self) -> float:
        return self.frames / self.rate if self.rate else 0.0

    @property
    def is_production_format(self) -> bool:
        return (self.rate == TARGET_RATE
                and self.channels == TARGET_CH
                and self.width == TARGET_WIDTH)


def detect_loopback_card() -> Optional[str]:
    """Return the loopback card id we should use, or None if not loaded."""
    try:
        cards = alsaaudio.cards()
    except alsaaudio.ALSAAudioError:
        return None
    if CARD_ID in cards:
        return CARD_ID
    if FALLBACK_CARD in cards:
        return FALLBACK_CARD
    return None


def read_wav_info(path: str) -> WavInfo:
    with wave.open(path, "rb") as w:
        return WavInfo(rate=w.getframerate(), channels=w.getnchannels(),
                       width=w.getsampwidth(), frames=w.getnframes())


# ---------------------------------------------------------------------------
# Feeder: writes the WAV into the loopback playback device on a worker thread.
# Blocking writes pace to the device clock, so no manual sleep loop is needed.
# ---------------------------------------------------------------------------

class Feeder(QObject):
    progress = pyqtSignal(int, int)      # (frames_done, frames_total_per_pass)
    message  = pyqtSignal(str)
    failed   = pyqtSignal(str)
    finished = pyqtSignal()

    def __init__(self, wav_path: str, device: str, rate: int,
                 channels: int, loop: bool):
        super().__init__()
        self._wav_path = wav_path
        self._device   = device
        self._rate     = rate
        self._channels = channels
        self._loop     = loop
        self._stop     = False

    def stop(self) -> None:
        self._stop = True

    def run(self) -> None:
        pcm = None
        try:
            pcm = alsaaudio.PCM(
                type=alsaaudio.PCM_PLAYBACK, mode=alsaaudio.PCM_NORMAL,
                rate=self._rate, channels=self._channels,
                format=alsaaudio.PCM_FORMAT_S16_LE,
                periodsize=PERIOD_FRAMES, device=self._device,
            )
        except alsaaudio.ALSAAudioError as e:
            self.failed.emit(
                f"Could not open {self._device} at {self._rate} Hz:\n  {e}\n\n"
                "If this is a rate error, the loopback is likely capped at "
                "192 kHz and needs rebuilding for 384 kHz.")
            return

        period_bytes = PERIOD_FRAMES * self._channels * TARGET_WIDTH
        self.message.emit(f"Streaming into {self._device} "
                          f"({self._rate} Hz, {self._channels}ch)…")
        try:
            while not self._stop:
                with wave.open(self._wav_path, "rb") as w:
                    total = w.getnframes()
                    done = 0
                    while not self._stop:
                        data = w.readframes(PERIOD_FRAMES)
                        if not data:
                            break
                        if len(data) < period_bytes:        # pad final partial period
                            data = data + b"\x00" * (period_bytes - len(data))
                        pcm.write(data)
                        done += PERIOD_FRAMES
                        self.progress.emit(min(done, total), total)
                if not self._loop:
                    break
        except alsaaudio.ALSAAudioError as e:
            self.failed.emit(f"Playback error:\n  {e}")
            return
        finally:
            if pcm is not None:
                pcm.close()
        self.finished.emit()


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Fake Ultramic — LiteSpectrum capture harness")
        self.resize(640, 420)

        self._card: Optional[str] = None
        self._wav_path: str = ""
        self._thread: Optional[QThread] = None
        self._feeder: Optional[Feeder] = None

        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)

        # Device status
        self.status_lbl = QLabel()
        self.status_lbl.setWordWrap(True)
        self.status_lbl.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
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
        browse.clicked.connect(self.browse)
        file_row.addWidget(self.path_edit)
        file_row.addWidget(browse)
        layout.addLayout(file_row)

        self.wav_lbl = QLabel("No file selected.")
        self.wav_lbl.setWordWrap(True)
        layout.addWidget(self.wav_lbl)

        # Controls
        ctrl = QHBoxLayout()
        self.play_btn = QPushButton("Play")
        self.play_btn.clicked.connect(self.play)
        self.stop_btn = QPushButton("Stop")
        self.stop_btn.clicked.connect(self.stop)
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

        self.refresh_device()

    # -- device ----------------------------------------------------------
    def refresh_device(self) -> None:
        self._card = detect_loopback_card()
        if self._card is None:
            self.status_lbl.setText(
                "<b style='color:#c0392b'>snd-aloop not loaded.</b><br>"
                "Run once (in this session, prefix with <code>!</code>):<br>"
                "<code>sudo modprobe snd-aloop id=UltraMic384K</code>")
            self.play_btn.setEnabled(False)
            return
        cap = CAPTURE_DEV.format(card=self._card)
        note = "" if self._card == CARD_ID else (
            f"  (loaded as '{self._card}', not '{CARD_ID}' — reload with "
            f"id={CARD_ID} for name parity with the Pi)")
        self.status_lbl.setText(
            f"<b style='color:#27ae60'>Loopback ready:</b> card "
            f"'{self._card}'.{note}<br>"
            f"Point the app at:<br><code>./deploy/bin/LiteSpectrum "
            f"--device {cap}</code>")
        self.play_btn.setEnabled(bool(self._wav_path))

    # -- file ------------------------------------------------------------
    def browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Select WAV", str(Path.cwd()), "WAV files (*.wav);;All files (*)")
        if not path:
            return
        self._wav_path = path
        self.path_edit.setText(path)
        try:
            info = read_wav_info(path)
        except (wave.Error, OSError) as e:
            self.wav_lbl.setText(
                f"<span style='color:#c0392b'>Cannot read WAV: {e}</span>")
            self.play_btn.setEnabled(False)
            return
        fmt = (f"{info.rate} Hz, {info.width * 8}-bit, "
               f"{'mono' if info.channels == 1 else f'{info.channels}ch'}, "
               f"{info.duration_s:.2f}s")
        if info.is_production_format:
            self.wav_lbl.setText(
                f"<b>{fmt}</b> — matches the Ultramic format.")
        else:
            self.wav_lbl.setText(
                f"<b>{fmt}</b><br><span style='color:#d35400'>"
                "Warning: not 384 kHz/S16/mono. It will play at its own rate, "
                "but won't exercise the production 384 kHz capture path.</span>")
        self._wav_info = info
        self.play_btn.setEnabled(self._card is not None)

    # -- transport -------------------------------------------------------
    def play(self) -> None:
        if not self._wav_path or self._card is None:
            return
        info = self._wav_info
        device = PLAYBACK_DEV.format(card=self._card)

        self._thread = QThread(self)
        self._feeder = Feeder(self._wav_path, device, info.rate,
                              info.channels, self.loop_chk.isChecked())
        self._feeder.moveToThread(self._thread)
        self._thread.started.connect(self._feeder.run)
        self._feeder.progress.connect(self._on_progress)
        self._feeder.message.connect(self._on_message)
        self._feeder.failed.connect(self._on_failed)
        self._feeder.finished.connect(self._on_finished)
        self._thread.start()

        self.play_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self._log(f"Playing {Path(self._wav_path).name}"
                  f"{' (looping)' if self.loop_chk.isChecked() else ''}")

    def stop(self) -> None:
        if self._feeder is not None:
            self._feeder.stop()
        self.stop_btn.setEnabled(False)
        self._log("Stopping…")

    def _on_progress(self, done: int, total: int) -> None:
        self.progress.setValue(int(done * 100 / total) if total else 0)

    def _on_message(self, msg: str) -> None:
        self._log(msg)

    def _on_failed(self, msg: str) -> None:
        self._log("ERROR: " + msg)
        self._teardown()

    def _on_finished(self) -> None:
        self._log("Done.")
        self._teardown()

    def _teardown(self) -> None:
        if self._thread is not None:
            self._thread.quit()
            self._thread.wait(2000)
            self._thread = None
        self._feeder = None
        self.progress.setValue(0)
        self.play_btn.setEnabled(bool(self._wav_path) and self._card is not None)
        self.stop_btn.setEnabled(False)

    def _log(self, msg: str) -> None:
        self.log.appendPlainText(msg)

    def closeEvent(self, event) -> None:
        self.stop()
        if self._thread is not None:
            self._thread.quit()
            self._thread.wait(2000)
        super().closeEvent(event)


def main() -> None:
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
