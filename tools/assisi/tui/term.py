"""The terminal itself: the only module that changes its mode or reads it.

A session switches to the alternate screen, hides the cursor and has keys
arrive one at a time without echo; leaving it, however that happens, puts the
terminal back exactly as it was. Ctrl+C keeps interrupting, so a bug in the
TUI can always be escaped. Python's curses is not used because Windows has
none: POSIX goes through termios, Windows through msvcrt and the console's
own virtual-terminal mode.
"""

from __future__ import annotations

import contextlib
import os
import sys
import time
from typing import IO, Iterator, List, Optional, Tuple

from assisi.tui import keys

# Switch to the alternate screen and hide the cursor, and the reverse; the
# reverse also resets the style, so nothing the TUI set outlives it.
ENTER = "\x1b[?1049h\x1b[?25l"
LEAVE = "\x1b[0m\x1b[?25h\x1b[?1049l"

# How long to wait for the rest of an escape sequence before taking an ESC to
# be the escape key. Sequences arrive in one write, so this only has to cover
# a slow connection, and stays short enough not to make Esc feel late.
ESCAPE_TIMEOUT_S = 0.05

# The most bytes taken from the terminal in one read: more than any burst of
# keys or a pasted line, and the rest waits for the next read.
READ_CHUNK = 1024

# The size when the terminal will not say, which is the classic default.
FALLBACK_SIZE = (80, 24)

# How often Windows looks for a key, since its console cannot be waited on.
WINDOWS_POLL_S = 0.01

# termios' VSUSP disabled: Ctrl+Z arrives as a key instead of stopping the
# TUI with the terminal left in its mode.
DISABLED_CONTROL_CHARACTER = 0

# The console mode bit that has Windows interpret escape sequences.
ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
STD_OUTPUT_HANDLE = -11


class PosixTerminal:
    """A terminal on Linux or macOS, read from @p fd and written to @p stream."""

    def __init__(self, fd: Optional[int] = None, stream: Optional[IO[str]] = None):
        self.fd = sys.stdin.fileno() if fd is None else fd
        self.stream = sys.stdout if stream is None else stream
        self._pending = b""
        self._ready: List[str] = []
        # The modes outside and inside the open session, as termios lists.
        self._modes: Tuple[list, list] = ([], [])

    def size(self) -> Tuple[int, int]:
        try:
            size = os.get_terminal_size(self.stream.fileno())
        except (OSError, ValueError):
            return FALLBACK_SIZE
        return size.columns, size.lines

    def write(self, text: str) -> None:
        self.stream.write(text)
        self.stream.flush()

    def _wait(self, timeout: float) -> bool:
        import select
        return bool(select.select([self.fd], [], [], timeout)[0])

    def read_key(self, timeout: float) -> Optional[str]:
        """The next key, or None if none comes within @p timeout seconds."""
        if not self._ready and self._wait(timeout):
            self._take()
        return self._ready.pop(0) if self._ready else None

    def _take(self) -> None:
        """Read what has arrived, waiting briefly for the rest of a sequence."""
        self._pending += os.read(self.fd, READ_CHUNK)
        while True:
            names, self._pending = keys.decode(self._pending)
            self._ready.extend(names)
            if not self._pending:
                return
            if not self._wait(ESCAPE_TIMEOUT_S):
                self._ready.extend(keys.flush(self._pending))
                self._pending = b""
                return
            self._pending += os.read(self.fd, READ_CHUNK)

    def _raw_mode(self, saved: list) -> list:
        """@p saved with keys arriving one at a time, unechoed and untranslated,
        and every output translation and signal key kept but Ctrl+Z."""
        import termios
        mode = [list(field) if isinstance(field, list) else field for field in saved]
        mode[0] &= ~(termios.IXON | termios.ICRNL)
        mode[3] &= ~(termios.ECHO | termios.ICANON | termios.IEXTEN)
        mode[6][termios.VMIN] = 1
        mode[6][termios.VTIME] = 0
        mode[6][termios.VSUSP] = DISABLED_CONTROL_CHARACTER
        return mode

    @contextlib.contextmanager
    def session(self) -> Iterator[None]:
        import termios
        saved = termios.tcgetattr(self.fd)
        raw = self._raw_mode(saved)
        termios.tcsetattr(self.fd, termios.TCSAFLUSH, raw)
        self._modes = (saved, raw)
        try:
            self.write(ENTER)
            yield
        finally:
            self.write(LEAVE)
            termios.tcsetattr(self.fd, termios.TCSAFLUSH, saved)

    @contextlib.contextmanager
    def suspended(self) -> Iterator[None]:
        """Inside a session: the terminal as it was before it, for running a
        command with its output going straight to the screen."""
        import termios
        saved, raw = self._modes
        self.write(LEAVE)
        termios.tcsetattr(self.fd, termios.TCSAFLUSH, saved)
        try:
            yield
        finally:
            termios.tcsetattr(self.fd, termios.TCSAFLUSH, raw)
            self.write(ENTER)
            self._pending = b""
            self._ready.clear()


class WindowsTerminal:
    """The Windows console, with escape sequences switched on for a session."""

    def __init__(self):
        import ctypes
        self.kernel32 = ctypes.windll.kernel32
        self.handle = self.kernel32.GetStdHandle(STD_OUTPUT_HANDLE)

    def console_mode(self) -> Optional[int]:
        import ctypes
        mode = ctypes.c_uint32()
        if not self.kernel32.GetConsoleMode(self.handle, ctypes.byref(mode)):
            return None
        return mode.value

    def size(self) -> Tuple[int, int]:
        try:
            size = os.get_terminal_size(sys.stdout.fileno())
        except (OSError, ValueError):
            return FALLBACK_SIZE
        return size.columns, size.lines

    def write(self, text: str) -> None:
        sys.stdout.write(text)
        sys.stdout.flush()

    def read_key(self, timeout: float) -> Optional[str]:
        import msvcrt
        deadline = time.monotonic() + timeout
        while not msvcrt.kbhit():
            if time.monotonic() >= deadline:
                return None
            time.sleep(WINDOWS_POLL_S)
        character = msvcrt.getwch()
        if character in keys.WINDOWS_PREFIXES:
            return keys.windows_key(msvcrt.getwch())
        return keys.char_key(character)

    @contextlib.contextmanager
    def session(self) -> Iterator[None]:
        saved = self.console_mode()
        if saved is not None:
            self.kernel32.SetConsoleMode(self.handle, saved | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        try:
            self.write(ENTER)
            yield
        finally:
            self.write(LEAVE)
            if saved is not None:
                self.kernel32.SetConsoleMode(self.handle, saved)

    @contextlib.contextmanager
    def suspended(self) -> Iterator[None]:
        self.write(LEAVE)
        try:
            yield
        finally:
            self.write(ENTER)


def is_supported() -> bool:
    """Whether this terminal can hold a session: both ends a terminal, and on
    POSIX one that understands escape sequences."""
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        return False
    if os.name == "nt":
        return WindowsTerminal().console_mode() is not None
    return os.environ.get("TERM", "") not in ("", "dumb")


def current():
    return WindowsTerminal() if os.name == "nt" else PosixTerminal()
