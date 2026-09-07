"""Terminal keys map to tiny toggle/quit callbacks; no global keyboard hooks."""
import asyncio
import os
import sys
import termios
import tty
from collections.abc import Callable


class TerminalPTT:
    def __init__(self, toggle: Callable[[], None], quit: Callable[[], None]) -> None:
        self.toggle, self.quit = toggle, quit
        self.saved = None
        self.loop = None

    def __enter__(self) -> "TerminalPTT":
        self.fd = sys.stdin.fileno()
        if not os.isatty(self.fd):
            raise RuntimeError("PTT requires an interactive Unix terminal (SPACE toggles, Q quits)")
        self.saved = termios.tcgetattr(self.fd)
        try:
            # cbreak retains Ctrl-C signal handling and disables echo/canonical input.
            tty.setcbreak(self.fd)
            self.loop = asyncio.get_running_loop()
            self.loop.add_reader(self.fd, self.read)
        except BaseException:
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)
            raise
        return self

    def read(self) -> None:
        data = os.read(self.fd, 32)
        if not data:
            self.quit()
        for key in data:
            if key in (ord('q'), ord('Q')):
                self.quit()
            elif key == ord(' '):
                self.toggle()

    def __exit__(self, *exc):
        try:
            if self.loop:
                self.loop.remove_reader(self.fd)
        finally:
            if self.saved is not None:
                termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)
