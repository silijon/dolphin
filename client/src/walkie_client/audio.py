"""PortAudio owns callbacks; asyncio polls a bounded capture queue every 5 ms.

A non-blocking try-lock prevents callbacks waiting on the event-loop thread.
No network calls, logging, or per-frame event-loop scheduling in callbacks.
"""
import sys
import time
from collections import deque
from contextlib import ExitStack
from dataclasses import dataclass
from threading import Lock
from .jitter import JitterBuffer
from .protocol import FRAME_BYTES, SAMPLE_RATE, SAMPLES_PER_FRAME

TX_MAX_FRAMES = 8
TX_MAX_AGE = 0.16


@dataclass
class AudioStats:
    input_overflows: int = 0
    output_underflows: int = 0
    tx_drops: int = 0
    callback_size_errors: int = 0


class Audio:
    def __init__(self, input_device: int | None = None, output_device: int | None = None) -> None:
        self.input_device, self.output_device = input_device, output_device
        self.jitter = JitterBuffer()
        self.stats = AudioStats()
        self.tx: deque[tuple[int, float, bytes]] = deque(maxlen=TX_MAX_FRAMES)
        self.lock = Lock()
        self.enabled = False
        self.generation = 0
        self.stack = ExitStack()

    def set_transmitting(self, enabled: bool) -> None:
        with self.lock:
            self.enabled = enabled
            self.generation += 1
            self.tx.clear()

    def input_callback(self, data, frames, timing, status) -> None:
        if status.input_overflow:
            self.stats.input_overflows += 1
        if frames != SAMPLES_PER_FRAME or len(data) != FRAME_BYTES:
            self.stats.callback_size_errors += 1
            return
        if not self.lock.acquire(blocking=False):
            self.stats.tx_drops += 1
            return
        try:
            if self.enabled:
                if len(self.tx) == TX_MAX_FRAMES:
                    self.stats.tx_drops += 1
                self.tx.append((self.generation, time.monotonic(), bytes(data)))
        finally:
            self.lock.release()

    def output_callback(self, data, frames, timing, status) -> None:
        if status.output_underflow:
            self.stats.output_underflows += 1
        if frames != SAMPLES_PER_FRAME or len(data) != FRAME_BYTES:
            self.stats.callback_size_errors += 1
            data[:] = bytes(len(data))
            return
        data[:] = self.jitter.pop()

    def take(self) -> bytes | None:
        with self.lock:
            now = time.monotonic()
            while self.tx:
                generation, captured, frame = self.tx.popleft()
                if self.enabled and generation == self.generation and now - captured <= TX_MAX_AGE:
                    return frame
                self.stats.tx_drops += 1
        return None

    def __enter__(self) -> "Audio":
        # Raw int16 is native endian; the supported laptop platforms are LE.
        if sys.byteorder != "little":
            raise RuntimeError("This POC requires a little-endian host for PCM16LE")
        import sounddevice as sd
        try:
            for kind, device, stream, callback in (
                ("input", self.input_device, sd.RawInputStream, self.input_callback),
                ("output", self.output_device, sd.RawOutputStream, self.output_callback),
            ):
                try:
                    self.stack.enter_context(stream(
                        samplerate=SAMPLE_RATE, blocksize=SAMPLES_PER_FRAME,
                        channels=1, dtype="int16", device=device, latency="low",
                        callback=callback))
                except sd.PortAudioError as exc:
                    raise RuntimeError(
                        f"Cannot open {kind} device {device if device is not None else 'default'} "
                        f"at 16000 Hz, mono int16, 320 samples/block: {exc}. "
                        "Use --list-devices and select another device; check permissions/PortAudio. "
                        "The protocol rate will not be changed.") from exc
        except BaseException:
            self.stack.close()
            raise
        return self

    def __exit__(self, *exc):
        self.set_transmitting(False)
        self.jitter.reset()
        self.stack.close()
