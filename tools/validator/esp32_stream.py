"""TCP wire protocol + paced streamer for the ESP32-as-microphone setup.

WIRE FORMAT (little-endian; the canonical spec to implement on the firmware
side):

    offset  size  field
    ------  ----  -----
      0      4    magic            uint32 = 0xBA7DEC70
      4      4    sample_rate      uint32, Hz
      8      2    channels         uint16  (1 = mono; only 1 is supported today)
     10      2    bits_per_sample  uint16  (16 = S16_LE; only 16 is supported today)
     12      4    total_samples    uint32  (declared count of samples to follow;
                                            0 if the sender is open-ended)
     ------------------ 16-byte header ------------------
     16    ...    audio payload    raw int16 LE samples, mono interleaved

The Python side opens a TCP connection to the ESP32, sends the header once,
then streams audio samples in small chunks paced to real-time playback rate
so the ESP32 (which has very limited RAM) never has to buffer more than one
chunk worth of data.

The ESP32 side responsibilities (out of scope for this file, but documented
here so the firmware implementer has a single source of truth):

  1. Open a TCP listening socket on a fixed port (default 4444).
  2. On accept, read exactly 16 bytes; validate magic, sample_rate, channels=1,
     bits_per_sample=16.
  3. Loop: read N bytes into a small ring buffer and forward them on the
     "mic" interface (USB-audio gadget, I2S DAC into a USB-audio bridge,
     whatever the user's wiring uses).
  4. End of stream is signalled by either reaching total_samples (if non-zero)
     or by the Python side closing the TCP connection.
"""
from __future__ import annotations

import socket
import struct
import threading
import time
from typing import Callable, Optional

import numpy as np


# --- protocol constants -----------------------------------------------------

MAGIC: int          = 0xBA7DEC70
DEFAULT_PORT: int   = 4444
HEADER_FORMAT: str  = "<IIHHI"   # magic, sample_rate, channels, bps, total_samples
HEADER_SIZE: int    = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 16, HEADER_SIZE


def pack_header(sample_rate: int,
                total_samples: int,
                channels: int = 1,
                bits_per_sample: int = 16) -> bytes:
    return struct.pack(HEADER_FORMAT, MAGIC, sample_rate,
                       channels, bits_per_sample, total_samples)


# --- streamer ---------------------------------------------------------------

ProgressCb = Callable[[float, float], None]   # (progress_0_to_1, elapsed_seconds)


class ESP32Streamer:
    """Real-time-paced streamer.

    Pacing: each chunk is `chunk_ms` of audio. After sending it we sleep
    until the absolute wall-clock time at which playback of that chunk would
    have finished, so the average send rate matches the sample rate exactly
    regardless of OS scheduling jitter.
    """

    def __init__(self,
                 host: str,
                 port: int = DEFAULT_PORT,
                 chunk_ms: int = 20,
                 connect_timeout_s: float = 5.0):
        self.host = host
        self.port = port
        self.chunk_ms = max(1, int(chunk_ms))
        self.connect_timeout_s = connect_timeout_s
        self._sock: Optional[socket.socket] = None

    # -- connection lifecycle -------------------------------------------

    def connect(self) -> None:
        if self._sock is not None:
            return
        self._sock = socket.create_connection((self.host, self.port),
                                              timeout=self.connect_timeout_s)
        self._sock.settimeout(None)  # blocking sends after connect
        # Disable Nagle so paced small chunks go on the wire promptly.
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def disconnect(self) -> None:
        s, self._sock = self._sock, None
        if s is not None:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()

    def is_connected(self) -> bool:
        return self._sock is not None

    # -- streaming -------------------------------------------------------

    def stream_int16(self,
                     samples: np.ndarray,
                     sample_rate: int,
                     *,
                     on_progress: Optional[ProgressCb] = None,
                     stop_event: Optional[threading.Event] = None,
                     declared_total: Optional[int] = None) -> None:
        """Stream `samples` (1-D int16 array) at real-time playback rate.

        Raises ConnectionError if the socket vanishes mid-stream. The header
        is sent automatically on the first chunk.
        """
        if self._sock is None:
            raise RuntimeError("ESP32Streamer.stream_int16 called before connect()")
        if samples.dtype != np.int16:
            raise TypeError("ESP32Streamer expects int16 samples; got %s" % samples.dtype)
        if samples.ndim != 1:
            raise ValueError("ESP32Streamer expects mono (1-D) samples")
        if sample_rate <= 0:
            raise ValueError("sample_rate must be positive")

        total = int(samples.shape[0])
        declared = int(declared_total) if declared_total is not None else total
        hdr = pack_header(sample_rate, declared)
        self._sock.sendall(hdr)

        chunk_samples = max(1, int(sample_rate * self.chunk_ms / 1000))
        chunk_bytes_view = samples.view()   # zero-copy slicing below

        start_wall = time.monotonic()
        sent = 0
        try:
            while sent < total:
                if stop_event is not None and stop_event.is_set():
                    break
                end = min(sent + chunk_samples, total)
                self._sock.sendall(chunk_bytes_view[sent:end].tobytes())
                sent = end
                if on_progress is not None:
                    on_progress(sent / total, time.monotonic() - start_wall)
                # Real-time pacing: wake at the wall-clock time at which the
                # samples we just sent would have finished playing.
                target = start_wall + sent / sample_rate
                slack = target - time.monotonic()
                if slack > 0:
                    time.sleep(slack)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            self.disconnect()
            raise ConnectionError("ESP32 connection lost mid-stream: %s" % e) from e


# --- convenience: stream a WAV in one call ----------------------------------

def stream_wav_to_esp32(host: str,
                        port: int,
                        wav_path: str,
                        *,
                        chunk_ms: int = 20,
                        on_progress: Optional[ProgressCb] = None,
                        stop_event: Optional[threading.Event] = None) -> None:
    from .dsp import load_wav_int16
    sample_rate, samples = load_wav_int16(wav_path)
    streamer = ESP32Streamer(host=host, port=port, chunk_ms=chunk_ms)
    streamer.connect()
    try:
        streamer.stream_int16(samples, sample_rate,
                              on_progress=on_progress,
                              stop_event=stop_event)
    finally:
        streamer.disconnect()
