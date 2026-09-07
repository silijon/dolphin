"""Small bounded playout buffer. Callback never waits for its short mutex."""
from collections import deque
from threading import Lock
from .protocol import FRAME_BYTES

TARGET_FRAMES = 3
MAX_FRAMES = 8
SILENCE = bytes(FRAME_BYTES)


class JitterBuffer:
    def __init__(self, target: int = TARGET_FRAMES, maximum: int = MAX_FRAMES) -> None:
        if not 1 <= target <= maximum:
            raise ValueError("require 1 <= target <= maximum")
        self.target, self.maximum = target, maximum
        self.frames: deque[bytes] = deque()
        self.lock = Lock()
        self.active = False
        self.playing = False
        self.drops = 0
        self.underruns = 0

    def reset(self, active: bool = False) -> None:
        with self.lock:
            self.active = active
            self.playing = False
            self.frames.clear()

    def push(self, frame: bytes) -> None:
        if len(frame) != FRAME_BYTES:
            raise ValueError("invalid audio frame size")
        with self.lock:
            if not self.active:
                return
            if len(self.frames) >= self.maximum:
                self.frames.popleft()
                self.drops += 1
            self.frames.append(frame)

    def pop(self) -> bytes:
        if not self.lock.acquire(blocking=False):
            return SILENCE
        try:
            if not self.active:
                return SILENCE
            if not self.playing:
                if len(self.frames) < self.target:
                    return SILENCE
                self.playing = True
            if not self.frames:
                self.underruns += 1
                self.playing = False
                return SILENCE
            return self.frames.popleft()
        finally:
            self.lock.release()
